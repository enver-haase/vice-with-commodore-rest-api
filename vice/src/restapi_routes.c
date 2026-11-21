/** \file   restapi_routes.c
 *  \brief  Route table of the Ultimate64 compatible REST API
 *
 *  \author Enver Haase <enver.haase@infraleap.com>
 *
 *  The routes, their parameter names and their JSON keys follow the
 *  1541-Ultimate firmware (software/api/route_*.cc) so that existing clients
 *  work unchanged. Where the hardware has no counterpart in an emulator, the
 *  route is absent rather than faked.
 *
 *  Handlers run on the emulation thread, called once per frame from
 *  restapi_vsync_hook(), and may use the machine API directly.
 *
 *  A note on file names: on the hardware, "file" and "image" name a path in the
 *  device's own filesystem. Here they name a path on the host, resolved like any
 *  other file name VICE is handed. That is why the server listens on localhost
 *  only unless told otherwise.
 */

/*
 * This file is part of VICE, the Versatile Commodore Emulator.
 * See README for copyright notice.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
 *  02111-1307  USA.
 *
 */

#include "vice.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "archdep.h"
#include "attach.h"
#include "autostart.h"
#include "cartridge.h"
#include "cbmdos.h"
#include "diskconstants.h"
#include "diskimage.h"
#include "drive.h"
#include "lib.h"
#include "log.h"
#include "machine.h"
#include "mem.h"
#include "resources.h"
#include "restapi.h"
#include "restapi_http.h"
#include "restapi_routes.h"
#include "restapi_sidplay.h"
#include "ui.h"
#include "util.h"
#include "vdrive-command.h"
#include "vdrive-internal.h"

/** \brief  Version this API implementation reports
 *
 * The firmware reports "0.1" for the v1 API; clients that check it expect that
 * value, so it is not VICE's version number.
 */
#define RESTAPI_API_VERSION "0.1"

typedef void (*route_handler_t)(restapi_request_t *req, restapi_response_t *resp);

typedef struct route_s {
    restapi_method_t method;
    const char *route;
    const char *command;
    route_handler_t handler;
} route_t;

/* ------------------------------------------------------------------------- */
/* shared helpers                                                            */

/** \brief  Resolve the file a runner should act on
 *
 * For PUT that is the \a param query parameter, naming a file on the host; for
 * POST it is the file uploaded in the request body, which has been spooled to a
 * temporary file. Mirrors the PUT/POST pairs in the firmware.
 *
 * \return  file name, or NULL after filling in an error response
 */
static const char *request_file(restapi_request_t *req, restapi_response_t *resp,
                                const char *param)
{
    if (req->method == RESTAPI_METHOD_POST) {
        if (req->upload_count == 0) {
            resp->status = RESTAPI_HTTP_PRECONDITION_FAILED;
            restapi_error(resp, "Expected Body, but got none.");
            return NULL;
        }
        return req->uploads[0];
    } else {
        const char *file = restapi_param(req, param);

        if (file == NULL || *file == '\0') {
            resp->status = RESTAPI_HTTP_BAD_REQUEST;
            restapi_error(resp, "Missing required parameter '%s'.", param);
            return NULL;
        }
        return file;
    }
}

/** \brief  Map a drive name to a VICE unit number
 *
 * The hardware knows the drives as "a" and "b"; unit numbers are accepted as
 * well, since they are what a VICE user thinks in.
 *
 * \return  unit number, or -1 when \a name is not a drive
 */
static int drive_name_to_unit(const char *name)
{
    if (name == NULL || *name == '\0') {
        return -1;
    }
    if (util_strcasecmp(name, "a") == 0) {
        return 8;
    }
    if (util_strcasecmp(name, "b") == 0) {
        return 9;
    }
    if (name[1] == '\0' && name[0] >= '8' && name[0] <= '9') {
        return name[0] - '0';
    }
    if (strcmp(name, "10") == 0) {
        return 10;
    }
    if (strcmp(name, "11") == 0) {
        return 11;
    }
    return -1;
}

/** \brief  Resolve the drive named in the path, or answer with an error
 *
 * \return  unit number, or -1 after filling in an error response
 */
static int request_unit(restapi_request_t *req, restapi_response_t *resp)
{
    const char *drive = restapi_path_part(req, 0);
    int unit = drive_name_to_unit(drive);

    if (unit < 0) {
        resp->status = RESTAPI_HTTP_BAD_REQUEST;
        restapi_error(resp, "Invalid Drive '%s'", (drive != NULL) ? drive : "");
    }
    return unit;
}

/** \brief  Value of hex digit \a c, or -1, as the firmware's chartohex() */
static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/** \brief  Read the start address of a memory call, or answer with an error
 *
 * \return  address, or -1 after filling in an error response
 */
static int request_address(restapi_request_t *req, restapi_response_t *resp)
{
    const char *text = restapi_param(req, "address");
    long value;
    int address;

    if (text == NULL) {
        resp->status = RESTAPI_HTTP_BAD_REQUEST;
        restapi_error(resp, "Missing required parameter 'address'.");
        return -1;
    }
    /* firmware 1.1.0 takes what strtol() makes of it and checks the range
       only, so "12zz" is $0012 and "zz" is $0000; on its 32 bit long an
       overflow saturates, which the clamp reproduces on a 64 bit host */
    value = strtol(text, NULL, 16);
    if (value > 0x7fffffffL) {
        value = 0x7fffffffL;
    } else if (value < -0x7fffffffL - 1) {
        value = -0x7fffffffL - 1;
    }
    address = (int)value;
    if (address < 0 || address > 0xffff) {
        resp->status = RESTAPI_HTTP_BAD_REQUEST;
        restapi_error(resp, "Invalid address");
        return -1;
    }
    return address;
}

