/*
 * menu_sid.c - Implementation of the SID settings menu for the SDL UI.
 *
 * Written by
 *  Hannu Nuotio <hannu.nuotio@tut.fi>
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

#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "lib.h"
#include "menu_common.h"
#include "resources.h"
#include "sid.h"
#include "sidcart.h"
#include "types.h"
#include "uiactions.h"
#include "uimenu.h"

#include "menu_sid.h"


static UI_MENU_CALLBACK(custom_SidModel_callback)
{
    int engine, model, selected;

    selected = vice_ptr_to_int(param);

    if (activated) {
        engine = selected >> 8;
        model = selected & 0xff;
        sid_set_engine_model(engine, model);
    } else {
        resources_get_int("SidEngine", &engine);
        resources_get_int("SidModel", &model);

        if (selected == ((engine << 8) | model)) {
            return sdl_menu_text_tick;
        }
    }

    return NULL;
}

static ui_menu_entry_t *sid_model_menu = NULL;

#if defined(HAVE_RESIDFP)

static ui_menu_entry_t *sid_profile_menu = NULL;

static int sid_profile_selected = -1;

typedef struct {
    char *name;
    double value;
} filter_map_t;

/* taken from https://github.com/libsidplayfp/sidplayfp/blob/c44e4c0e74c12401a6dbef0ea371e34a30de099d/src/player.cpp#L138 */
static const filter_map_t filterRangeMap[] =
{
    { "Anthony Lees",                        1.3 },
    { "Antony Crowther (Ratt)",              1.1 },
    { "Barry Leitch (The Jackal)",           0.3 },
    { "Ben Daglish",                         0.6 },
    { "Carsten Berggreen (Scarzix)",         0.7 },
    { "Charles Deenen",                      0.2 },
    { "Chris Huelsbeck",                     0.9 },
    { "David Dunn",                          0.1 },
    { "David Dunn & Aidan Bell",             0.1 },
    { "David Whittaker",                     0.15 },
    { "Edwin van Santen",                    0.5 },
    { "Edwin van Santen & Falco Paul",       0.4 },
    { "Edwin van Santen & Venom",            0.4 },
    { "Falco Paul",                          0.15 },
    { "Falco Paul & Edwin van Santen",       0.4 },
    { "Figge Wasberger (Fegolhuzz)",         0.25 },
    { "Fred Gray",                           0.4 },
    { "Geir Tjelta",                         0.5 },
    { "Geoff Follin",                        0.85 },
    { "Georg Feil",                          0.2 },
    { "Glenn Rune Gallefoss",                1.3 },
    { "Graham Jarvis & Rob Hartshorne",      0.25 },
    { "Jason Page",                          0.35 },
    { "Jeroen Tel",                          0.35 },
    { "Johannes Bjerregaard",                0.35 },
    { "Jonathan Dunn",                       0.25 },
    { "Jouni Ikonen (Mixer)",                0.25 },
    { "Jori Olkkonen",                       0.15 },
    { "Jori Olkkonen (Yip)",                 0.35 },
    { "Kim Christensen (Future Freak)",      0.35 },
    { "Linus Akesson (lft)",                 0.3 },
    { "Mark Cooksey",                        0.4 },
    { "Mark Wilson",                         0.2 },
    { "Markus Mueller (Superbrain)",         0.5 },
    { "Martin Galway",                       0.65 },
    { "Martin Walker",                       0.15 },
    { "Matt Gray",                           0.3 },
    { "Michael Hendriks",                    0.35 },
    { "Mitch & Dane",                        0.85 },
    { "M. Nilsson-Vonderburgh (Mic)",        0.3 },
    { "M. Nilsson-Vonderburgh (Mitch)",      0.3 },
    { "M. Nilsson-Vonderburgh (Yankee)",     0.3 },
    { "NM156",                               0.7 },
    { "Neil Brennan",                        0.25 },
    { "Peter Clarke",                        0.2 },
    { "Pex Tufvesson (Mahoney)",             0.35 },
    { "Pex Tufvesson (Zax)",                 0.35 },
    { "Renato Brosowski (Zoci-Joe)",         0.3 },
    { "Reyn Ouwehand",                       0.8 },
    { "Richard Joseph",                      0.3 },
    { "Rob Hubbard",                         0.35 },
    { "Russell Lieblich",                    0.25 },
    { "Stellan Andersson (Dane)",            0.85 },
    { "Steve Turner",                        0.6 },
    { "Tim Follin",                          0.5 },
    { "Thomas E. Petersen (Laxity)",         0.3 },
    { "Thomas E. Petersen (TSS)",            0.3 },
    { "Thomas Mogensen (DRAX)",              0.3 },
    { NULL, 0.0 },
};

static UI_MENU_CALLBACK(custom_SidProfile_callback)
{
    int selected;
    double value;
    int value_int;

    selected = vice_ptr_to_int(param);
    if (activated) {
        sid_profile_selected = selected;
        value = ((filterRangeMap[selected].value * 20.0f) - 1.0f) / 39.0f;
        value_int = (int)round(value * 1000.0f);
        resources_set_int("SidResid6581FilterCurve", RESIDFP_6581_FILTER_CURVE_DEFAULT);
        resources_set_int("SidResidCombinedWaveformStrength", RESIDFP_COMBINED_WAVEFORM_STRENGTH_DEFAULT);
        resources_set_int("SidResid6581FilterRange", value_int);
    } else {
        if (selected == sid_profile_selected) {
            return sdl_menu_text_tick;
        }
    }

    return NULL;
}
#endif

