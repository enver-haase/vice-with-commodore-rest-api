/** \file   restapi.c
 *  \brief  Ultimate64/1541-Ultimate compatible REST API server
 *
 *  \author Enver Haase <enver.haase@infraleap.com>
 *
 *  Exposes the HTTP API of the 1541-Ultimate / Ultimate64 hardware, so that
 *  launchers and tools written against a real device can drive VICE unchanged.
 *
 *  The server is off by default and binds to localhost when enabled: unlike the
 *  appliance it imitates, VICE has no business listening on the network unless
 *  its user asked for it.
 *
 *  Requests are served from the vsync hook, once per emulated frame, which is
 *  the same approach the remote monitor takes. Handlers therefore run on the
 *  emulation thread and may touch machine state directly.
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
#include "archdep_hostname.h"
#include "cmdline.h"
#include "lib.h"
#include "log.h"
#include "resources.h"
#include "restapi.h"
#include "restapi_http.h"
#include "restapi_routes.h"
#include "util.h"

#ifdef HAVE_NETWORK

#include "vicesocket.h"

/** \brief  Number of clients served at the same time */
#define MAX_CONNECTIONS 4

/** \brief  Largest request we are willing to buffer, uploads included */
#define MAX_REQUEST_SIZE (32 * 1024 * 1024)

/** \brief  Bytes read from a socket per attempt */
#define READ_CHUNK 65536

/** \brief  Seconds a connection may make no progress before it is dropped
 *
 * A client that connects and then says nothing would otherwise hold its slot
 * for the lifetime of the emulator, and four of them would wedge the API.
 */
#define IDLE_TIMEOUT_SECONDS 30

typedef struct connection_s {
    vice_network_socket_t *socket;
    char *buffer;
    size_t length;      /* bytes in buffer */
    size_t alloc;       /* bytes allocated */
    size_t header_end;  /* offset of the first body byte, 0 while unknown */
    size_t header_length;   /* header block without the blank line */
    long content_length;
    tick_t last_progress;   /* when this connection last delivered bytes */
} connection_t;

static vice_network_socket_t *listen_socket = NULL;
static connection_t connections[MAX_CONNECTIONS];

static char *restapi_server_address = NULL;
static char *restapi_password = NULL;
static char *restapi_hostname = NULL;
static int restapi_enabled = 0;

/* ------------------------------------------------------------------------- */
/* connection bookkeeping                                                    */

static void connection_close(connection_t *conn)
{
    if (conn->socket != NULL) {
        vice_network_socket_close(conn->socket);
        conn->socket = NULL;
    }
    lib_free(conn->buffer);
    conn->buffer = NULL;
    conn->length = 0;
    conn->alloc = 0;
    conn->header_end = 0;
    conn->header_length = 0;
    conn->content_length = -1;
}

static void send_all(connection_t *conn, const char *data, size_t length)
{
    size_t sent = 0;
    int attempts = 0;

    while (sent < length && conn->socket != NULL && attempts < 1000) {
        ssize_t count = vice_network_send(conn->socket, data + sent,
                                         length - sent, 0);
        if (count < 0) {
            log_message(LOG_DEFAULT, "restapi: send failed, closing connection");
            break;
        }
        if (count == 0) {
            attempts++;
        } else {
            sent += (size_t)count;
        }
    }
}

/** \brief  Answer with a bare status, for errors found before routing */
static void send_status(connection_t *conn, int status, const char *message)
{
    restapi_response_t resp;
    char *rendered;
    size_t length;

    restapi_response_init(&resp);
    resp.status = status;
    restapi_error(&resp, "%s", message);
    rendered = restapi_response_render(&resp, &length);
    send_all(conn, rendered, length);
    lib_free(rendered);
    restapi_response_free(&resp);
}

/* ------------------------------------------------------------------------- */
/* request handling                                                          */

/** \brief  Locate the end of the header block in \a conn's buffer
 *
 * \return  offset of the first body byte, or 0 when the headers are incomplete
 */
