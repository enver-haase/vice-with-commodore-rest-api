/** \file   restapi_sidplay.c
 *  \brief  runners:sidplay of the Ultimate64 compatible REST API
 *
 *  \author Enver Haase <enver.haase@infraleap.com>
 *
 *  Plays a SID file the way Commodore's firmware 1.1.0 for the C64 Ultimate
 *  does (software/filetypes/filetype_sid.cc there): it does not emulate a
 *  player, it plugs the device's own SID player cartridge into the machine and
 *  feeds it the tune through the cartridge's handshake.
 *
 *  1. The tune's header is checked and prepared, little endian, with the load
 *     end address at $7e, and the song lengths go into the cartridge image.
 *  2. Low memory is cleared, $02 = $aa tells the cartridge to act as SID
 *     player, and the machine is reset with the cartridge attached.
 *  3. Once the cartridge has set up the machine it sets $02 = $01. Then memory
 *     from $0340 up is cleared, the tune and its header are written, and
 *     $0162 = $aa lets the cartridge go on.
 *  4. The cartridge copies the player into free RAM next to the tune and
 *     switches itself off through $DFFF, a switch of the device's cartridge
 *     emulation that VICE's generic 16KiB cartridge has on request. Then the
 *     cartridge is detached, and the machine is left playing.
 *
 *  The cartridge (restapi_sidcrt.h) is GPLv3, unlike VICE.
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
#include "c64cart.h"
#include "cart/c64-generic.h"
#include "cartridge.h"
#include "lib.h"
#include "log.h"
#include "machine.h"
#include "maincpu.h"
#include "mem.h"
#include "resources.h"
#include "restapi_http.h"
#include "restapi_sidcrt.h"
#include "restapi_sidplay.h"
#include "sid/sid.h"
#include "util.h"

/** \brief  Largest SID file read: a header and 64KiB of data */
#define SID_FILE_MAX            (0x10000 + 0x7c + 2)

/** \brief  Size of the cartridge: the player, then BASIC */
#define SIDCRT_SIZE             0x4000

/** \brief  Where the song lengths go in the cartridge image ($b000) */
#define SIDCRT_SONG_LENGTHS     0x3000

/** \brief  Song length used when none is known, in minutes */
#define DEFAULT_SONG_LENGTH_SID 5

/** \brief  How long the cartridge may take to ask for the tune, in seconds
 *
 * The device waits 30 times 25ms.
 */
#define HANDSHAKE_TIMEOUT       0.75

typedef enum sidplay_state_e {
    SIDPLAY_IDLE = 0,
    SIDPLAY_WAITING,        /* machine reset into the cartridge */
    SIDPLAY_PLAYING         /* tune handed over, cartridge not yet off */
} sidplay_state_t;

/** \brief  A started tune, from the request until the cartridge is off */
static struct {
    sidplay_state_t state;
    CLOCK deadline;             /* end of the handshake wait */
    char *cartfile;             /* the cartridge image, a temporary file */
    unsigned char header[0x80];
    int header_version;
    uint16_t flags;
    uint16_t start;
    uint16_t end;
    uint16_t header_location;
    unsigned char *data;        /* the tune, from its load address on */
    size_t data_length;
} tune;

static log_t sidplay_log = LOG_DEFAULT;

/* ------------------------------------------------------------------------- */
/* memory                                                                    */

static int bank(const char *name)
{
    int b = mem_bank_from_name(name);

    return (b >= 0) ? b : 0;
}

/* the device writes C64 memory with the machine stopped and all RAM mapped */
static void ram_write(uint16_t addr, uint8_t value)
{
    mem_bank_write(bank("ram"), addr, value, NULL);
}

static uint8_t ram_peek(uint16_t addr)
{
    return mem_bank_peek(bank("ram"), addr, NULL);
}

/* ------------------------------------------------------------------------- */
/* the cartridge                                                             */

/** \brief  Attach or detach without the power cycle CartridgeReset asks for
 *
 * A power cycle would clear the RAM that carries the handshake, and later stop
 * the tune that is playing. The device resets the CPU only.
 */