/* ------------------------------------------------------------------------- */
/* version, info                                                             */

static void route_version(restapi_request_t *req, restapi_response_t *resp)
{
    restapi_add_string(resp, "version", RESTAPI_API_VERSION);
}

static void route_info(restapi_request_t *req, restapi_response_t *resp)
{
    char *product = lib_msprintf("VICE %s", machine_get_name());

    restapi_add_string(resp, "product", product);
    restapi_add_string(resp, "firmware_version", VERSION);
    restapi_add_string(resp, "hostname", restapi_get_hostname());
    /* not part of the firmware's response, but the honest answer to "what am I
       actually talking to?" */
    restapi_add_string(resp, "emulator", machine_name);

    lib_free(product);
}

/** \brief  GET /v1/help, as firmware 1.1.0 answers it
 *
 * The device answers a fixed placeholder page whatever the command, once its
 * generic parameter check (ArgsURI::Validate() in routes.h) has passed: every
 * parameter must be one the call knows, and "command" is required. "none" in
 * the messages is the call's command name, as the device prints it.
 */
static void route_help(restapi_request_t *req, restapi_response_t *resp)
{
    int i;

    for (i = 0; i < req->param_count; i++) {
        if (strcmp(req->params[i].name, "command") != 0) {
            resp->status = RESTAPI_HTTP_BAD_REQUEST;
            restapi_error(resp, "Function none does not have parameter %s",
                          req->params[i].name);
        }
    }
    if (restapi_param(req, "command") == NULL) {
        resp->status = RESTAPI_HTTP_BAD_REQUEST;
        restapi_error(resp, "Function none requires parameter command");
    }
    if (resp->status == RESTAPI_HTTP_OK) {
        restapi_set_html(resp, "This function provides some help!", "Help text.");
    }
}

/* ------------------------------------------------------------------------- */
/* machine                                                                   */

static void route_machine_reset(restapi_request_t *req, restapi_response_t *resp)
{
    machine_trigger_reset(MACHINE_RESET_MODE_RESET_CPU);
}

static void route_machine_reboot(restapi_request_t *req, restapi_response_t *resp)
{
    machine_trigger_reset(MACHINE_RESET_MODE_POWER_CYCLE);
}

static void route_machine_pause(restapi_request_t *req, restapi_response_t *resp)
{
    if (!ui_pause_active()) {
        ui_pause_enable();
    }
}

static void route_machine_resume(restapi_request_t *req, restapi_response_t *resp)
{
    if (ui_pause_active()) {
        ui_pause_disable();
    }
}

/** \brief  Set by machine:poweroff, read by the server after answering */
static int quit_requested = 0;

int restapi_routes_quit_requested(void)
{
    return quit_requested;
}

/** \brief  Switch the machine off; for an emulator that means quitting */
static void route_machine_poweroff(restapi_request_t *req, restapi_response_t *resp)
{
    quit_requested = 1;
}

/** \brief  Memory bank that sees what the CPU sees
 *
 * The device reads and writes over the cartridge bus, decoded through the
 * bank configuration in force at the time, so $D020 is the VIC register while
 * I/O is mapped in, and a write under a ROM lands in the RAM below it.
 */
static int cpu_bank(void)
{
    int bank = mem_bank_from_name("cpu");

    return (bank >= 0) ? bank : 0;
}

static void route_machine_readmem(restapi_request_t *req, restapi_response_t *resp)
{
    int address = request_address(req, resp);
    int length;
    int bank;
    int i;
    unsigned char *data;

    if (address < 0) {
        return;
    }
    length = restapi_param_int(req, "length", 256);
    if (length < 0 || length > 65536) {
        resp->status = RESTAPI_HTTP_BAD_REQUEST;
        restapi_error(resp, "Invalid length");
        return;
    }
    if (address + length > 65536) {
        resp->status = RESTAPI_HTTP_BAD_REQUEST;
        restapi_error(resp, "Memory read exceeds location $FFFF");
        return;
    }

    /* peek, not read: looking must not acknowledge a CIA interrupt or
       otherwise disturb the program being looked at */
    bank = cpu_bank();
    data = lib_malloc((size_t)length + 1);
    for (i = 0; i < length; i++) {
        data[i] = mem_bank_peek(bank, (uint16_t)(address + i), NULL);
    }
    restapi_set_binary(resp, data, (size_t)length);
}

/** \brief  Write \a length bytes from \a data at \a address, as the CPU would */
static void write_memory(restapi_response_t *resp, int address,
                         const unsigned char *data, int length)
{
    int bank = cpu_bank();
    int i;
    char *range;

    for (i = 0; i < length; i++) {
        mem_bank_write(bank, (uint16_t)(address + i), data[i], NULL);
    }
    range = lib_msprintf("%04x-%04x", (unsigned int)address,
                         (unsigned int)(address + length - 1));
    restapi_add_string(resp, "address", range);
    lib_free(range);
}