static size_t find_header_end(connection_t *conn, size_t *separator_length)
{
    size_t i;

    for (i = 0; i + 1 < conn->length; i++) {
        if (conn->buffer[i] == '\n' && conn->buffer[i + 1] == '\n') {
            *separator_length = 2;
            return i + 2;
        }
        if (i + 3 < conn->length
            && conn->buffer[i] == '\r' && conn->buffer[i + 1] == '\n'
            && conn->buffer[i + 2] == '\r' && conn->buffer[i + 3] == '\n') {
            *separator_length = 4;
            return i + 4;
        }
    }
    return 0;
}

/** \brief  Run the request in \a conn's buffer and answer it */
static void serve_request(connection_t *conn, size_t header_length)
{
    restapi_request_t req;
    restapi_response_t resp;
    char *headers;
    char *rendered;
    size_t length;

    memset(&req, 0, sizeof(req));
    restapi_response_init(&resp);

    /* the header block is text, the body may be binary, so they are handled
       separately */
    headers = lib_malloc(header_length + 1);
    memcpy(headers, conn->buffer, header_length);
    headers[header_length] = '\0';

    if (restapi_parse_headers(&req, headers) < 0) {
        resp.status = RESTAPI_HTTP_NOT_FOUND;
        restapi_error(&resp, "Not a supported API endpoint.");
    } else if (req.method == RESTAPI_METHOD_UNKNOWN) {
        resp.status = RESTAPI_HTTP_NOT_IMPLEMENTED;
        restapi_error(&resp, "Unsupported method.");
    } else if (restapi_password != NULL && *restapi_password != '\0'
               && (req.password == NULL
                   || strcmp(req.password, restapi_password) != 0)) {
        resp.status = RESTAPI_HTTP_FORBIDDEN;
        restapi_error(&resp, "Forbidden.");
    } else {
        if (conn->length > conn->header_end) {
            req.body_length = conn->length - conn->header_end;
            /* anything past Content-Length is not ours: a client may append a
               stray CRLF, or pipeline a second request behind this one */
            if (conn->content_length >= 0
                && req.body_length > (size_t)conn->content_length) {
                req.body_length = (size_t)conn->content_length;
            }
            req.body = lib_malloc(req.body_length);
            memcpy(req.body, conn->buffer + conn->header_end, req.body_length);
            restapi_parse_body(&req);
        }
        restapi_routes_dispatch(&req, &resp);
    }
    lib_free(headers);

    rendered = restapi_response_render(&resp, &length);
    send_all(conn, rendered, length);
    lib_free(rendered);

    restapi_request_free(&req);
    restapi_response_free(&resp);

    if (restapi_routes_quit_requested()) {
        /* machine:poweroff; quit the way the monitor's "quit" does, from this
           same thread, once the client has its answer */
        connection_close(conn);
        archdep_vice_exit(0);
    }
}

/** \brief  Read whatever is pending on \a conn and serve a completed request */
static void connection_poll(connection_t *conn)
{
    int eof = 0;

    while (vice_network_select_poll_one(conn->socket) > 0) {
        ssize_t count;

        if (conn->length + READ_CHUNK > conn->alloc) {
            if (conn->length + READ_CHUNK > MAX_REQUEST_SIZE) {
                send_status(conn, RESTAPI_HTTP_PAYLOAD_TOO_LARGE,
                            "Request too large.");
                connection_close(conn);
                return;
            }
            conn->alloc = conn->length + READ_CHUNK;
            conn->buffer = lib_realloc(conn->buffer, conn->alloc);
        }

        count = vice_network_receive(conn->socket, conn->buffer + conn->length,
                                    READ_CHUNK, 0);
        if (count <= 0) {
            /* the peer closed its side or the connection broke; whatever has
               arrived so far is all we are going to get */
            eof = 1;
            break;
        }
        conn->length += (size_t)count;
        conn->last_progress = tick_now();
    }

    if (!eof
        && tick_now_delta(conn->last_progress)
           > tick_per_second() * IDLE_TIMEOUT_SECONDS) {
        log_message(LOG_DEFAULT, "restapi: dropping a connection that stalled");
        connection_close(conn);
        return;
    }

    if (conn->length == 0) {
        if (eof) {
            connection_close(conn);
        }
        return;
    }

    if (conn->header_end == 0) {
        size_t separator_length = 0;
        size_t header_end = find_header_end(conn, &separator_length);
        restapi_request_t probe;
        char *headers;
        int chunked;

        if (header_end == 0) {
            if (eof) {
                connection_close(conn);     /* truncated request */
            }
            return;
        }
        conn->header_end = header_end;
        conn->header_length = header_end - separator_length;

        /* peek at the headers to know whether to wait for a body */
        memset(&probe, 0, sizeof(probe));
        headers = lib_malloc(conn->header_length + 1);
        memcpy(headers, conn->buffer, conn->header_length);
        headers[conn->header_length] = '\0';
        conn->content_length = -1;
        chunked = 0;
        if (restapi_parse_headers(&probe, headers) == 0) {
            conn->content_length = probe.content_length;
            chunked = probe.chunked;
        }
        lib_free(headers);
        restapi_request_free(&probe);

        if (chunked) {
            /* not worth implementing: every client of this API announces a
               Content-Length */
            send_status(conn, RESTAPI_HTTP_NOT_IMPLEMENTED,
                        "Chunked transfer encoding is not supported.");
            connection_close(conn);
            return;
        }
    }

    if (conn->content_length > 0
        && conn->length < conn->header_end + (size_t)conn->content_length) {
        if (eof) {
            connection_close(conn);         /* truncated body */
        }
        return;                             /* body still incomplete */
    }

    serve_request(conn, conn->header_length);
    connection_close(conn);
}