#if defined(HAVE_RESID) || defined(HAVE_RESIDFP)
UI_MENU_DEFINE_RADIO(SidResidSampling)

static const ui_menu_entry_t sid_sampling_menu[] = {
    {   .string   = "Fast",
        .type     = MENU_ENTRY_RESOURCE_RADIO,
        .callback = radio_SidResidSampling_callback,
        .data     = (ui_callback_data_t)SID_RESID_SAMPLING_FAST
    },
    {   .string   = "Interpolating",
        .type     = MENU_ENTRY_RESOURCE_RADIO,
        .callback = radio_SidResidSampling_callback,
        .data     = (ui_callback_data_t)SID_RESID_SAMPLING_INTERPOLATION
    },
    {   .string   = "Resampling",
        .type     = MENU_ENTRY_RESOURCE_RADIO,
        .callback = radio_SidResidSampling_callback,
        .data     = (ui_callback_data_t)SID_RESID_SAMPLING_RESAMPLING
    },
    {   .string   = "Fast Resampling",
        .type     = MENU_ENTRY_RESOURCE_RADIO,
        .callback = radio_SidResidSampling_callback,
        .data     = (ui_callback_data_t)SID_RESID_SAMPLING_FAST_RESAMPLING
    },
    SDL_MENU_LIST_END
};
#endif

#ifdef HAVE_RESID
UI_MENU_DEFINE_SLIDER(SidResidPassband, 0, 90)
UI_MENU_DEFINE_SLIDER(SidResidGain, 90, 100)
UI_MENU_DEFINE_SLIDER(SidResidFilterBias, -5000, 5000)

/* Do we have new 8580 filter emulation? */
#ifdef HAVE_NEW_8580_FILTER

/* Yup, create 8580 filter sliders */
UI_MENU_DEFINE_SLIDER(SidResid8580Passband, 0, 90)
UI_MENU_DEFINE_SLIDER(SidResid8580Gain, 90, 100)
UI_MENU_DEFINE_SLIDER(SidResid8580FilterBias, -5000, 5000)

/* Create menu items including 8580 slider */
# define VICE_SDL_RESID_OPTIONS                                                                                               \
    {   .string   = "reSID sampling method",                                                                                  \
        .type     = MENU_ENTRY_SUBMENU,                                                                                       \
        .callback = submenu_radio_callback,                                                                                   \
        .data     = (ui_callback_data_t)sid_sampling_menu                                                                     \
    },                                                                                                                        \
    {   .string   = "reSID 6581 resampling passband",                                                                         \
        .type     = MENU_ENTRY_RESOURCE_INT,                                                                                  \
        .callback = slider_SidResidPassband_callback,                                                                         \
        .data     = (ui_callback_data_t)"Enter passband in percentage of total bandwidth (lower is faster, higher is better)" \
    },                                                                                                                        \
    {   .string   = "reSID 6581 filter gain",                                                                                 \
        .type     = MENU_ENTRY_RESOURCE_INT,                                                                                  \
        .callback = slider_SidResidGain_callback,                                                                             \
        .data     = (ui_callback_data_t)"Set filter gain in percent"                                                          \
    },                                                                                                                        \
    {   .string   = "reSID 6581 filter bias",                                                                                 \
        .type     = MENU_ENTRY_RESOURCE_INT,                                                                                  \
        .callback = slider_SidResidFilterBias_callback,                                                                       \
        .data     = (ui_callback_data_t)"Set filter bias in mV"                                                               \
    },                                                                                                                        \
    {   .string   = "reSID 8580 resampling passband",                                                                         \
        .type     = MENU_ENTRY_RESOURCE_INT,                                                                                  \
        .callback = slider_SidResid8580Passband_callback,                                                                     \
        .data     = (ui_callback_data_t)"Enter passband in percentage of total bandwidth (lower is faster, higher is better)" \
    },                                                                                                                        \
    {   .string   = "reSID 8580 filter gain",                                                                                 \
        .type     = MENU_ENTRY_RESOURCE_INT,                                                                                  \
        .callback = slider_SidResid8580Gain_callback,                                                                         \
        .data     = (ui_callback_data_t)"Set filter gain in percent"                                                          \
    },                                                                                                                        \
    {   .string   = "reSID 8580 filter bias",                                                                                 \
        .type     = MENU_ENTRY_RESOURCE_INT,                                                                                  \
        .callback = slider_SidResid8580FilterBias_callback,                                                                   \
        .data     = (ui_callback_data_t)"Set filter bias in mV"                                                               \
    },
#else

/* Nope, don't show 8580 filter sliders */
# define VICE_SDL_RESID_OPTIONS                                                                                               \
    {   .string   = "reSID sampling method",                                                                                  \
        .type     = MENU_ENTRY_SUBMENU,                                                                                       \
        .callback = submenu_radio_callback,                                                                                   \
        .data     = (ui_callback_data_t)sid_sampling_menu                                                                     \
    },                                                                                                                        \
    {   .string   = "reSID 6581 resampling passband",                                                                         \
        .type     = MENU_ENTRY_RESOURCE_INT,                                                                                  \
        .callback = slider_SidResidPassband_callback,                                                                         \
        .data     = (ui_callback_data_t)"Enter passband in percentage of total bandwidth (lower is faster, higher is better)" \
    },                                                                                                                        \
    {   .string   = "reSID 6581 filter gain",                                                                                 \
        .type     = MENU_ENTRY_RESOURCE_INT,                                                                                  \
        .callback = slider_SidResidGain_callback,                                                                             \
        .data     = (ui_callback_data_t)"Set filter gain in percent"                                                          \
    },                                                                                                                        \
    {   .string   = "reSID 6581 filter bias",                                                                                 \
        .type     = MENU_ENTRY_RESOURCE_INT,                                                                                  \
        .callback = slider_SidResidFilterBias_callback,                                                                       \
        .data     = (ui_callback_data_t)"Set filter bias in mV"                                                               \
    },