static void route_machine_writemem(restapi_request_t *req, restapi_response_t *resp)
{
    int address = request_address(req, resp);
    const char *text;
    unsigned char data[128];
    int length;
    int i;

    if (address < 0) {
        return;
    }
    text = restapi_param(req, "data");
    if (text == NULL) {
        resp->status = RESTAPI_HTTP_BAD_REQUEST;
        restapi_error(resp, "Missing required parameter 'data'.");
        return;
    }

    /* the same checks, in the same order and words, as the firmware */
    length = (int)(strlen(text) >> 1);
    if (length > 128) {
        resp->status = RESTAPI_HTTP_BAD_REQUEST;
        restapi_error(resp, "Maximum length of 128 bytes exceeded. "
                      "Consider using POST method with attachment.");
        return;
    }
    if (length < 1) {
        resp->status = RESTAPI_HTTP_BAD_REQUEST;
        restapi_error(resp, "Use this API call to write at least one byte!");
        return;
    }
    if (address + length > 65536) {
        resp->status = RESTAPI_HTTP_BAD_REQUEST;
        restapi_error(resp, "Memory write exceeds location $FFFF");
        return;
    }
    for (i = 0; i < 2 * length; i++) {
        int nibble = hex_digit(text[i]);

        if (nibble < 0) {
            resp->status = RESTAPI_HTTP_BAD_REQUEST;
            restapi_error(resp, "Invalid char '%c' at position %d.", text[i], i);
            return;
        }
        if ((i & 1) == 0) {
            data[i >> 1] = (unsigned char)(nibble << 4);
        } else {
            data[i >> 1] |= (unsigned char)nibble;
        }
    }

    write_memory(resp, address, data, length);
}

static void route_machine_writemem_post(restapi_request_t *req, restapi_response_t *resp)
{
    int address = request_address(req, resp);
    const char *file;
    unsigned char *data;
    size_t length;
    FILE *fd;

    if (address < 0) {
        return;
    }
    file = request_file(req, resp, "file");
    if (file == NULL) {
        return;
    }
    fd = fopen(file, MODE_READ);
    if (fd == NULL) {
        resp->status = RESTAPI_HTTP_NOT_FOUND;
        restapi_error(resp, "Could not read data from attachment");
        return;
    }
    /* one byte more than fits, so an oversized upload shows as one */
    data = lib_malloc(65536 + 1);
    length = fread(data, 1, 65536 + 1, fd);
    fclose(fd);
    if ((size_t)address + length > 65536) {
        resp->status = RESTAPI_HTTP_BAD_REQUEST;
        restapi_error(resp, "Memory write exceeds location $FFFF");
        lib_free(data);
        return;
    }
    if (length > 0) {
        write_memory(resp, address, data, (int)length);
    }
    lib_free(data);
}

/* ------------------------------------------------------------------------- */
/* runners                                                                   */

/** \brief  The uploaded file the last runner call was given
 *
 * autostart_autodetect() only schedules the load, and for a disk or tape image
 * it attaches the image for the rest of the session (autostart_disk() calls
 * file_system_attach_disk()). So an uploaded file has to outlive its request
 * here just as a mounted image does; it is dropped when the next upload
 * arrives or on shutdown.
 */
static char *runner_upload = NULL;

static void release_runner_upload(void)
{
    if (runner_upload != NULL) {
        archdep_remove(runner_upload);
        lib_free(runner_upload);
        runner_upload = NULL;
    }
}

/** \brief  Keep \a filename until the next runner upload; takes ownership */
static void keep_runner_upload(char *filename)
{
    release_runner_upload();
    runner_upload = filename;
}

/** \brief  The uploaded cartridge the last run_crt call was given */
static char *cart_upload = NULL;

static void release_cart_upload(void)
{
    if (cart_upload != NULL) {
        archdep_remove(cart_upload);
        lib_free(cart_upload);
        cart_upload = NULL;
    }
}

/** \brief  Keep \a filename until the next cartridge upload; takes ownership */
static void keep_cart_upload(char *filename)
{
    release_cart_upload();
    cart_upload = filename;
}

static void autostart_file(restapi_request_t *req, restapi_response_t *resp,
                           unsigned int mode)
{
    const char *file = request_file(req, resp, "file");

    if (file == NULL) {
        return;
    }
    if (autostart_autodetect(file, NULL, 0, mode) < 0) {
        resp->status = RESTAPI_HTTP_BAD_REQUEST;
        restapi_error(resp, "Could not start '%s'.", file);
        return;
    }
    if (req->method == RESTAPI_METHOD_POST) {
        keep_runner_upload(restapi_take_upload(req, 0));
    }
}

static void route_runners_run_prg(restapi_request_t *req, restapi_response_t *resp)
{
    autostart_file(req, resp, AUTOSTART_MODE_RUN);
}

static void route_runners_load_prg(restapi_request_t *req, restapi_response_t *resp)
{
    autostart_file(req, resp, AUTOSTART_MODE_LOAD);
}

static void route_runners_run_crt(restapi_request_t *req, restapi_response_t *resp)
{
    const char *file = request_file(req, resp, "file");
    int cartridge_reset = 0;

    if (file == NULL) {
        return;
    }
    if (cartridge_attach_image(CARTRIDGE_CRT, file) < 0) {
        resp->status = RESTAPI_HTTP_BAD_REQUEST;
        restapi_error(resp, "Could not attach '%s'.", file);
        return;
    }
    if (req->method == RESTAPI_METHOD_POST) {
        /* the ROM is in memory now, but VICE remembers the file name, so the
           upload is kept rather than leaving that name pointing at nothing */
        keep_cart_upload(restapi_take_upload(req, 0));
    }
    /* attaching only starts the cartridge when CartridgeReset is on, but
       run_crt promises to start it */
    if (resources_get_int("CartridgeReset", &cartridge_reset) < 0
        || cartridge_reset == 0) {
        machine_trigger_reset(MACHINE_RESET_MODE_POWER_CYCLE);
    }
}

