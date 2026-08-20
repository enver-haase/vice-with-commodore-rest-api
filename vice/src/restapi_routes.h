/** \file   restapi_routes.h
 *  \brief  Route table of the Ultimate64 compatible REST API - interface
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

#ifndef VICE_RESTAPI_ROUTES_H
#define VICE_RESTAPI_ROUTES_H

#include "restapi_http.h"

/** \brief  Run the handler matching \a req and fill in \a resp */
void restapi_routes_dispatch(restapi_request_t *req, restapi_response_t *resp);

/** \brief  Release resources the handlers hold on to, such as mounted uploads */
void restapi_routes_shutdown(void);

#endif
