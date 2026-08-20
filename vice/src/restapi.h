/** \file   restapi.h
 *  \brief  Ultimate64/1541-Ultimate compatible REST API server - interface
 *
 *  \author Enver Haase <enver.haase@infraleap.com>
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

#ifndef VICE_RESTAPI_H
#define VICE_RESTAPI_H

int restapi_resources_init(void);
void restapi_resources_shutdown(void);
int restapi_cmdline_options_init(void);

/** \brief  Serve pending REST requests
 *
 * Called once per emulated frame from the machine's vsync hook, so handlers run
 * on the emulation thread at a point where the machine state is consistent, and
 * from the UI's pause loop through vsync_pause_hook(), so a paused machine can
 * be resumed over the API that paused it.
 */
void restapi_vsync_hook(void);

/** \brief  Name this emulator reports to REST API clients
 *
 * The RESTAPIHostname resource when set, otherwise the name of the machine VICE
 * runs on. Clients use it to tell one device from another.
 */
const char *restapi_get_hostname(void);

#endif