/* ------------------------------------------------------------------------- */
/* drives                                                                    */

/** \brief  Uploaded images still mounted, one slot per unit 8-11
 *
 * A disk image is read for as long as it stays attached, so an uploaded image
 * must outlive the request that brought it in. Its temporary file is kept until
 * the unit gets something else mounted, is emptied, or the emulator exits.
 */
static char *mounted_uploads[4];

/** \brief  Drop the uploaded image kept for \a unit, if any */
static void release_mounted_upload(int unit)
{
    int slot = unit - 8;

    if (slot < 0 || slot >= 4 || mounted_uploads[slot] == NULL) {
        return;
    }
    archdep_remove(mounted_uploads[slot]);
    lib_free(mounted_uploads[slot]);
    mounted_uploads[slot] = NULL;
}

/** \brief  Keep \a filename for \a unit, replacing what was kept before
 *
 * Takes ownership of \a filename.
 */
static void keep_mounted_upload(int unit, char *filename)
{
    int slot = unit - 8;

    release_mounted_upload(unit);
    if (slot < 0 || slot >= 4) {
        archdep_remove(filename);
        lib_free(filename);
        return;
    }
    mounted_uploads[slot] = filename;
}

static void restore_drive_roms(void);

void restapi_routes_shutdown(void)
{
    int unit;

    for (unit = 8; unit <= 11; unit++) {
        release_mounted_upload(unit);
    }
    release_runner_upload();
    release_cart_upload();
    restore_drive_roms();
    restapi_sidplay_shutdown();
}

/** \brief  Drive type as the API's vocabulary knows it
 *
 * The API speaks of 1541, 1571 and 1581, so VICE's finer distinctions are
 * folded into those three. A drive with no counterpart keeps its VICE type
 * number: a client that does not recognise it is better off than one that has
 * been told a 4000 is a 1541.
 */
static char *drive_type_name(int drive_type)
{
    switch (drive_type) {
        case DRIVE_TYPE_NONE:
            return lib_strdup("");
        case DRIVE_TYPE_1540:       /* FALL THROUGH */
        case DRIVE_TYPE_1541:       /* FALL THROUGH */
        case DRIVE_TYPE_1541II:     /* FALL THROUGH */
        case DRIVE_TYPE_1551:
            return lib_strdup("1541");
        case DRIVE_TYPE_1570:       /* FALL THROUGH */
        case DRIVE_TYPE_1571:       /* FALL THROUGH */
        case DRIVE_TYPE_1571CR:
            return lib_strdup("1571");
        case DRIVE_TYPE_1581:
            return lib_strdup("1581");
        default:
            return lib_msprintf("%d", drive_type);
    }
}

/** \brief  Name of the DOS ROM resource for \a drive_type, or NULL */
static const char *drive_rom_resource(int drive_type)
{
    switch (drive_type) {
        case DRIVE_TYPE_1540:   return "DosName1540";
        case DRIVE_TYPE_1541:   return "DosName1541";
        case DRIVE_TYPE_1541II: return "DosName1541ii";
        case DRIVE_TYPE_1551:   return "DosName1551";
        case DRIVE_TYPE_1570:   return "DosName1570";
        case DRIVE_TYPE_1571:   return "DosName1571";
        case DRIVE_TYPE_1571CR: return "DosName1571cr";
        case DRIVE_TYPE_1581:   return "DosName1581";
        case DRIVE_TYPE_2000:   return "DosName2000";
        case DRIVE_TYPE_4000:   return "DosName4000";
        default:                return NULL;
    }
}

/** \brief  Append the JSON description of one drive to \a json */
static char *append_drive_info(char *json, unsigned int unit, const char *letter)
{
    const char *image = file_system_get_disk_name(unit, 0);
    char *directory = NULL;
    char *name = NULL;
    char *type_name;
    char *entry;
    char *result;
    int drive_type = 0;
    const char *rom_resource;
    const char *rom = NULL;
    char *rom_json;
    char *file_json;
    char *path_json;

    resources_get_int_sprintf("Drive%dType", &drive_type, (int)unit);
    if (image != NULL) {
        util_fname_split(image, &directory, &name);
        /* clients join image_path and image_file as they come, so the path
           has to end in a separator */
        if (*directory != '\0'
                && directory[strlen(directory) - 1] != ARCHDEP_DIR_SEP_CHR) {
            char *with_sep = util_concat(directory, ARCHDEP_DIR_SEP_STR, NULL);
            lib_free(directory);
            directory = with_sep;
        }
    }
    type_name = drive_type_name(drive_type);
    rom_resource = drive_rom_resource(drive_type);
    if (rom_resource != NULL) {
        resources_get_string(rom_resource, &rom);
    }

    /* these three are file names, so they go through the JSON escaper: a
       Windows path is full of backslashes, and a file name may contain a quote */
    rom_json = restapi_json_quote(rom);
    file_json = restapi_json_quote(name);
    path_json = restapi_json_quote(directory);

    entry = lib_msprintf("{\"%s\":{\"enabled\":%s,\"bus_id\":%u,\"type\":\"%s\","
                         "\"rom\":%s,\"image_file\":%s,\"image_path\":%s}}",
                         letter,
                         (drive_type != DRIVE_TYPE_NONE) ? "true" : "false",
                         unit,
                         type_name,
                         rom_json,
                         file_json,
                         path_json);

    if (json == NULL) {
        result = lib_strdup(entry);
    } else {
        result = util_concat(json, ",", entry, NULL);
        lib_free(json);
    }

    lib_free(entry);
    lib_free(rom_json);
    lib_free(file_json);
    lib_free(path_json);
    lib_free(type_name);
    lib_free(directory);
    lib_free(name);
    return result;
}