#endif

#endif /* HAVE_RESID */

#ifdef HAVE_RESIDFP
UI_MENU_DEFINE_SLIDER(SidResid6581FilterCurve, 0, RESIDFP_6581_FILTER_CURVE_MAX)
UI_MENU_DEFINE_SLIDER(SidResid6581FilterRange, 0, RESIDFP_6581_FILTER_RANGE_MAX)
UI_MENU_DEFINE_SLIDER(SidResid8580FilterCurve, 0, RESIDFP_8580_FILTER_CURVE_MAX)
UI_MENU_DEFINE_SLIDER(SidResidCombinedWaveformStrength, 0, RESIDFP_COMBINED_WAVEFORM_STRENGTH_MAX)
UI_MENU_DEFINE_TOGGLE(SidResid6581OldCaps)

# define VICE_SDL_RESIDFP_OPTIONS                                                                                             \
    {   .string   = "reSIDfp Profile",                                                                                        \
        .type     = MENU_ENTRY_SUBMENU,                                                                                       \
        .callback = submenu_radio_callback,                                                                                   \
        .data     = (ui_callback_data_t)0xdeadc0de,                                                                           \
    },                                                                                                                        \
    {   .string   = "reSIDfp sampling method",                                                                                \
        .type     = MENU_ENTRY_SUBMENU,                                                                                       \
        .callback = submenu_radio_callback,                                                                                   \
        .data     = (ui_callback_data_t)sid_sampling_menu                                                                     \
    },                                                                                                                        \
    {   .string   = "reSIDfp 6581 filter curve",                                                                              \
        .type     = MENU_ENTRY_RESOURCE_INT,                                                                                  \
        .callback = slider_SidResid6581FilterCurve_callback,                                                                  \
        .data     = (ui_callback_data_t)"Set filter curve"                                                                    \
    },                                                                                                                        \
    {   .string   = "reSIDfp 6581 filter range",                                                                              \
        .type     = MENU_ENTRY_RESOURCE_INT,                                                                                  \
        .callback = slider_SidResid6581FilterRange_callback,                                                                  \
        .data     = (ui_callback_data_t)"Set filter range"                                                                    \
    },                                                                                                                        \
    {   .string   = "reSIDfp 8580 filter curve",                                                                              \
        .type     = MENU_ENTRY_RESOURCE_INT,                                                                                  \
        .callback = slider_SidResid8580FilterCurve_callback,                                                                  \
        .data     = (ui_callback_data_t)"Set filter curve"                                                                    \
    },                                                                                                                        \
    {   .string   = "reSIDfp 6581 mixed wave strength",                                                                       \
        .type     = MENU_ENTRY_RESOURCE_INT,                                                                                  \
        .callback = slider_SidResidCombinedWaveformStrength_callback,                                                         \
        .data     = (ui_callback_data_t)"Set mixed waveform strength"                                                         \
    },                                                                                                                        \
    {   .string   = "reSIDfp 6581 old 2200pf caps",                                                                           \
        .type     = MENU_ENTRY_RESOURCE_TOGGLE,                                                                               \
        .callback = toggle_SidResid6581OldCaps_callback,                                                                      \
    },
#endif /* HAVE_RESIDFP */


#ifdef HAVE_USBSID
UI_MENU_DEFINE_TOGGLE(SidUSBSIDReadMode)
UI_MENU_DEFINE_TOGGLE(SidUSBSIDAudioMode)
UI_MENU_DEFINE_RADIO(SidUSBSIDDiffSize)
UI_MENU_DEFINE_RADIO(SidUSBSIDBufferSize)

static const ui_menu_entry_t us_diffsize_menu[] = {
    {   .string   = "32",
        .type     = MENU_ENTRY_RESOURCE_RADIO,
        .callback = radio_SidUSBSIDDiffSize_callback,
        .data     = (ui_callback_data_t)32
    },
    {   .string   = "64",
        .type     = MENU_ENTRY_RESOURCE_RADIO,
        .callback = radio_SidUSBSIDDiffSize_callback,
        .data     = (ui_callback_data_t)64
    },
    {   .string   = "128",
        .type     = MENU_ENTRY_RESOURCE_RADIO,
        .callback = radio_SidUSBSIDDiffSize_callback,
        .data     = (ui_callback_data_t)128
    },
    {   .string   = "256",
        .type     = MENU_ENTRY_RESOURCE_RADIO,
        .callback = radio_SidUSBSIDDiffSize_callback,
        .data     = (ui_callback_data_t)256
    },
    SDL_MENU_LIST_END
};