static int with_cartridge_reset_off(int attach, const char *file)
{
    int reset = 0;
    int result = 0;

    resources_get_int("CartridgeReset", &reset);
    resources_set_int("CartridgeReset", 0);
    if (attach) {
        generic_16kb_request_ultimate_switch(1);
        result = cartridge_attach_image(CARTRIDGE_GENERIC_16KB, file);
        generic_16kb_request_ultimate_switch(0);
    } else {
        cartridge_detach_image(CARTRIDGE_GENERIC_16KB);
    }
    resources_set_int("CartridgeReset", reset);
    return result;
}

static void forget_tune(void)
{
    if (tune.cartfile != NULL) {
        archdep_remove(tune.cartfile);
        lib_free(tune.cartfile);
    }
    lib_free(tune.data);
    memset(&tune, 0, sizeof(tune));
}

/** \brief  Take the cartridge out and forget the tune */
static void finish(void)
{
    if (tune.state != SIDPLAY_IDLE) {
        with_cartridge_reset_off(0, NULL);
    }
    forget_tune();
}

/** \brief  Fill \a image with the player, BASIC, and the song lengths
 *
 * As readSongLengths() does: the lengths come from \a sslfile when it can be
 * read, else from the "hacked" header field some SID files carry for their
 * default song; any missing or invalid length becomes five minutes.
 */
static void build_cartridge(unsigned char *image, const char *sslfile,
                            int number_of_songs, int default_song)
{
    unsigned char *lengths = image + SIDCRT_SONG_LENGTHS;
    int rom = bank("rom");
    FILE *fd = NULL;
    size_t loaded = 0;
    int i;

    memset(image, 0, SIDCRT_SIZE);
    memcpy(image, restapi_sidcrt, sizeof(restapi_sidcrt));
    for (i = 0; i < 0x2000; i++) {
        image[0x2000 + i] = mem_bank_peek(rom, (uint16_t)(0xa000 + i), NULL);
    }

    memset(lengths, 0, 512);
    if (sslfile != NULL) {
        fd = fopen(sslfile, MODE_READ);
    }
    if (fd != NULL) {
        loaded = fread(lengths, 1, 512, fd);
        fclose(fd);
        log_message(sidplay_log, "Song length array loaded. %lu bytes", (unsigned long)loaded);
    } else if (tune.header[0x04] == 2) {
        /* only the default song's length; none for 2SID and 3SID tunes */
        if (tune.header[0x7b] < 0xa0 && (tune.header[0x7b] & 0x0f) < 0x0a
            && tune.header[0x7a] < 0xa0 && (tune.header[0x7a] & 0x0f) < 0x0a) {
            int index = (default_song - 1) * 2;

            if (index >= 0 && index + 1 < 512) {
                lengths[index] = tune.header[0x7b];
                lengths[index + 1] = tune.header[0x7a];
            }
        }
        tune.header[0x7a] = 0;
        tune.header[0x7b] = 0;
    }

    /* the device checks one pair per song; a tune with more songs than fit
       would run past the image, so the check stops at its end */
    for (i = 0; i < number_of_songs * 2 && SIDCRT_SONG_LENGTHS + i + 1 < SIDCRT_SIZE; i += 2) {
        uint8_t minutes = lengths[i];
        uint8_t seconds = lengths[i + 1];

        if (minutes > 0x99 || (minutes & 0x0f) > 0x09
            || seconds > 0x59 || (seconds & 0x0f) > 0x09) {
            lengths[i] = 0;
            lengths[i + 1] = 0;
            minutes = 0;
            seconds = 0;
        }
        if (minutes == 0 && seconds == 0) {
            lengths[i] = DEFAULT_SONG_LENGTH_SID;
        }
    }
}

/* ------------------------------------------------------------------------- */
/* SID chips                                                                 */

/** \brief  The VICE model for a model field of the flags
 *
 * \param[in]  field   two bits: 1 = 6581, 2 = 8580, 0 = unknown, 3 = either
 * \param[in]  other   what to return for 0 and 3
 */
static int sid_model_of(int field, int other)
{
    switch (field) {
        case 1:
            return SID_MODEL_6581;
        case 2:
            return SID_MODEL_8580;
        default:
            return other;
    }
}