static void route_drives_list(restapi_request_t *req, restapi_response_t *resp)
{
    char *json = NULL;
    char *array;

    json = append_drive_info(json, 8, "a");
    json = append_drive_info(json, 9, "b");

    array = lib_msprintf("[%s]", (json != NULL) ? json : "");
    restapi_add_raw(resp, "drives", array);

    lib_free(array);
    lib_free(json);
}

static void route_drives_mount(restapi_request_t *req, restapi_response_t *resp)
{
    const char *drive = restapi_path_part(req, 0);
    int unit = drive_name_to_unit(drive);
    const char *mode = restapi_param(req, "mode");
    const char *image;
    int readonly;

    if (unit < 0) {
        resp->status = RESTAPI_HTTP_BAD_REQUEST;
        restapi_error(resp, "Invalid Drive '%s'", (drive != NULL) ? drive : "");
        return;
    }
    if (mode == NULL || *mode == '\0') {
        /* the hardware treats a missing mode as read/write; here the unit may
           already have been set read only by its user, and quietly clearing
           that is worse than leaving the choice alone */
        readonly = -1;
    } else if (util_strcasecmp(mode, "readwrite") == 0) {
        readonly = 0;
    } else if (util_strcasecmp(mode, "readonly") == 0) {
        readonly = 1;
    } else {
        /* "unlinked" keeps writes in the device's RAM, which has no equivalent
           here; saying so beats silently writing to the user's image */
        resp->status = RESTAPI_HTTP_BAD_REQUEST;
        restapi_error(resp, "Unsupported mode '%s'", mode);
        return;
    }

    image = request_file(req, resp, "image");
    if (image == NULL) {
        return;
    }

    if (readonly >= 0
        && resources_set_int_sprintf("AttachDevice%dd0Readonly",
                                     readonly, unit) < 0) {
        resp->status = RESTAPI_HTTP_INTERNAL_ERROR;
        restapi_error(resp, "Could not set mode '%s' on drive %d.", mode, unit);
        return;
    }

    if (file_system_attach_disk((unsigned int)unit, 0, image) < 0) {
        resp->status = RESTAPI_HTTP_BAD_REQUEST;
        restapi_error(resp, "Could not mount '%s' on drive %d.", image, unit);
        return;
    }

    if (req->method == RESTAPI_METHOD_POST) {
        /* the image came in with the request and is now in use: keep its
           temporary file alive for as long as it stays mounted */
        keep_mounted_upload(unit, restapi_take_upload(req, 0));
    } else {
        release_mounted_upload(unit);
    }
}

static void route_drives_remove(restapi_request_t *req, restapi_response_t *resp)
{
    const char *drive = restapi_path_part(req, 0);
    int unit = drive_name_to_unit(drive);

    if (unit < 0) {
        resp->status = RESTAPI_HTTP_BAD_REQUEST;
        restapi_error(resp, "Invalid Drive '%s'", (drive != NULL) ? drive : "");
        return;
    }
    file_system_detach_disk((unsigned int)unit, 0);
    release_mounted_upload(unit);
}

static void route_drives_reset(restapi_request_t *req, restapi_response_t *resp)
{
    const char *drive = restapi_path_part(req, 0);
    int unit = drive_name_to_unit(drive);

    if (unit < 0) {
        resp->status = RESTAPI_HTTP_BAD_REQUEST;
        restapi_error(resp, "Invalid Drive '%s'", (drive != NULL) ? drive : "");
        return;
    }
    drive_cpu_trigger_reset((unsigned int)(unit - 8));
}

/** \brief  Drive type each unit had before drives:off, for drives:on
 *
 * The device can switch a drive off and on again with its type intact. VICE
 * has no power switch for a drive; its type NONE is the nearest thing, so the
 * type is remembered here while the drive is off.
 */
static int type_before_off[4];

static int get_drive_type(int unit)
{
    int drive_type = DRIVE_TYPE_NONE;

    resources_get_int_sprintf("Drive%dType", &drive_type, unit);
    return drive_type;
}

static void set_drive_type(restapi_response_t *resp, int unit, int drive_type)
{
    if (resources_set_int_sprintf("Drive%dType", drive_type, unit) < 0) {
        resp->status = RESTAPI_HTTP_INTERNAL_ERROR;
        restapi_error(resp, "Could not set drive %d to type %d.", unit, drive_type);
    }
}

static void route_drives_on(restapi_request_t *req, restapi_response_t *resp)
{
    int unit = request_unit(req, resp);
    int drive_type;

    if (unit < 0 || get_drive_type(unit) != DRIVE_TYPE_NONE) {
        return;
    }
    drive_type = type_before_off[unit - 8];
    set_drive_type(resp, unit,
                   (drive_type != DRIVE_TYPE_NONE) ? drive_type : DRIVE_TYPE_1541);
}