static const ui_menu_entry_t us_buffsize_menu[] = {
    {   .string   = "512",
        .type     = MENU_ENTRY_RESOURCE_RADIO,
        .callback = radio_SidUSBSIDBufferSize_callback,
        .data     = (ui_callback_data_t)512
    },
    {   .string   = "1024",
        .type     = MENU_ENTRY_RESOURCE_RADIO,
        .callback = radio_SidUSBSIDBufferSize_callback,
        .data     = (ui_callback_data_t)1024
    },
    {   .string   = "2048",
        .type     = MENU_ENTRY_RESOURCE_RADIO,
        .callback = radio_SidUSBSIDBufferSize_callback,
        .data     = (ui_callback_data_t)2048
    },
    {   .string   = "4096",
        .type     = MENU_ENTRY_RESOURCE_RADIO,
        .callback = radio_SidUSBSIDBufferSize_callback,
        .data     = (ui_callback_data_t)4096
    },
    {   .string   = "8192",
        .type     = MENU_ENTRY_RESOURCE_RADIO,
        .callback = radio_SidUSBSIDBufferSize_callback,
        .data     = (ui_callback_data_t)8192
    },
    {   .string   = "16384",
        .type     = MENU_ENTRY_RESOURCE_RADIO,
        .callback = radio_SidUSBSIDBufferSize_callback,
        .data     = (ui_callback_data_t)16384
    },
    SDL_MENU_LIST_END
};

# define VICE_SDL_USBSID_OPTIONS                                                                                              \
    {   .string   = "USBSID enable stereo audio mode",                                                                        \
        .type     = MENU_ENTRY_RESOURCE_TOGGLE,                                                                               \
        .callback = toggle_SidUSBSIDAudioMode_callback                                                                        \
    },                                                                                                                        \
    {   .string   = "USBSID enable read mode",                                                                                \
        .type     = MENU_ENTRY_RESOURCE_TOGGLE,                                                                               \
        .callback = toggle_SidUSBSIDReadMode_callback                                                                         \
    },                                                                                                                        \
    {   .string   = "USBSID buffer size",                                                                                     \
        .type     = MENU_ENTRY_SUBMENU,                                                                                       \
        .callback = submenu_radio_callback,                                                                                   \
        .data     = (ui_callback_data_t)us_buffsize_menu                                                                      \
    },                                                                                                                        \
    {   .string   = "USBSID diff size",                                                                                       \
        .type     = MENU_ENTRY_SUBMENU,                                                                                       \
        .callback = submenu_radio_callback,                                                                                   \
        .data     = (ui_callback_data_t)us_diffsize_menu                                                                      \
    },                                                                                                                        \


#endif /* HAVE_USBSID */


/* memory of the built menus, freed by uisid_menu_shutdown() */
static void **sid_menu_allocs = NULL;
static int sid_menu_num_allocs = 0;

static void *sid_menu_alloc(size_t size)
{
    void *p = lib_calloc(1, size);

    sid_menu_allocs = lib_realloc(sid_menu_allocs, (sid_menu_num_allocs + 1) * sizeof *sid_menu_allocs);
    sid_menu_allocs[sid_menu_num_allocs++] = p;
    return p;
}

static char *sid_menu_text(const char *fmt, int n)
{
    char *text = sid_menu_alloc(48);

    snprintf(text, 48, fmt, n);
    return text;
}

static ui_menu_entry_t sid_menu_item(char *string, ui_menu_entry_type_t type,
                                     ui_callback_t callback, ui_callback_data_t data)
{
    ui_menu_entry_t item;

    memset(&item, 0, sizeof item);
    item.action   = ACTION_NONE;
    item.string   = string;
    item.type     = type;
    item.callback = callback;
    item.data     = data;
    return item;
}

#if defined(HAVE_FASTSID) || defined(HAVE_RESID) || defined(HAVE_RESID_DTV) || defined(HAVE_RESIDFP)
UI_MENU_DEFINE_TOGGLE(SidFilters)
UI_MENU_DEFINE_RADIO(SidStereo)

/* The menus of the further SIDs, SID #2 and up, are built by
   uisid_menu_create(), one set for each SID the machine has. The data of
   their radio items holds the number of the SID (0 for the first) in the
   upper 16 bits and the value in the lower 16. */
#define SID_MENU_DATA(chipno, value) \
    ((ui_callback_data_t)vice_int_to_ptr(((chipno) << 16) | ((value) & 0xffff)))

static const char *sid_chip_radio(int activated, ui_callback_data_t param,
                                  const char *fmt, int value)
{
    char name[32];
    int current;

    snprintf(name, sizeof name, fmt, (vice_ptr_to_int(param) >> 16) + 1);
    if (activated) {
        resources_set_int(name, value);
    } else if (resources_get_int(name, &current) == 0 && current == value) {
        return sdl_menu_text_tick;
    }
    return NULL;
}

static UI_MENU_CALLBACK(radio_SidNAddressStart_callback)
{
    return sid_chip_radio(activated, param, "Sid%dAddressStart",
                          vice_ptr_to_int(param) & 0xffff);
}

static UI_MENU_CALLBACK(radio_SidNModel_callback)
{
    return sid_chip_radio(activated, param, "Sid%dModel",
                          (int)(int16_t)(vice_ptr_to_int(param) & 0xffff));
}

/* the address of a SID, for the title of its address menus: data is the
   number of the SID */
static UI_MENU_CALLBACK(show_SidAddress_callback)
{
    static char buf[20];
    char name[32];
    int value = 0;

    snprintf(name, sizeof name, "Sid%dAddressStart", vice_ptr_to_int(param) + 1);
    resources_get_int(name, &value);
    snprintf(buf, sizeof buf, "$%04x", (unsigned int)value);
    return buf;
}

/* the same for the item that opens them: data is the menu, whose title has
   the number of the SID */
static UI_MENU_CALLBACK(show_SidBase_callback)
{
    const ui_menu_entry_t *menu = (const ui_menu_entry_t *)param;

    return show_SidAddress_callback(0, menu[0].data);
}

