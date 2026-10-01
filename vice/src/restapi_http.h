/** \file   restapi_http.h
 *  \brief  Minimal HTTP request parsing and JSON response building for the
 *          Ultimate64/1541-Ultimate compatible REST API - interface
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

#ifndef VICE_RESTAPI_HTTP_H
#define VICE_RESTAPI_HTTP_H

#include <stddef.h>

/* HTTP status codes used by the API */
#define RESTAPI_HTTP_OK                     200
#define RESTAPI_HTTP_BAD_REQUEST            400
#define RESTAPI_HTTP_FORBIDDEN              403
#define RESTAPI_HTTP_NOT_FOUND              404
#define RESTAPI_HTTP_PRECONDITION_FAILED    412
#define RESTAPI_HTTP_PAYLOAD_TOO_LARGE      413
#define RESTAPI_HTTP_UNSUPPORTED_MEDIA_TYPE 415
#define RESTAPI_HTTP_INTERNAL_ERROR         500
#define RESTAPI_HTTP_NOT_IMPLEMENTED        501

/* limits */
#define RESTAPI_MAX_PATH_PARTS  16
#define RESTAPI_MAX_PARAMS      16
#define RESTAPI_MAX_UPLOADS      2

typedef enum restapi_method_e {
    RESTAPI_METHOD_UNKNOWN = 0,
    RESTAPI_METHOD_GET,
    RESTAPI_METHOD_PUT,
    RESTAPI_METHOD_POST
} restapi_method_t;

typedef struct restapi_param_s {
    char *name;
    char *value;
} restapi_param_t;

/** \brief  A parsed request
 *
 * The URL is split the same way the Ultimate firmware's HTTP core
 * (GideonZ/MicroHttpServer, lib/url.c) splits it:
 *
 *   /v1/<route>[/<path>][:<command>][?<querystring>]
 *
 * The querystring is separated first, then the command, then the path, so a
 * ':' inside the querystring is not a command separator. An absent command
 * becomes "none".
 */
typedef struct restapi_request_s {
    restapi_method_t method;

    char *url;                                  /* raw request target */
    char *route;                                /* eg "machine", "drives" */
    char *command;                              /* eg "reset", or "none" */
    char *path;                                 /* undivided, url-decoded */
    char *path_parts[RESTAPI_MAX_PATH_PARTS];   /* path split on '/' */
    int path_count;

    restapi_param_t params[RESTAPI_MAX_PARAMS];
    int param_count;

    char *content_type;
    char *password;                             /* X-Password header */
    int chunked;                                /* Transfer-Encoding: chunked */
    long content_length;                        /* -1 when absent */

    char *body;
    size_t body_length;

    /* temp files extracted from a multipart/form-data body; removed again when
       the request is freed, unless a handler took ownership */
    char *uploads[RESTAPI_MAX_UPLOADS];
    int upload_count;
} restapi_request_t;

/** \brief  A response under construction
 *
 * Every API response is a JSON object which always ends in an "errors" array,
 * mirroring the firmware's ResponseWrapper. The exception is a successful call
 * that returns data, such as machine:readmem: it answers with the bytes
 * themselves, as the firmware's binary_response() does.
 */
typedef struct restapi_response_s {
    char *fields;           /* accumulated "key":value pairs, comma separated */
    size_t fields_size;
    char *errors;           /* accumulated JSON strings, comma separated */
    size_t errors_size;
    int status;
    unsigned char *binary;  /* body of a binary response, or NULL */
    size_t binary_length;
} restapi_response_t;

/* request handling */
void restapi_request_free(restapi_request_t *req);
int restapi_parse_headers(restapi_request_t *req, const char *header_block);
void restapi_parse_body(restapi_request_t *req);
const char *restapi_param(const restapi_request_t *req, const char *name);
const char *restapi_param_or(const restapi_request_t *req, const char *name,
                             const char *fallback);
int restapi_param_int(const restapi_request_t *req, const char *name, int fallback);
const char *restapi_path_part(const restapi_request_t *req, int index);

/** \brief  Take ownership of uploaded temp file \a index
 *
 * For handlers that keep using the file after the request is answered, such as
 * mounting a disk image. The caller must lib_free() the name and remove the
 * file when done with it.
 *
 * \return  file name, or NULL when there is no such upload
 */
char *restapi_take_upload(restapi_request_t *req, int index);
void restapi_url_decode(char *src, char *dest);

/* response building */
void restapi_response_init(restapi_response_t *resp);
void restapi_response_free(restapi_response_t *resp);
void restapi_add_string(restapi_response_t *resp, const char *key, const char *value);
void restapi_add_int(restapi_response_t *resp, const char *key, int value);
void restapi_add_bool(restapi_response_t *resp, const char *key, int value);
void restapi_add_raw(restapi_response_t *resp, const char *key, const char *json);

/** \brief  Render \a text as a JSON string, quotes included and escaped
 *
 * For the few places that build nested JSON by hand.
 *
 * \return  heap buffer, caller frees with lib_free()
 */
char *restapi_json_quote(const char *text);
void restapi_error(restapi_response_t *resp, const char *fmt, ...) VICE_ATTR_PRINTF2;

/** \brief  Answer with \a length bytes of \a data instead of JSON
 *
 * Takes ownership of \a data, which must come from lib_malloc(). Only a
 * response that ends up with status 200 is sent this way; an error turns it
 * back into the usual JSON object.
 */
void restapi_set_binary(restapi_response_t *resp, unsigned char *data, size_t length);

/** \brief  Serialize \a resp into a complete HTTP response message
 *
 * \return  heap buffer, caller frees with lib_free(); \a length receives its size
 */
char *restapi_response_render(restapi_response_t *resp, size_t *length);

#endif
