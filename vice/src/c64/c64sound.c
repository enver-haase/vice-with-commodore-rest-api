/*
 * c64sound.c - C64/C128 sound emulation.
 *
 * Written by
 *  Marco van den Heuvel <blackystardust68@yahoo.com>
 *
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
#include <string.h>

#include "cartio.h"
#include "cartridge.h"
#include "lib.h"
#include "machine.h"
#include "sid.h"
#include "sid-resources.h"
#include "sound.h"
#include "types.h"

/* ---------------------------------------------------------------------*/

/* The further SIDs, #2 and up, each an I/O device, can be cartridges or
   internal boards. They share their functions, which find the SID by the
   address: their address mask passes the whole of it. Index 0 is unused. */
static io_source_t sid_device[SOUND_SIDS_MAX];
static io_source_list_t *sid_list_item[SOUND_SIDS_MAX];

static int sid_io_chip(uint16_t addr)
{
    int chipno;

    for (chipno = 1; chipno < SOUND_SIDS_MAX; chipno++) {
        if (addr >= sid_device[chipno].start_address
            && addr <= sid_device[chipno].end_address) {
            return chipno;
        }
    }
    return 0;
}

static uint8_t sid_io_read(uint16_t addr)
{
    return sid_read_chip_nr(addr, sid_io_chip(addr));
}

static uint8_t sid_io_peek(uint16_t addr)
{
    return sid_peek_chip_nr(addr, sid_io_chip(addr));
}

static void sid_io_store(uint16_t addr, uint8_t byte)
{
    sid_store_chip_nr(addr, byte, sid_io_chip(addr));
}

/* the monitor calls it with the start address of the device */
static int sid_io_dump(void *context, uint16_t addr)
{
    return sid_dump_chip_nr(sid_io_chip(addr));
}

static void sid_devices_init(void)
{
    int chipno;

    for (chipno = 1; chipno < SOUND_SIDS_MAX; chipno++) {
        if (sid_device[chipno].name != NULL) {
            continue;
        }
        sid_device[chipno].name = lib_msprintf("SID #%d", chipno + 1);
        sid_device[chipno].detach_id = IO_DETACH_RESOURCE;   /* on a read-collision, */
        sid_device[chipno].resource_name = "SidStereo";      /* set it to 0 */
        sid_device[chipno].start_address = 0xde00;           /* moved by machine_sid_check_range() */
        sid_device[chipno].end_address = 0xde1f;
        sid_device[chipno].address_mask = 0xffff;            /* the functions need the whole address */
        sid_device[chipno].io_source_valid = 1;
        sid_device[chipno].store = sid_io_store;
        sid_device[chipno].read = sid_io_read;
        sid_device[chipno].peek = sid_io_peek;
        /* the field's type takes no arguments, but the monitor calls it with
           (context, address), through void (*)(void) as the generic type */
        sid_device[chipno].dump = (int (*)(void))(void (*)(void))sid_io_dump;
        sid_device[chipno].cart_id = IO_CART_ID_NONE;
        sid_device[chipno].io_source_prio = IO_PRIO_NORMAL;
        sid_device[chipno].mirror_mode = IO_MIRROR_NONE;
    }
}

/* ---------------------------------------------------------------------*/

#ifdef SOUND_SYSTEM_FLOAT
/* stereo mixing placement of the C64 SID sound */
/* stereo mixing placement of the C64 SID sound, set by machine_sid2_enable() */
static sound_chip_mixing_spec_t sid_sound_mixing_spec[SOUND_CHIP_CHANNELS_MAX];
#endif