static UI_MENU_CALLBACK(show_SidStereo_callback)
{
    static char buf[8];
    int value = 0;

    resources_get_int("SidStereo", &value);
    snprintf(buf, sizeof buf, "%d", value);
    return buf;
}

/* the "Extra SIDs" menu: 0 up to the number of further SIDs */
static ui_menu_entry_t *sid_stereo_menu_create(void)
{
    int max = sid_machine_get_max_sids();
    ui_menu_entry_t *menu = sid_menu_alloc((size_t)(max + 1) * sizeof *menu);
    int n;

    for (n = 0; n < max; n++) {
        menu[n] = sid_menu_item(sid_menu_text("%d", n), MENU_ENTRY_RESOURCE_RADIO,
                                radio_SidStereo_callback, (ui_callback_data_t)vice_int_to_ptr(n));
    }
    return menu;
}

/* the base address menu of a SID: a submenu for each page of I/O */
static ui_menu_entry_t *sid_address_menu_create(int chipno, int c128)
{
    static const int c64_pages[] = { 0xd4, 0xd5, 0xd6, 0xd7, 0xde, 0xdf };
    static const int c128_pages[] = { 0xd4, 0xd7, 0xde, 0xdf };
    const int *pages = c128 ? c128_pages : c64_pages;
    int num_pages = c128 ? (int)(sizeof c128_pages / sizeof c128_pages[0])
                         : (int)(sizeof c64_pages / sizeof c64_pages[0]);
    ui_menu_entry_t *menu = sid_menu_alloc((size_t)(num_pages + 2) * sizeof *menu);
    char *title = sid_menu_text("SID #%d base address", chipno + 1);
    int p;

    menu[0] = sid_menu_item(title, MENU_ENTRY_TEXT, show_SidAddress_callback,
                            (ui_callback_data_t)vice_int_to_ptr(chipno));
    for (p = 0; p < num_pages; p++) {
        /* $D400 is the first SID, so its page starts at $D420 */
        int first = (pages[p] == 0xd4) ? 1 : 0;
        ui_menu_entry_t *page = sid_menu_alloc((size_t)(8 - first + 2) * sizeof *page);
        int a;

        page[0] = sid_menu_item(title, MENU_ENTRY_TEXT, show_SidAddress_callback,
                                (ui_callback_data_t)vice_int_to_ptr(chipno));
        for (a = first; a < 8; a++) {
            int addr = (pages[p] << 8) | (a << 5);

            page[a - first + 1] = sid_menu_item(sid_menu_text("$%04X", addr), MENU_ENTRY_RESOURCE_RADIO,
                                                radio_SidNAddressStart_callback, SID_MENU_DATA(chipno, addr));
        }
        menu[p + 1] = sid_menu_item(sid_menu_text("$%02Xx0", pages[p]), MENU_ENTRY_SUBMENU,
                                    submenu_callback, (ui_callback_data_t)page);
    }
    return menu;
}

/* the model menu of a SID */
static ui_menu_entry_t *sid_chip_model_menu_create(int chipno)
{
    static const struct {
        const char *name;
        int model;
    } models[] = {
        { "Same as SID #1",    SID_MODEL_SAME_AS_FIRST },
        { "6581",              SID_MODEL_6581 },
        { "8580",              SID_MODEL_8580 },
        { "8580 + digi boost", SID_MODEL_8580D }
    };
    int n = (int)(sizeof models / sizeof models[0]);
    ui_menu_entry_t *menu = sid_menu_alloc((size_t)(n + 1) * sizeof *menu);
    int i;

    for (i = 0; i < n; i++) {
        menu[i] = sid_menu_item((char *)models[i].name, MENU_ENTRY_RESOURCE_RADIO,
                                radio_SidNModel_callback, SID_MENU_DATA(chipno, models[i].model));
    }
    return menu;
}

#endif /* defined(HAVE_FASTSID) || defined(HAVE_RESID) || defined(HAVE_RESID_DTV) || defined(HAVE_RESIDFP) */

/* fill a SID menu: "SID Model", the further SIDs, then the fixed rest */
static void sid_menu_build(ui_menu_entry_t *menu, const ui_menu_entry_t *tail,
                           size_t tail_len, ui_menu_entry_t *stereo_menu, int c128)
{
    int i = 0;
    int chipno;
    int max = sid_machine_get_max_sids();

    /* CAUTION: position is hardcoded below */
    menu[i++] = sid_menu_item("SID Model", MENU_ENTRY_SUBMENU, submenu_radio_callback, NULL);
#if defined(HAVE_FASTSID) || defined(HAVE_RESID) || defined(HAVE_RESID_DTV) || defined(HAVE_RESIDFP)
    if (max > 1) {
        menu[i++] = sid_menu_item("Extra SIDs", MENU_ENTRY_SUBMENU, show_SidStereo_callback,
                                  (ui_callback_data_t)stereo_menu);
        for (chipno = 1; chipno < max; chipno++) {
            menu[i++] = sid_menu_item(sid_menu_text("SID #%d base address", chipno + 1),
                                      MENU_ENTRY_SUBMENU, show_SidBase_callback,
                                      (ui_callback_data_t)sid_address_menu_create(chipno, c128));
        }
        for (chipno = 1; chipno < max; chipno++) {
            menu[i++] = sid_menu_item(sid_menu_text("SID #%d model", chipno + 1),
                                      MENU_ENTRY_SUBMENU, submenu_radio_callback,
                                      (ui_callback_data_t)sid_chip_model_menu_create(chipno));
        }
    }
#endif
    memcpy(menu + i, tail, tail_len * sizeof *tail);
}