/** \brief  Set up the SIDs the tune asks for, as ConfigSIDs() does
 *
 * The device maps its SIDs to the addresses in the header, each with the model
 * the header asks for; one it leaves open gets the model of the first, as
 * ConfigSIDs() does. A first one left open keeps the current SidModel.
 */
static void configure_sids(void)
{
    int count = 1;
    int model = sid_model_of((tune.flags >> 4) & 3, -1);

    if (tune.header_version >= 2) {
        if (tune.header_version >= 3 && tune.header[0x7a]) {
            resources_set_int("Sid2AddressStart", 0xd000 | (tune.header[0x7a] << 4));
            resources_set_int("Sid2Model",
                              sid_model_of((tune.flags >> 6) & 3, SID_MODEL_SAME_AS_FIRST));
            count = 2;
        }
        if (tune.header_version >= 4 && tune.header[0x7b]) {
            resources_set_int("Sid3AddressStart", 0xd000 | (tune.header[0x7b] << 4));
            resources_set_int("Sid3Model",
                              sid_model_of((tune.flags >> 8) & 3, SID_MODEL_SAME_AS_FIRST));
            count = 3;
        }
        if (model >= 0) {
            resources_set_int("SidModel", model);
        }
    }
    resources_set_int("SidStereo", count - 1);
}

/* ------------------------------------------------------------------------- */
/* the handshake                                                             */

/** \brief  Hand the tune to the cartridge, as FileTypeSID::load() does */
static void load_tune(void)
{
    unsigned int addr;
    size_t i;

    for (addr = 0x0340; addr <= 0xffff; addr++) {
        ram_write((uint16_t)addr, 0);
    }
    /* at most 64KiB, wrapping at $ffff like the device's DMA load */
    for (i = 0; i < tune.data_length && i < 0x10000; i++) {
        ram_write((uint16_t)(tune.start + i), tune.data[i]);
    }

    tune.header[0x7e] = (uint8_t)(tune.end & 0xff);
    tune.header[0x7f] = (uint8_t)(tune.end >> 8);
    configure_sids();
    for (i = 0; i < 0x80; i++) {
        ram_write((uint16_t)(tune.header_location + i), tune.header[i]);
    }

    ram_write(0x0164, (uint8_t)(tune.header_location & 0xff));
    ram_write(0x0165, (uint8_t)(tune.header_location >> 8));
    ram_write(0x002d, (uint8_t)(tune.end & 0xff));
    ram_write(0x002e, (uint8_t)(tune.end >> 8));
    ram_write(0x002f, (uint8_t)(tune.end & 0xff));
    ram_write(0x0030, (uint8_t)(tune.end >> 8));
    ram_write(0x0031, (uint8_t)(tune.end & 0xff));
    ram_write(0x0032, (uint8_t)(tune.end >> 8));
    ram_write(0x00ae, (uint8_t)(tune.end & 0xff));
    ram_write(0x00af, (uint8_t)(tune.end >> 8));

    ram_write(0x0162, 0xaa);    /* handshake */
    ram_write(0x00ba, 8);       /* load drive */
    ram_write(0x0090, 0x40);    /* load status */
    ram_write(0x0035, 0);       /* FRESPC */
    ram_write(0x0036, 0xa0);
}

void restapi_sidplay_poll(void)
{
    switch (tune.state) {
        case SIDPLAY_WAITING:
            if (ram_peek(0x0002) == 0x01) {
                load_tune();
                lib_free(tune.data);
                tune.data = NULL;
                tune.state = SIDPLAY_PLAYING;
            } else if (maincpu_clk > tune.deadline) {
                /* the device gives up quietly too, and restores the cartridge */
                log_warning(sidplay_log, "Time out waiting for the SID player cartridge.");
                finish();
            }
            break;
        case SIDPLAY_PLAYING:
            if (generic_16kb_ultimate_switched_off()) {
                finish();
            }
            break;
        default:
            break;
    }
}

void restapi_sidplay_shutdown(void)
{
    finish();
}

/* ------------------------------------------------------------------------- */
/* the request                                                               */

static void fail(restapi_response_t *resp, int status, const char *message)
{
    resp->status = status;
    restapi_error(resp, "%s", message);
}

/** \brief  Read up to SID_FILE_MAX bytes of \a name
 *
 * \return  the bytes, or NULL when the file cannot be opened
 */