void restapi_vsync_hook(void)
{
    /* also called from the pause loop, where a handler that pauses or resumes
       could otherwise re-enter this while it is walking the connection list */
    static int serving = 0;
    int i;

    if (listen_socket == NULL || serving) {
        return;
    }
    serving = 1;

    if (vice_network_select_poll_one(listen_socket) > 0) {
        vice_network_socket_t *incoming = vice_network_accept(listen_socket);

        if (incoming != NULL) {
            connection_t *slot = NULL;

            for (i = 0; i < MAX_CONNECTIONS; i++) {
                if (connections[i].socket == NULL) {
                    slot = &connections[i];
                    break;
                }
            }
            if (slot != NULL) {
                memset(slot, 0, sizeof(connection_t));
                slot->socket = incoming;
                slot->content_length = -1;
                slot->last_progress = tick_now();
            } else {
                log_message(LOG_DEFAULT,
                            "restapi: too many connections, rejecting client");
                vice_network_socket_close(incoming);
            }
        }
    }

    for (i = 0; i < MAX_CONNECTIONS; i++) {
        if (connections[i].socket != NULL) {
            connection_poll(&connections[i]);
        }
    }

    serving = 0;
}

/* ------------------------------------------------------------------------- */
/* server lifecycle                                                          */

static int restapi_activate(void)
{
    vice_network_socket_address_t *server_address = NULL;
    int error = -1;

    if (restapi_server_address == NULL || *restapi_server_address == '\0') {
        log_error(LOG_DEFAULT, "restapi: no server address set");
        return -1;
    }

    server_address = vice_network_address_generate(restapi_server_address, 0);
    if (server_address == NULL) {
        log_error(LOG_DEFAULT, "restapi: invalid server address '%s'",
                  restapi_server_address);
        return -1;
    }

    listen_socket = vice_network_server(server_address);
    if (listen_socket == NULL) {
        log_error(LOG_DEFAULT, "restapi: could not listen on '%s'",
                  restapi_server_address);
    } else {
        log_message(LOG_DEFAULT, "restapi: listening on %s",
                    restapi_server_address);
        error = 0;
    }

    vice_network_address_close(server_address);
    return error;
}

