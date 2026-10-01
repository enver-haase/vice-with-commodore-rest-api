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
#include "drive.h"
#include "lib.h"
#include "log.h"
#include "machine.h"
#include "resources.h"
#include "restapi.h"
#include "restapi_http.h"
#include "restapi_routes.h"
#include "ui.h"
#include "util.h"

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

void restapi_routes_shutdown(void)
{
    int unit;

    for (unit = 8; unit <= 11; unit++) {
        release_mounted_upload(unit);
    }
    release_runner_upload();
    release_cart_upload();
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

/* ------------------------------------------------------------------------- */
/* dispatch                                                                  */

static const route_t routes[] = {
    { RESTAPI_METHOD_GET,  "version", "none",   route_version },
    { RESTAPI_METHOD_GET,  "info",    "none",   route_info },

    { RESTAPI_METHOD_PUT,  "machine", "reset",  route_machine_reset },
    { RESTAPI_METHOD_PUT,  "machine", "reboot", route_machine_reboot },
    { RESTAPI_METHOD_PUT,  "machine", "pause",  route_machine_pause },
    { RESTAPI_METHOD_PUT,  "machine", "resume", route_machine_resume },

    { RESTAPI_METHOD_PUT,  "runners", "run_prg",  route_runners_run_prg },
    { RESTAPI_METHOD_POST, "runners", "run_prg",  route_runners_run_prg },
    { RESTAPI_METHOD_PUT,  "runners", "load_prg", route_runners_load_prg },
    { RESTAPI_METHOD_POST, "runners", "load_prg", route_runners_load_prg },
    { RESTAPI_METHOD_PUT,  "runners", "run_crt",  route_runners_run_crt },
    { RESTAPI_METHOD_POST, "runners", "run_crt",  route_runners_run_crt },

    { RESTAPI_METHOD_GET,  "drives",  "none",   route_drives_list },
    { RESTAPI_METHOD_PUT,  "drives",  "mount",  route_drives_mount },
    { RESTAPI_METHOD_POST, "drives",  "mount",  route_drives_mount },
    { RESTAPI_METHOD_PUT,  "drives",  "remove", route_drives_remove },
    { RESTAPI_METHOD_PUT,  "drives",  "reset",  route_drives_reset },

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