static unsigned char *read_file(const char *name, size_t *length)
{
    FILE *fd = fopen(name, MODE_READ);
    unsigned char *data;

    if (fd == NULL) {
        return NULL;
    }
    data = lib_malloc(SID_FILE_MAX);
    *length = fread(data, 1, SID_FILE_MAX, fd);
    fclose(fd);
    return data;
}

/** \brief  The song lengths file the device looks for next to \a file
 *
 * <dir>/SONGLENGTHS/<name>.ssl, as FileTypeSID::play_file() builds it.
 */
static char *default_ssl_file(const char *file)
{
    char *directory = NULL;
    char *name = NULL;
    char *dot;
    char *path;

    util_fname_split(file, &directory, &name);
    dot = strrchr(name, '.');
    if (dot != NULL) {
        *dot = '\0';
    }
    path = util_join_paths(directory, "SONGLENGTHS", name, NULL);
    lib_free(directory);
    lib_free(name);
    name = util_concat(path, ".ssl", NULL);
    lib_free(path);
    return name;
}

void restapi_sidplay(restapi_request_t *req, restapi_response_t *resp)
{
    const char *file;
    char *sslfile;
    int songnr = restapi_param_int(req, "songnr", 0);
    unsigned char *data;
    size_t length = 0;
    size_t pos;
    int data_offset;
    int number_of_songs;
    int default_song;
    int song;
    int i;
    unsigned char image[SIDCRT_SIZE];
    FILE *fd;

    if (sidplay_log == LOG_DEFAULT) {
        sidplay_log = log_open("REST sidplay");
    }

    if (req->method == RESTAPI_METHOD_POST) {
        if (req->upload_count == 0) {
            fail(resp, RESTAPI_HTTP_PRECONDITION_FAILED, "Expected Body, but got none.");
            return;
        }
        file = req->uploads[0];
        sslfile = (req->upload_count > 1) ? lib_strdup(req->uploads[1]) : NULL;
    } else {
        file = restapi_param(req, "file");
        if (file == NULL || *file == '\0') {
            fail(resp, RESTAPI_HTTP_BAD_REQUEST, "Missing required parameter 'file'.");
            return;
        }
        sslfile = default_ssl_file(file);
    }

    /* play_file() turns a song number of 0 or less into "the default song";
       above 256 there is no such command */
    if (songnr > 0x100) {
        lib_free(sslfile);
        fail(resp, RESTAPI_HTTP_INTERNAL_ERROR, "Undefined subsystem command");
        return;
    }

    data = read_file(file, &length);
    if (data == NULL) {
        lib_free(sslfile);
        fail(resp, RESTAPI_HTTP_NOT_FOUND, "Cannot open file");
        return;
    }

    /* a new tune replaces one still being handed over */
    finish();

    memset(tune.header, 0, sizeof(tune.header));
    memcpy(tune.header, data, (length < 0x7e) ? length : 0x7e);
    if (memcmp(tune.header, "PSID", 4) != 0 && memcmp(tune.header, "RSID", 4) != 0) {
        goto format_error;
    }

    /* processHeader() */
    tune.header_version = (tune.header[0x04] << 8) | tune.header[0x05];
    data_offset = (tune.header[0x06] << 8) | tune.header[0x07];
    if (data_offset < 0x7c) {
        for (i = data_offset; i < 0x7c; i++) {
            tune.header[i] = 0;
        }
        tune.header[0x04] = 0;
        tune.header[0x05] = 2;
        tune.header_version = 2;
        tune.header[0x07] = 0x7c;
    }
    number_of_songs = (tune.header[0x0e] << 8) | tune.header[0x0f];
    if (tune.header[0x77] & 1) {
        /* Compute's Sidplayer data: the device plays it with its MUS player,
           which is not part of this */
        lib_free(data);
        lib_free(sslfile);
        fail(resp, RESTAPI_HTTP_NOT_IMPLEMENTED,
             "This command is not supported on this architecture");
        return;
    }

    /* the load address is in the header, or else in front of the data */
    pos = (size_t)data_offset;
    tune.start = (uint16_t)((tune.header[0x08] << 8) | tune.header[0x09]);
    if (tune.start == 0) {
        tune.start = (uint16_t)((pos + 1 < length) ? (data[pos] | (data[pos + 1] << 8)) : 0);
        pos += 2;
    }
    /* sic: the device takes two bytes off the length whether or not the data
       began with a load address, so its end address may fall short by two */
    tune.end = (uint16_t)(tune.start + ((int)length - data_offset - 2));
    tune.header[0x7e] = (uint8_t)(tune.end & 0xff);
    tune.header[0x7f] = (uint8_t)(tune.end >> 8);

    if (tune.end < tune.start) {
        lib_free(data);
        lib_free(sslfile);
        fail(resp, RESTAPI_HTTP_UNSUPPORTED_MEDIA_TYPE, "SID File Memory Rollover");
        return;
    }
    if (tune.start >= 0x03c0) {
        tune.header_location = 0x0340;
    } else if (tune.end < 0xff70) {
        tune.header_location = 0xff70;
    } else {
        lib_free(data);
        lib_free(sslfile);
        fail(resp, RESTAPI_HTTP_INSUFFICIENT_STORAGE, "Out of memory");
        return;
    }

    default_song = (tune.header[0x10] << 8) | tune.header[0x11];
    if (default_song == 0) {
        default_song = 1;
    }
    song = (songnr <= 0) ? default_song : songnr;
    if (song > number_of_songs) {
        lib_free(data);
        lib_free(sslfile);
        fail(resp, RESTAPI_HTTP_BAD_REQUEST, "Invalid Song Number Requested");
        return;
    }
    tune.header[0x10] = (uint8_t)((song - 1) >> 8);
    tune.header[0x11] = (uint8_t)((song - 1) & 0xff);
    tune.flags = (uint16_t)((tune.header[0x76] << 8) | tune.header[0x77]);

    /* the player wants version, data offset, load, init and play address,
       song count and start song little endian */
    for (i = 4; i < 4 + 7 * 2; i += 2) {
        unsigned char high = tune.header[i];

        tune.header[i] = tune.header[i + 1];
        tune.header[i + 1] = high;
    }

    build_cartridge(image, sslfile, number_of_songs, default_song);
    lib_free(sslfile);

    tune.data_length = (pos < length) ? length - pos : 0;
    tune.data = lib_malloc(tune.data_length + 1);
    memcpy(tune.data, data + pos, tune.data_length);
    lib_free(data);

    tune.cartfile = archdep_tmpnam();
    fd = fopen(tune.cartfile, MODE_WRITE);
    if (fd == NULL || fwrite(image, 1, SIDCRT_SIZE, fd) != SIDCRT_SIZE) {
        if (fd != NULL) {
            fclose(fd);
        }
        forget_tune();
        fail(resp, RESTAPI_HTTP_INTERNAL_ERROR, "Internal Error");
        return;
    }
    fclose(fd);

    /* what the device does before it starts the cartridge */
    for (i = 0; i < 0x400; i++) {
        ram_write((uint16_t)i, 0);
    }
    ram_write(0x0282, 0x08);    /* make memory testing obsolete */
    ram_write(0x0284, 0xa0);
    ram_write(0x0288, 0x04);
    mem_bank_write(bank("cpu"), 0xdd0d, 0x7f, NULL);
    mem_bank_read(bank("cpu"), 0xdd0d, NULL);
    ram_write(0x0002, 0xaa);    /* "act as SID player" */
    ram_write(0x0162, 0x00);    /* clear the handshake */

    if (with_cartridge_reset_off(1, tune.cartfile) < 0) {
        forget_tune();
        fail(resp, RESTAPI_HTTP_INTERNAL_ERROR, "Internal Error");
        return;
    }
    machine_trigger_reset(MACHINE_RESET_MODE_RESET_CPU);

    tune.state = SIDPLAY_WAITING;
    tune.deadline = maincpu_clk
                    + (CLOCK)(HANDSHAKE_TIMEOUT * machine_get_cycles_per_second());
    return;

format_error:
    lib_free(data);
    lib_free(sslfile);
    forget_tune();
    fail(resp, RESTAPI_HTTP_UNSUPPORTED_MEDIA_TYPE, "Error detected in file format");
}