static void route_drives_off(restapi_request_t *req, restapi_response_t *resp)
{
    int unit = request_unit(req, resp);
    int drive_type;

    if (unit < 0) {
        return;
    }
    drive_type = get_drive_type(unit);
    if (drive_type == DRIVE_TYPE_NONE) {
        return;
    }
    type_before_off[unit - 8] = drive_type;
    set_drive_type(resp, unit, DRIVE_TYPE_NONE);
}

static void route_drives_set_mode(restapi_request_t *req, restapi_response_t *resp)
{
    int unit = request_unit(req, resp);
    const char *mode;
    int drive_type;
    char *current;

    if (unit < 0) {
        return;
    }
    mode = restapi_param(req, "mode");
    if (mode == NULL) {
        resp->status = RESTAPI_HTTP_BAD_REQUEST;
        restapi_error(resp, "Missing required parameter 'mode'.");
        return;
    }
    if (strcmp(mode, "1541") == 0) {
        drive_type = DRIVE_TYPE_1541;
    } else if (strcmp(mode, "1571") == 0) {
        drive_type = DRIVE_TYPE_1571;
    } else if (strcmp(mode, "1581") == 0) {
        drive_type = DRIVE_TYPE_1581;
    } else {
        resp->status = RESTAPI_HTTP_BAD_REQUEST;
        restapi_error(resp, "Invalid Drive Type '%s'", mode);
        return;
    }
    restapi_add_string(resp, "mode", mode);

    if (get_drive_type(unit) == DRIVE_TYPE_NONE) {
        /* the device changes the mode of a drive that is off without switching
           it on; it takes effect with the next drives:on */
        type_before_off[unit - 8] = drive_type;
        return;
    }
    /* a 1541-II already is a 1541 in the API's terms; leave it be */
    current = drive_type_name(get_drive_type(unit));
    if (strcmp(current, mode) != 0) {
        set_drive_type(resp, unit, drive_type);
    }
    lib_free(current);
}

/** \brief  A drive ROM resource drives:load_rom changed, and its old value */
typedef struct rom_override_s {
    const char *resource;
    char *original;     /* value to restore on shutdown */
    char *upload;       /* temp file kept while the ROM may be reread, or NULL */
} rom_override_t;

static rom_override_t rom_overrides[10];

/** \brief  Find or create the override record for \a resource */
static rom_override_t *rom_override(const char *resource)
{
    size_t i;
    const char *original = NULL;

    for (i = 0; i < sizeof(rom_overrides) / sizeof(rom_overrides[0]); i++) {
        if (rom_overrides[i].resource == NULL) {
            resources_get_string(resource, &original);
            rom_overrides[i].resource = resource;
            rom_overrides[i].original = lib_strdup((original != NULL) ? original : "");
            return &rom_overrides[i];
        }
        if (strcmp(rom_overrides[i].resource, resource) == 0) {
            return &rom_overrides[i];
        }
    }
    return NULL;
}

/** \brief  Put back the ROMs drives:load_rom replaced
 *
 * Their resources would otherwise be saved with the settings and point at a
 * temporary file that is deleted on exit.
 */
static void restore_drive_roms(void)
{
    size_t i;

    for (i = 0; i < sizeof(rom_overrides) / sizeof(rom_overrides[0]); i++) {
        rom_override_t *o = &rom_overrides[i];

        if (o->resource == NULL) {
            continue;
        }
        resources_set_string(o->resource, o->original);
        if (o->upload != NULL) {
            archdep_remove(o->upload);
            lib_free(o->upload);
        }
        lib_free(o->original);
        memset(o, 0, sizeof(*o));
    }
}

/** \brief  Whether a ROM of \a size bytes fits a drive of \a drive_type
 *
 * The device takes 16K or 32K. VICE wants 32K for the 1570, 1571 and 1581, and
 * would otherwise reject the ROM silently while reloading it.
 */
static int drive_rom_size_ok(int drive_type, long size)
{
    switch (drive_type) {
        case DRIVE_TYPE_1570:       /* FALL THROUGH */
        case DRIVE_TYPE_1571:       /* FALL THROUGH */
        case DRIVE_TYPE_1571CR:     /* FALL THROUGH */
        case DRIVE_TYPE_1581:
            return size == 0x8000;
        default:
            return size == 0x4000 || size == 0x8000;
    }
}

static void route_drives_load_rom(restapi_request_t *req, restapi_response_t *resp)
{
    int unit = request_unit(req, resp);
    int drive_type;
    const char *resource;
    const char *file;
    rom_override_t *override;
    FILE *fd;
    long size;

    if (unit < 0) {
        return;
    }
    drive_type = get_drive_type(unit);
    resource = drive_rom_resource(drive_type);
    if (resource == NULL) {
        resp->status = RESTAPI_HTTP_UNSUPPORTED_MEDIA_TYPE;
        restapi_error(resp, "Drive is in the wrong mode");
        return;
    }
    file = request_file(req, resp, "file");
    if (file == NULL) {
        return;
    }

    fd = fopen(file, MODE_READ);
    if (fd == NULL) {
        resp->status = RESTAPI_HTTP_NOT_FOUND;
        restapi_error(resp, "Cannot open file");
        return;
    }
    size = (long)archdep_file_size(fd);
    fclose(fd);
    if (!drive_rom_size_ok(drive_type, size)) {
        resp->status = RESTAPI_HTTP_PRECONDITION_FAILED;
        restapi_error(resp, "Drive ROM is invalid");
        return;
    }

    override = rom_override(resource);
    if (override == NULL || resources_set_string(resource, file) < 0) {
        resp->status = RESTAPI_HTTP_PRECONDITION_FAILED;
        restapi_error(resp, "Drive ROM is invalid");
        return;
    }
    /* VICE reads the ROM again whenever a drive changes type, so an uploaded
       one is kept until it is replaced or the emulator exits */
    if (override->upload != NULL) {
        archdep_remove(override->upload);
        lib_free(override->upload);
        override->upload = NULL;
    }
    if (req->method == RESTAPI_METHOD_POST) {
        override->upload = restapi_take_upload(req, 0);
    }
}