/* the fixed rest of the menu, after the further SIDs */
static const ui_menu_entry_t sid_c64_menu_tail[] = {
#if defined(HAVE_FASTSID) || defined(HAVE_RESID) || defined(HAVE_RESID_DTV) || defined(HAVE_RESIDFP)
    {   .string   = "Emulate filters",
        .type     = MENU_ENTRY_RESOURCE_TOGGLE,
        .callback = toggle_SidFilters_callback,
    },
#endif
#ifdef HAVE_RESID
    VICE_SDL_RESID_OPTIONS
#endif
#ifdef HAVE_RESIDFP
    VICE_SDL_RESIDFP_OPTIONS
#endif
#ifdef HAVE_USBSID
    VICE_SDL_USBSID_OPTIONS
#endif
    SDL_MENU_LIST_END
};

/* built by uisid_menu_create(): "SID Model", for each further SID its base
   address and model, and the rest */
ui_menu_entry_t sid_c64_menu[2 + (2 * (SOUND_SIDS_MAX - 1))
                    + (sizeof sid_c64_menu_tail / sizeof sid_c64_menu_tail[0])];

/* the fixed rest of the menu, after the further SIDs */
static const ui_menu_entry_t sid_c128_menu_tail[] = {
#if defined(HAVE_FASTSID) || defined(HAVE_RESID) || defined(HAVE_RESID_DTV) || defined(HAVE_RESIDFP)
    {   .string   = "Emulate filters",
        .type     = MENU_ENTRY_RESOURCE_TOGGLE,
        .callback = toggle_SidFilters_callback
    },
#endif
#ifdef HAVE_RESID
    VICE_SDL_RESID_OPTIONS
#endif
#ifdef HAVE_RESIDFP
    VICE_SDL_RESIDFP_OPTIONS
#endif
#ifdef HAVE_USBSID
    VICE_SDL_USBSID_OPTIONS
#endif
    SDL_MENU_LIST_END
};

/* built by uisid_menu_create(): "SID Model", for each further SID its base
   address and model, and the rest */
ui_menu_entry_t sid_c128_menu[2 + (2 * (SOUND_SIDS_MAX - 1))
                    + (sizeof sid_c128_menu_tail / sizeof sid_c128_menu_tail[0])];

ui_menu_entry_t sid_cbm2_menu[] = {
    /* CAUTION: position is hardcoded below */
    {   .string   = "SID Model",
        .type     = MENU_ENTRY_SUBMENU,
        .callback = submenu_radio_callback
    },
#if defined(HAVE_FASTSID) || defined(HAVE_RESID) || defined(HAVE_RESID_DTV) || defined(HAVE_RESIDFP)
    {   .string   = "Emulate filters",
        .type     = MENU_ENTRY_RESOURCE_TOGGLE,
        .callback = toggle_SidFilters_callback
    },
#endif
#ifdef HAVE_RESID
    VICE_SDL_RESID_OPTIONS
#endif
#ifdef HAVE_RESIDFP
    VICE_SDL_RESIDFP_OPTIONS
#endif
#ifdef HAVE_USBSID
    VICE_SDL_USBSID_OPTIONS
#endif
    SDL_MENU_LIST_END
};

ui_menu_entry_t sid_dtv_menu[] = {
    /* CAUTION: position is hardcoded below */
    {   .string   = "SID Model",
        .type     = MENU_ENTRY_SUBMENU,
        .callback = submenu_radio_callback
    },
#if defined(HAVE_FASTSID) || defined(HAVE_RESID) || defined(HAVE_RESID_DTV) || defined(HAVE_RESIDFP)
    {   .string   = "Emulate filters",
        .type     = MENU_ENTRY_RESOURCE_TOGGLE,
        .callback = toggle_SidFilters_callback
    },
#endif
#ifdef HAVE_RESID
    VICE_SDL_RESID_OPTIONS
#endif
#ifdef HAVE_RESIDFP
    VICE_SDL_RESIDFP_OPTIONS
#endif
#ifdef HAVE_USBSID
    VICE_SDL_USBSID_OPTIONS
#endif
    SDL_MENU_LIST_END
};

UI_MENU_DEFINE_TOGGLE(SidCart)
UI_MENU_DEFINE_RADIO(SidAddress)
UI_MENU_DEFINE_RADIO(SidClock)

