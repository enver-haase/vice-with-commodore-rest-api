/** \file   archdep_hostname.c
 * \brief   Get the name of the machine VICE runs on
 * \author  Enver Haase <enver.haase@infraleap.com>
 *
 * OS support:
 *  - Linux
 *  - Windows
 *  - BSD
 *  - MacOS
 *  - Haiku
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
#include "archdep_defs.h"

#include <string.h>

#if defined(UNIX_COMPILE) || defined(HAIKU_COMPILE)
# include <unistd.h>
#elif defined(WINDOWS_COMPILE)
/* gethostname() lives in winsock on Windows, unlike the unix systems where it
   comes with <unistd.h> */
# include <winsock2.h>
#else
# error "Unsupported OS!"
#endif

#include "archdep_hostname.h"


/** \brief  Get the name of the machine VICE runs on
 *
 * \param[out]  name    buffer receiving the name
 * \param[in]   size    size of \a name, terminator included
 *
 * \return  0 on success, -1 on failure
 *
 * \note    On failure \a name is set to the empty string, so the result is safe
 *          to use either way.
 */
int archdep_get_hostname(char *name, size_t size)
{
    if (name == NULL || size == 0) {
        return -1;
    }
    name[0] = '\0';

#if defined(UNIX_COMPILE) || defined(HAIKU_COMPILE) || defined(WINDOWS_COMPILE)
    if (gethostname(name, (int)size) != 0) {
        name[0] = '\0';
        return -1;
    }
    /* not every implementation terminates when the name does not fit */
    name[size - 1] = '\0';
    if (name[0] == '\0') {
        return -1;
    }
    return 0;
#else
    return -1;
#endif
}