/* ------------------------------------------------------------------------- */
/* files                                                                     */

/** \brief  The disk name the device gives an image created without one
 *
 * Its file name without the extension, as enforce_diskname() does.
 */
static char *default_disk_name(const char *path)
{
    char *name = NULL;
    char *dot;

    util_fname_split(path, NULL, &name);
    dot = strrchr(name, '.');
    if (dot != NULL && dot != name) {
        *dot = '\0';
    }
    return name;
}

/** \brief  The file named by the request path, as the absolute path it is
 *
 * The path arrives split at its slashes, the way the firmware's
 * args.get_full_path() puts it back together: /v1/files/tmp/x.d64 names
 * /tmp/x.d64.
 *
 * \return  heap string, or NULL when the request names no file
 */
static char *request_full_path(restapi_request_t *req)
{
    char *path = NULL;
    int i;

    for (i = 0; i < req->path_count; i++) {
        char *joined = util_concat((path != NULL) ? path : "", "/",
                                   req->path_parts[i], NULL);
        lib_free(path);
        path = joined;
    }
    if (path != NULL && path[strlen(path) - 1] == '/') {
        lib_free(path);
        return NULL;
    }
    return path;
}

/** \brief  Size of a 40 track D64 without error info: 683 + 5 * 17 sectors */
#define D64_FILE_SIZE_40_TRACKS ((683 + 5 * 17) * 256)

/** \brief  Create and format a 40 track D64
 *
 * vdrive_internal_create_format_disk_image() makes 35 track images only, so
 * the empty image is written here and then formatted as it would format one.
 *
 * \return  0 on success, -1 on failure
 */
static int create_d64_40(const char *path, const char *diskname)
{
    FILE *fd;
    unsigned char *zeros;
    size_t written;
    struct vdrive_s *vdrive;
    int status = 0;

    fd = fopen(path, MODE_WRITE);
    if (fd == NULL) {
        return -1;
    }
    zeros = lib_calloc(1, D64_FILE_SIZE_40_TRACKS);
    written = fwrite(zeros, 1, D64_FILE_SIZE_40_TRACKS, fd);
    lib_free(zeros);
    if (fclose(fd) != 0 || written != D64_FILE_SIZE_40_TRACKS) {
        return -1;
    }

    vdrive = vdrive_internal_open_fsimage(path, 0);
    if (vdrive == NULL) {
        return -1;
    }
    if (vdrive_command_format(vdrive, diskname) != CBMDOS_IPE_OK) {
        status = -1;
    }
    if (vdrive_internal_close_disk_image(vdrive) < 0) {
        status = -1;
    }
    return status;
}

/** \brief  Create and format the image named by the request path
 *
 * \param[in]   type    disk image type
 * \param[in]   tracks  number of tracks, or 0 for the type's only size
 * \param[in]   size    file size in bytes, for the response
 */
static void create_image(restapi_request_t *req, restapi_response_t *resp,
                         unsigned int type, int tracks, long size)
{
    char *path = request_full_path(req);
    const char *given = restapi_param(req, "diskname");
    char *diskname;
    char *upper;
    int status;
    size_t i;

    if (path == NULL) {
        resp->status = RESTAPI_HTTP_BAD_REQUEST;
        restapi_error(resp, "No file name given.");
        return;
    }
    restapi_add_string(resp, "path", path);
    if (tracks > 0) {
        restapi_add_int(resp, "tracks", tracks);
    }
    diskname = (given != NULL) ? lib_strdup(given) : default_disk_name(path);
    restapi_add_string(resp, "diskname", diskname);

    /* the device overwrites what is there; on a host that may be anything the
       user owns, so an existing file is left alone */
    if (util_file_exists(path)) {
        resp->status = RESTAPI_HTTP_BAD_REQUEST;
        restapi_error(resp, "File exists: '%s'", path);
        lib_free(diskname);
        lib_free(path);
        return;
    }

    /* DOS takes names in PETSCII, whose capitals are ASCII's; a ",ID" suffix
       sets the disk ID, as on the device */
    upper = lib_strdup(diskname);
    for (i = 0; upper[i] != '\0'; i++) {
        upper[i] = util_toupper(upper[i]);
    }

    if (type == DISK_IMAGE_TYPE_D64 && tracks == EXT_TRACKS_1541) {
        status = create_d64_40(path, upper);
    } else {
        status = vdrive_internal_create_format_disk_image(path, upper, type);
    }
    if (status < 0) {
        resp->status = RESTAPI_HTTP_INTERNAL_ERROR;
        restapi_error(resp, "Could not create '%s'.", path);
        archdep_remove(path);
    } else {
        restapi_add_int(resp, "bytes_written", (int)size);
    }
    lib_free(upper);
    lib_free(diskname);
    lib_free(path);
}