/* C64 SID sound chip */
static sound_chip_t sid_sound_chip = {
    sid_sound_machine_open,              /* sound chip open function */
    sid_sound_machine_init,              /* sound chip init function */
    sid_sound_machine_close,             /* sound chip close function */
    sid_sound_machine_calculate_samples, /* sound chip calculate samples function */
    sid_sound_machine_store,             /* sound chip store function */
    sid_sound_machine_read,              /* sound chip read function */
    sid_sound_machine_reset,             /* sound chip reset function */
    sid_sound_machine_cycle_based,       /* sound chip 'is_cycle_based()' function, resid engine is cycle based, all other engines are not */
    sid_sound_machine_channels,          /* sound chip 'get_amount_of_channels()' function, the amount of channels depends on the extra amount of active SIDs */
#ifdef SOUND_SYSTEM_FLOAT
    sid_sound_mixing_spec,               /* stereo mixing placement specs */
#endif
    1                                    /* sound chip is always enabled */
};

static uint16_t sid_sound_chip_offset = 0;

void sid_sound_chip_init(void)
{
#ifdef SOUND_SYSTEM_FLOAT
    int chipno;

    /* all on both sides until machine_sid2_enable() sets them */
    for (chipno = 0; chipno < SOUND_CHIP_CHANNELS_MAX; chipno++) {
        sid_sound_mixing_spec[chipno].left_channel_volume = 100;
        sid_sound_mixing_spec[chipno].right_channel_volume = 100;
    }
#endif
    sid_devices_init();
    sid_sound_chip_offset = sound_chip_register(&sid_sound_chip);
}

/* ---------------------------------------------------------------------*/

int machine_sid_check_range(int chipno, unsigned int sid_adr)
{
    int high;

    if (chipno < 1 || chipno >= SOUND_SIDS_MAX) {
        return -1;
    }
    if (machine_class == VICE_MACHINE_C128) {
        if (!((sid_adr >= 0xd400 && sid_adr <= 0xd4e0) || (sid_adr >= 0xd700 && sid_adr <= 0xdfe0))) {
            return -1;
        }
        high = (sid_adr >= 0xd400 && sid_adr <= 0xd4e0);
    } else {
        if (!(sid_adr >= 0xd400 && sid_adr <= 0xdfe0)) {
            return -1;
        }
        high = (sid_adr >= 0xd400 && sid_adr <= 0xd7e0);
    }

    sid_devices_init();
    sid_device[chipno].start_address = (uint16_t)sid_adr;
    sid_device[chipno].end_address = (uint16_t)(sid_adr + 0x1f);
    sid_device[chipno].io_source_prio = high ? IO_PRIO_HIGH : IO_PRIO_NORMAL;
    if (sid_list_item[chipno] != NULL) {
        io_source_unregister(sid_list_item[chipno]);
        sid_list_item[chipno] = io_source_register(&sid_device[chipno]);
    } else if (sid_stereo >= chipno) {
        sid_list_item[chipno] = io_source_register(&sid_device[chipno]);
    }
    return 0;
}

void machine_sid2_enable(int val)
{
    int chipno;

    sid_devices_init();
    for (chipno = 1; chipno < SOUND_SIDS_MAX; chipno++) {
        if (sid_list_item[chipno] != NULL) {
            io_source_unregister(sid_list_item[chipno]);
            sid_list_item[chipno] = NULL;
        }
    }
    for (chipno = 1; chipno <= val && chipno < SOUND_SIDS_MAX; chipno++) {
        sid_list_item[chipno] = io_source_register(&sid_device[chipno]);
    }

#ifdef SOUND_SYSTEM_FLOAT
    /* set stereo rendering preferences: one SID on both sides; of several,
       the even ones left, the odd ones right, and with an odd count the last
       one on both */
    for (chipno = 0; chipno <= val && chipno < SOUND_SIDS_MAX; chipno++) {
        int both = (val == 0) || ((val & 1) == 0 && chipno == val);

        sid_sound_mixing_spec[chipno].left_channel_volume = ((chipno & 1) == 0 || both) ? 100 : 0;
        sid_sound_mixing_spec[chipno].right_channel_volume = ((chipno & 1) == 1 || both) ? 100 : 0;
    }
#endif
}

char *sound_machine_dump_state(sound_t *psid)
{
    return sid_sound_machine_dump_state(psid);
}

void sound_machine_enable(int enable)
{
    sid_sound_machine_enable(enable);
}
