/** \file   restapi_sidplay.h
 *  \brief  runners:sidplay of the Ultimate64 compatible REST API - interface
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

#ifndef VICE_RESTAPI_SIDPLAY_H
#define VICE_RESTAPI_SIDPLAY_H

#include "restapi_http.h"

/** \brief  PUT and POST /v1/runners:sidplay */
void restapi_sidplay(restapi_request_t *req, restapi_response_t *resp);

/** \brief  Advance a started tune through the player's handshake
 *
 * Called from the server's vsync hook, once per frame.
 */
void restapi_sidplay_poll(void);

/** \brief  Remove the player cartridge if it is still attached */
void restapi_sidplay_shutdown(void);

#endif