static void route_files_create_d64(restapi_request_t *req, restapi_response_t *resp)
{
    int tracks = restapi_param_int(req, "tracks", NUM_TRACKS_1541);

    /* the device takes 35 to 41 tracks; VICE knows D64 images of 35 and 40 */
    if (tracks != NUM_TRACKS_1541 && tracks != EXT_TRACKS_1541) {
        resp->status = RESTAPI_HTTP_BAD_REQUEST;
        restapi_add_int(resp, "tracks", tracks);
        restapi_error(resp, "Track count should be 35 or 40.");
        return;
    }
    create_image(req, resp, DISK_IMAGE_TYPE_D64, tracks,
                 (tracks == EXT_TRACKS_1541) ? D64_FILE_SIZE_40_TRACKS
                                             : D64_FILE_SIZE_35);
}

static void route_files_create_d71(restapi_request_t *req, restapi_response_t *resp)
{
    create_image(req, resp, DISK_IMAGE_TYPE_D71, 70, D71_FILE_SIZE);
}

static void route_files_create_d81(restapi_request_t *req, restapi_response_t *resp)
{
    create_image(req, resp, DISK_IMAGE_TYPE_D81, 0, D81_FILE_SIZE);
}

/* ------------------------------------------------------------------------- */
/* dispatch                                                                  */

static const route_t routes[] = {
    { RESTAPI_METHOD_GET,  "version", "none",   route_version },
    { RESTAPI_METHOD_GET,  "info",    "none",   route_info },
    { RESTAPI_METHOD_GET,  "help",    "none",   route_help },

    { RESTAPI_METHOD_PUT,  "machine", "reset",  route_machine_reset },
    { RESTAPI_METHOD_PUT,  "machine", "reboot", route_machine_reboot },
    { RESTAPI_METHOD_PUT,  "machine", "pause",  route_machine_pause },
    { RESTAPI_METHOD_PUT,  "machine", "resume", route_machine_resume },
    { RESTAPI_METHOD_PUT,  "machine", "poweroff", route_machine_poweroff },
    { RESTAPI_METHOD_GET,  "machine", "readmem",  route_machine_readmem },
    { RESTAPI_METHOD_PUT,  "machine", "writemem", route_machine_writemem },
    { RESTAPI_METHOD_POST, "machine", "writemem", route_machine_writemem_post },

    { RESTAPI_METHOD_PUT,  "runners", "run_prg",  route_runners_run_prg },
    { RESTAPI_METHOD_POST, "runners", "run_prg",  route_runners_run_prg },
    { RESTAPI_METHOD_PUT,  "runners", "load_prg", route_runners_load_prg },
    { RESTAPI_METHOD_POST, "runners", "load_prg", route_runners_load_prg },
    { RESTAPI_METHOD_PUT,  "runners", "run_crt",  route_runners_run_crt },
    { RESTAPI_METHOD_POST, "runners", "run_crt",  route_runners_run_crt },
    { RESTAPI_METHOD_PUT,  "runners", "sidplay",  restapi_sidplay },
    { RESTAPI_METHOD_POST, "runners", "sidplay",  restapi_sidplay },

    { RESTAPI_METHOD_GET,  "drives",  "none",   route_drives_list },
    { RESTAPI_METHOD_PUT,  "drives",  "mount",  route_drives_mount },
    { RESTAPI_METHOD_POST, "drives",  "mount",  route_drives_mount },
    { RESTAPI_METHOD_PUT,  "drives",  "remove", route_drives_remove },
    { RESTAPI_METHOD_PUT,  "drives",  "reset",  route_drives_reset },
    { RESTAPI_METHOD_PUT,  "drives",  "on",     route_drives_on },
    { RESTAPI_METHOD_PUT,  "drives",  "off",    route_drives_off },
    { RESTAPI_METHOD_PUT,  "drives",  "set_mode", route_drives_set_mode },
    { RESTAPI_METHOD_PUT,  "drives",  "load_rom", route_drives_load_rom },
    { RESTAPI_METHOD_POST, "drives",  "load_rom", route_drives_load_rom },

    { RESTAPI_METHOD_PUT,  "files",   "create_d64", route_files_create_d64 },
    { RESTAPI_METHOD_PUT,  "files",   "create_d71", route_files_create_d71 },
    { RESTAPI_METHOD_PUT,  "files",   "create_d81", route_files_create_d81 },

    { RESTAPI_METHOD_UNKNOWN, NULL, NULL, NULL }
};

void restapi_routes_dispatch(restapi_request_t *req, restapi_response_t *resp)
{
    const route_t *route;
    int route_exists = 0;

    for (route = routes; route->route != NULL; route++) {
        if (strcmp(route->route, req->route) != 0) {
            continue;
        }
        route_exists = 1;
        if (route->method == req->method
            && strcmp(route->command, req->command) == 0) {
            route->handler(req, resp);
            return;
        }
    }

    resp->status = RESTAPI_HTTP_NOT_FOUND;
    if (route_exists) {
        restapi_error(resp, "Unknown command '%s' on '%s'.",
                      req->command, req->route);
    } else {
        restapi_error(resp, "Unknown route '%s'.", req->route);
    }
}