ui_menu_entry_t sid_vic_menu[] = {
    {   .string   = "Enable SID cartridge emulation",
        .type     = MENU_ENTRY_RESOURCE_TOGGLE,
        .callback = toggle_SidCart_callback
    },
    /* CAUTION: position is hardcoded below */
    {   .string   = "SID Model",
        .type     = MENU_ENTRY_SUBMENU,
        .callback = submenu_radio_callback
    },
#if defined(HAVE_FASTSID) || defined(HAVE_RESID) || defined(HAVE_RESID_DTV) || defined(HAVE_RESIDFP)
    {   .string   = "Emulate filters",
        .type     = MENU_ENTRY_RESOURCE_TOGGLE,
        .callback = toggle_SidFilters_callback
    },
#endif
#ifdef HAVE_RESID
    VICE_SDL_RESID_OPTIONS
#endif
#ifdef HAVE_RESIDFP
    VICE_SDL_RESIDFP_OPTIONS
#endif
#ifdef HAVE_USBSID
    VICE_SDL_USBSID_OPTIONS
#endif
    SDL_MENU_ITEM_SEPARATOR,
    SDL_MENU_ITEM_TITLE("SID address"),
    {   .string   = "$9800",
        .type     = MENU_ENTRY_RESOURCE_RADIO,
        .callback = radio_SidAddress_callback,
        .data     = (ui_callback_data_t)0x9800
    },
    {   .string   = "$9C00",
        .type     = MENU_ENTRY_RESOURCE_RADIO,
        .callback = radio_SidAddress_callback,
        .data     = (ui_callback_data_t)0x9c00
    },
    SDL_MENU_ITEM_SEPARATOR,

    SDL_MENU_ITEM_TITLE("SID clock"),
    {   .string   = "C64",
        .type     = MENU_ENTRY_RESOURCE_RADIO,
        .callback = radio_SidClock_callback,
        .data     = (ui_callback_data_t)SIDCART_CLOCK_C64
    },
    {   .string   = "VIC20",
        .type     = MENU_ENTRY_RESOURCE_RADIO,
        .callback = radio_SidClock_callback,
        .data     = (ui_callback_data_t)SIDCART_CLOCK_NATIVE
    },
    SDL_MENU_LIST_END
};

ui_menu_entry_t sid_pet_menu[] = {
    {   .string   = "Enable SID cartridge emulation",
        .type     = MENU_ENTRY_RESOURCE_TOGGLE,
        .callback = toggle_SidCart_callback
    },
    /* CAUTION: position is hardcoded below */
    {   .string   = "SID Model",
        .type     = MENU_ENTRY_SUBMENU,
        .callback = submenu_radio_callback
    },
#if defined(HAVE_FASTSID) || defined(HAVE_RESID) || defined(HAVE_RESID_DTV) || defined(HAVE_RESIDFP)
    {   .string   = "Emulate filters",
        .type     = MENU_ENTRY_RESOURCE_TOGGLE,
        .callback = toggle_SidFilters_callback
    },
#endif
#ifdef HAVE_RESID
    VICE_SDL_RESID_OPTIONS
#endif
#ifdef HAVE_RESIDFP
    VICE_SDL_RESIDFP_OPTIONS
#endif
#ifdef HAVE_USBSID
    VICE_SDL_USBSID_OPTIONS
#endif
    SDL_MENU_ITEM_SEPARATOR,
    SDL_MENU_ITEM_TITLE("SID address"),
    {   .string   = "$8F00",
        .type     = MENU_ENTRY_RESOURCE_RADIO,
        .callback = radio_SidAddress_callback,
        .data     = (ui_callback_data_t)0x8f00
    },
    {   .string   = "$E900",
        .type     = MENU_ENTRY_RESOURCE_RADIO,
        .callback = radio_SidAddress_callback,
        .data     = (ui_callback_data_t)0xe900
    },
    SDL_MENU_ITEM_SEPARATOR,

    SDL_MENU_ITEM_TITLE("SID clock"),
    {   .string   = "C64",
        .type     = MENU_ENTRY_RESOURCE_RADIO,
        .callback = radio_SidClock_callback,
        .data     = (ui_callback_data_t)SIDCART_CLOCK_C64
    },
    {   .string   = "PET",
        .type     = MENU_ENTRY_RESOURCE_RADIO,
        .callback = radio_SidClock_callback,
        .data     = (ui_callback_data_t)SIDCART_CLOCK_NATIVE
    },
    SDL_MENU_LIST_END
};

UI_MENU_DEFINE_TOGGLE(DIGIBLASTER)

ui_menu_entry_t sid_plus4_menu[] = {
    {   .string   = "Enable SID cartridge emulation",
        .type     = MENU_ENTRY_RESOURCE_TOGGLE,
        .callback = toggle_SidCart_callback
    },
    /* CAUTION: position is hardcoded below */
    {   .string   = "SID Model",
        .type     = MENU_ENTRY_SUBMENU,
        .callback = submenu_radio_callback
    },
#if defined(HAVE_FASTSID) || defined(HAVE_RESID) || defined(HAVE_RESID_DTV) || defined(HAVE_RESIDFP)
    {   .string   = "Emulate filters",
        .type     = MENU_ENTRY_RESOURCE_TOGGLE,
        .callback = toggle_SidFilters_callback
    },
#endif
#ifdef HAVE_RESID
    VICE_SDL_RESID_OPTIONS
#endif
#ifdef HAVE_RESIDFP
    VICE_SDL_RESIDFP_OPTIONS
#endif
#ifdef HAVE_USBSID
    VICE_SDL_USBSID_OPTIONS
#endif
    SDL_MENU_ITEM_SEPARATOR,

    SDL_MENU_ITEM_TITLE("SID address"),
    {   .string   = "$FD40",
        .type     = MENU_ENTRY_RESOURCE_RADIO,
        .callback = radio_SidAddress_callback,
        .data     = (ui_callback_data_t)0xfd40
    },
    {   .string   = "$FE80",
        .type     = MENU_ENTRY_RESOURCE_RADIO,
        .callback = radio_SidAddress_callback,
        .data     = (ui_callback_data_t)0xfe80
    },
    SDL_MENU_ITEM_SEPARATOR,

    SDL_MENU_ITEM_TITLE("SID clock"),
    {   .string   = "C64",
        .type     = MENU_ENTRY_RESOURCE_RADIO,
        .callback = radio_SidClock_callback,
        .data     = (ui_callback_data_t)SIDCART_CLOCK_C64
    },
    {   .string   = "PLUS4",
        .type     = MENU_ENTRY_RESOURCE_RADIO,
        .callback = radio_SidClock_callback,
        .data     = (ui_callback_data_t)SIDCART_CLOCK_NATIVE
    },
    SDL_MENU_ITEM_SEPARATOR,

    {   .string   = "Enable SID cartridge digiblaster add-on",
        .type     = MENU_ENTRY_RESOURCE_TOGGLE,
        .callback = toggle_DIGIBLASTER_callback
    },
    SDL_MENU_LIST_END
};