static int restapi_deactivate(void)
{
    int i;

    for (i = 0; i < MAX_CONNECTIONS; i++) {
        connection_close(&connections[i]);
    }
    if (listen_socket != NULL) {
        vice_network_socket_close(listen_socket);
        listen_socket = NULL;
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* resources                                                                 */

static int set_restapi_enabled(int value, void *param)
{
    int enabled = value ? 1 : 0;

    if (enabled == restapi_enabled) {
        return 0;
    }
    if (enabled) {
        if (restapi_activate() < 0) {
            return -1;
        }
    } else {
        restapi_deactivate();
    }
    restapi_enabled = enabled;
    return 0;
}

static int set_restapi_server_address(const char *name, void *param)
{
    if (restapi_server_address != NULL && name != NULL
        && strcmp(name, restapi_server_address) == 0) {
        return 0;
    }
    if (restapi_enabled) {
        restapi_deactivate();
    }
    util_string_set(&restapi_server_address, name);
    if (restapi_enabled) {
        return restapi_activate();
    }
    return 0;
}

static int set_restapi_password(const char *name, void *param)
{
    util_string_set(&restapi_password, name);
    return 0;
}

static int set_restapi_hostname(const char *name, void *param)
{
    util_string_set(&restapi_hostname, name);
    return 0;
}

const char *restapi_get_hostname(void)
{
    static char system_name[256];

    /* the hardware reports its configured network name here, and clients use it
       to tell one device from another, so a fixed string would make every
       running emulator look like the same machine */
    if (restapi_hostname != NULL && *restapi_hostname != '\0') {
        return restapi_hostname;
    }
    if (archdep_get_hostname(system_name, sizeof system_name) == 0) {
        return system_name;
    }
    return "vice";
}

static const resource_string_t resources_string[] = {
    { "RESTAPIServerAddress", "ip4://127.0.0.1:8080", RES_EVENT_NO, NULL,
      &restapi_server_address, set_restapi_server_address, NULL },
    { "RESTAPIPassword", "", RES_EVENT_NO, NULL,
      &restapi_password, set_restapi_password, NULL },
    { "RESTAPIHostname", "", RES_EVENT_NO, NULL,
      &restapi_hostname, set_restapi_hostname, NULL },
    RESOURCE_STRING_LIST_END
};

static const resource_int_t resources_int[] = {
    { "RESTAPIServer", 0, RES_EVENT_STRICT, (resource_value_t)0,
      &restapi_enabled, set_restapi_enabled, NULL },
    RESOURCE_INT_LIST_END
};

int restapi_resources_init(void)
{
    if (resources_register_string(resources_string) < 0) {
        return -1;
    }
    return resources_register_int(resources_int);
}

void restapi_resources_shutdown(void)
{
    restapi_deactivate();
    restapi_routes_shutdown();
    lib_free(restapi_server_address);
    restapi_server_address = NULL;
    lib_free(restapi_password);
    restapi_password = NULL;
    lib_free(restapi_hostname);
    restapi_hostname = NULL;
}

/* ------------------------------------------------------------------------- */
/* command line options                                                      */

static const cmdline_option_t cmdline_options[] =
{
    { "-restapi", SET_RESOURCE, CMDLINE_ATTRIB_NONE,
      NULL, NULL, "RESTAPIServer", (resource_value_t)1,
      NULL, "Enable the Ultimate64 compatible REST API server" },
    { "+restapi", SET_RESOURCE, CMDLINE_ATTRIB_NONE,
      NULL, NULL, "RESTAPIServer", (resource_value_t)0,
      NULL, "Disable the Ultimate64 compatible REST API server" },
    { "-restapiaddress", SET_RESOURCE, CMDLINE_ATTRIB_NEED_ARGS,
      NULL, NULL, "RESTAPIServerAddress", NULL,
      "<Name>", "The local address the REST API server should bind to" },
    { "-restapipassword", SET_RESOURCE, CMDLINE_ATTRIB_NEED_ARGS,
      NULL, NULL, "RESTAPIPassword", NULL,
      "<Password>", "Require this password in the X-Password header (empty: no password)" },
    { "-restapihostname", SET_RESOURCE, CMDLINE_ATTRIB_NEED_ARGS,
      NULL, NULL, "RESTAPIHostname", NULL,
      "<Name>", "Name this emulator reports to REST API clients (empty: the host's own name)" },
    CMDLINE_LIST_END
};

int restapi_cmdline_options_init(void)
{
    return cmdline_register_options(cmdline_options);
}

#else   /* !HAVE_NETWORK */

int restapi_resources_init(void)
{
    return 0;
}

void restapi_resources_shutdown(void)
{
}

int restapi_cmdline_options_init(void)
{
    return 0;
}

void restapi_vsync_hook(void)
{
}

const char *restapi_get_hostname(void)
{
    return "vice";
}

#endif