void uisid_menu_create(void)
{
    sid_engine_model_t **list = sid_get_engine_model_list();
    int i;

    ui_menu_entry_t *stereo_menu = NULL;

#if defined(HAVE_FASTSID) || defined(HAVE_RESID) || defined(HAVE_RESID_DTV) || defined(HAVE_RESIDFP)
    stereo_menu = sid_stereo_menu_create();
#endif
    sid_menu_build(sid_c64_menu, sid_c64_menu_tail,
                   sizeof sid_c64_menu_tail / sizeof sid_c64_menu_tail[0], stereo_menu, 0);
    sid_menu_build(sid_c128_menu, sid_c128_menu_tail,
                   sizeof sid_c128_menu_tail / sizeof sid_c128_menu_tail[0], stereo_menu, 1);

    /* create "Sid Model" menu */
    for (i = 0; list[i]; ++i) {}

    sid_model_menu = lib_malloc((i + 1) * sizeof(ui_menu_entry_t));

    for (i = 0; list[i]; ++i) {
        sid_model_menu[i].action   = ACTION_NONE;
        sid_model_menu[i].string   = (char*)list[i]->name;
        sid_model_menu[i].type     = MENU_ENTRY_RESOURCE_RADIO;
        sid_model_menu[i].callback = custom_SidModel_callback;
        sid_model_menu[i].data     = (ui_callback_data_t)vice_int_to_ptr(list[i]->value);
    }
    sid_model_menu[i].string = NULL;

    sid_c64_menu[0].data   = (ui_callback_data_t)sid_model_menu;
    sid_c128_menu[0].data  = (ui_callback_data_t)sid_model_menu;
    sid_cbm2_menu[0].data  = (ui_callback_data_t)sid_model_menu;
    sid_dtv_menu[0].data   = (ui_callback_data_t)sid_model_menu;
    sid_vic_menu[1].data   = (ui_callback_data_t)sid_model_menu;
    sid_pet_menu[1].data   = (ui_callback_data_t)sid_model_menu;
    sid_plus4_menu[1].data = (ui_callback_data_t)sid_model_menu;

#ifdef HAVE_RESIDFP
    /* create "SID Profile" Menu */
    for (i = 0; filterRangeMap[i].name; ++i) {}

    sid_profile_menu = lib_malloc((i + 1) * sizeof(ui_menu_entry_t));

    for (i = 0; filterRangeMap[i].name; ++i) {
        sid_profile_menu[i].action   = ACTION_NONE;
        sid_profile_menu[i].string   = (char*)filterRangeMap[i].name;
        sid_profile_menu[i].type     = MENU_ENTRY_RESOURCE_RADIO;
        sid_profile_menu[i].callback = custom_SidProfile_callback;
        sid_profile_menu[i].data     = (ui_callback_data_t)vice_int_to_ptr(i);
    }
    sid_profile_menu[i].string = NULL;

    for (i = 0; (sid_c64_menu[i].data != (ui_callback_data_t)0xdeadc0de); ++i) {}
    sid_c64_menu[i].data   = (ui_callback_data_t)sid_profile_menu;
    for (i = 0; (sid_c128_menu[i].data != (ui_callback_data_t)0xdeadc0de); ++i) {}
    sid_c128_menu[i].data  = (ui_callback_data_t)sid_profile_menu;
    for (i = 0; (sid_cbm2_menu[i].data != (ui_callback_data_t)0xdeadc0de); ++i) {}
    sid_cbm2_menu[i].data  = (ui_callback_data_t)sid_profile_menu;
    for (i = 0; (sid_dtv_menu[i].data != (ui_callback_data_t)0xdeadc0de); ++i) {}
    sid_dtv_menu[i].data   = (ui_callback_data_t)sid_profile_menu;
    for (i = 0; (sid_vic_menu[i].data != (ui_callback_data_t)0xdeadc0de); ++i) {}
    sid_vic_menu[i].data   = (ui_callback_data_t)sid_profile_menu;
    for (i = 0; (sid_pet_menu[i].data != (ui_callback_data_t)0xdeadc0de); ++i) {}
    sid_pet_menu[i].data   = (ui_callback_data_t)sid_profile_menu;
    for (i = 0; (sid_plus4_menu[i].data != (ui_callback_data_t)0xdeadc0de); ++i) {}
    sid_plus4_menu[i].data = (ui_callback_data_t)sid_profile_menu;
#endif
}

/** \brief  Clean up memory used by the SID model menu
 */
void uisid_menu_shutdown(void)
{
    int i;

    for (i = 0; i < sid_menu_num_allocs; i++) {
        lib_free(sid_menu_allocs[i]);
    }
    lib_free(sid_menu_allocs);
    sid_menu_allocs = NULL;
    sid_menu_num_allocs = 0;

    if (sid_model_menu != NULL) {
        lib_free(sid_model_menu);
    }
#ifdef HAVE_RESIDFP
    if (sid_profile_menu != NULL) {
        lib_free(sid_profile_menu);
    }
#endif
}
