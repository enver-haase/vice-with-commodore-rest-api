/** \file   restapi_http.c
 *  \brief  Minimal HTTP request parsing and JSON response building for the
 *          Ultimate64/1541-Ultimate compatible REST API
 *
 *  \author Enver Haase <enver.haase@infraleap.com>
 *
 *  The URL grammar and the shape of the JSON envelope deliberately match the
 *  1541-Ultimate / Ultimate64 firmware, so that clients written against a real
 *  device work against VICE unmodified. References:
 *
 *  - URL splitting: GideonZ/MicroHttpServer, c-version/lib/url.c,
 *    parse_url_static()
 *  - response envelope: 1541ultimate, software/api/routes.h, ResponseWrapper
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

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "archdep.h"
#include "lib.h"
#include "log.h"
#include "restapi_http.h"
#include "util.h"

/* ------------------------------------------------------------------------- */
/* helpers                                                                   */

/** \brief  Decode %XX escapes and '+' in \a src into \a dest
 *
 * \a dest may be equal to \a src; the result is never longer than the input.
 * A truncated escape at the end of the string is copied literally.
 */
void restapi_url_decode(char *src, char *dest)
{
    char *p = src;
    char code[3] = { 0, 0, 0 };

    while (*p != '\0') {
        if (*p == '%') {
            if (p[1] != '\0' && p[2] != '\0'
                && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
                code[0] = p[1];
                code[1] = p[2];
                *dest++ = (char)strtoul(code, NULL, 16);
                p += 2;
            } else {
                *dest++ = *p;
            }
        } else if (*p == '+') {
            *dest++ = ' ';
        } else {
            *dest++ = *p;
        }
        p++;
    }
    *dest = '\0';
}

/** \brief  Split \a str at the first occurrence of \a delim
 *
 * The delimiter is replaced by a terminator and a pointer to the remainder is
 * returned, or the empty string when the delimiter does not occur. This is the
 * behaviour the firmware's SPLIT() macro gets out of strsep(), spelled out so
 * we do not depend on strsep() being available everywhere.
 */
static char *split_at(char *str, char delim)
{
    static char empty[] = "";
    char *p = strchr(str, delim);

    if (p == NULL) {
        return empty;
    }
    *p = '\0';
    return p + 1;
}

/** \brief  Case insensitive strstr(), which is not portably available */
static const char *find_nocase(const char *haystack, const char *needle)
{
    size_t needle_len = strlen(needle);

    if (needle_len == 0) {
        return haystack;
    }
    while (*haystack != '\0') {
        if (util_strncasecmp(haystack, needle, needle_len) == 0) {
            return haystack;
        }
        haystack++;
    }
    return NULL;
}

static char *trim(char *str)
{
    char *end;

    while (*str != '\0' && isspace((unsigned char)*str)) {
        str++;
    }
    end = str + strlen(str);
    while (end > str && isspace((unsigned char)end[-1])) {
        *--end = '\0';
    }
    return str;
}

/* ------------------------------------------------------------------------- */
/* request parsing                                                           */

static void parse_querystring(restapi_request_t *req, char *querystring)
{
    while (*querystring != '\0' && req->param_count < RESTAPI_MAX_PARAMS) {
        char *pair = querystring;
        char *value;

        querystring = split_at(querystring, '&');
        if (*pair == '\0') {
            continue;
        }
        value = split_at(pair, '=');
        restapi_url_decode(pair, pair);
        restapi_url_decode(value, value);
        req->params[req->param_count].name = pair;
        req->params[req->param_count].value = value;
        req->param_count++;
    }
}

/** \brief  Split the request target into route, path, command and parameters
 *
 * \return  0 on success, -1 when this is not a /v1/... URL
 *
 * \note    Pointers in \a req point into req->url, which is owned by \a req.
 */
static int parse_url(restapi_request_t *req, const char *url)
{
    char *token;
    char *apiversion;
    char *querystring;

    if (*url == '/') {
        url++;
    }
    req->url = lib_strdup(url);
    token = req->url;

    apiversion = token;
    token = split_at(token, '/');
    if (*token == '\0') {
        /* nothing follows the version, so there is no route */
        return -1;
    }
    if (strcmp(apiversion, "v1") != 0) {
        return -1;
    }

    /* the querystring is separated first, so a ':' inside it is not a command */
    querystring = split_at(token, '?');
    req->command = split_at(token, ':');
    req->path = split_at(token, '/');
    req->route = token;

    restapi_url_decode(req->route, req->route);
    restapi_url_decode(req->path, req->path);
    restapi_url_decode(req->command, req->command);

    if (*req->command == '\0') {
        req->command = (char *)"none";
    }

    /* split the path into its elements for routes addressing a drive or unit */
    if (*req->path != '\0') {
        char *part = req->path;

        while (part != NULL && req->path_count < RESTAPI_MAX_PATH_PARTS) {
            char *next = strchr(part, '/');

            if (next != NULL) {
                *next = '\0';
            }
            req->path_parts[req->path_count++] = part;
            part = (next != NULL) ? next + 1 : NULL;
        }
    }

    parse_querystring(req, querystring);
    return 0;
}

/** \brief  Parse the request line and headers in \a header_block
 *
 * \param   header_block    NUL terminated headers, without the trailing blank
 *                          line, lines separated by CRLF or LF
 *
 * \return  0 on success, -1 on a malformed or unsupported request
 */
int restapi_parse_headers(restapi_request_t *req, const char *header_block)
{
    char *copy = lib_strdup(header_block);
    char *line = copy;
    char *rest;
    char *method;
    char *url;
    int result = -1;

    req->content_length = -1;

    /* request line */
    rest = split_at(line, '\n');
    line = trim(line);
    method = line;
    url = split_at(line, ' ');
    (void)split_at(url, ' ');   /* drop the HTTP version */
    url = trim(url);

    if (strcmp(method, "GET") == 0) {
        req->method = RESTAPI_METHOD_GET;
    } else if (strcmp(method, "PUT") == 0) {
        req->method = RESTAPI_METHOD_PUT;
    } else if (strcmp(method, "POST") == 0) {
        req->method = RESTAPI_METHOD_POST;
    } else {
        req->method = RESTAPI_METHOD_UNKNOWN;
    }

    if (*url == '\0') {
        lib_free(copy);
        return -1;
    }

    /* header fields */
    while (*rest != '\0') {
        char *key;
        char *value;

        line = rest;
        rest = split_at(line, '\n');
        value = split_at(line, ':');
        key = trim(line);
        value = trim(value);

        if (util_strcasecmp(key, "Content-Length") == 0) {
            req->content_length = strtol(value, NULL, 10);
        } else if (util_strcasecmp(key, "Content-Type") == 0) {
            req->content_type = lib_strdup(value);
        } else if (util_strcasecmp(key, "X-Password") == 0) {
            req->password = lib_strdup(value);
        } else if (util_strcasecmp(key, "Transfer-Encoding") == 0) {
            if (util_strcasecmp(value, "chunked") == 0) {
                req->chunked = 1;
            }
        }
    }

    result = parse_url(req, url);
    lib_free(copy);
    return result;
}

const char *restapi_param(const restapi_request_t *req, const char *name)
{
    int i;

    for (i = 0; i < req->param_count; i++) {
        if (util_strcasecmp(req->params[i].name, name) == 0) {
            return req->params[i].value;
        }
    }
    return NULL;
}

const char *restapi_param_or(const restapi_request_t *req, const char *name,
                             const char *fallback)
{
    const char *value = restapi_param(req, name);

    if (value == NULL || *value == '\0') {
        return fallback;
    }
    return value;
}

int restapi_param_int(const restapi_request_t *req, const char *name, int fallback)
{
    const char *value = restapi_param(req, name);
    char *end;
    long result;

    if (value == NULL || *value == '\0') {
        return fallback;
    }
    result = strtol(value, &end, 0);
    if (end == value) {
        return fallback;
    }
    return (int)result;
}

const char *restapi_path_part(const restapi_request_t *req, int index)
{
    if (index < 0 || index >= req->path_count) {
        return NULL;
    }
    return req->path_parts[index];
}

char *restapi_take_upload(restapi_request_t *req, int index)
{
    char *filename;

    if (index < 0 || index >= req->upload_count) {
        return NULL;
    }
    filename = req->uploads[index];
    req->uploads[index] = NULL;
    return filename;
}

void restapi_request_free(restapi_request_t *req)
{
    int i;

    for (i = 0; i < req->upload_count; i++) {
        if (req->uploads[i] != NULL) {
            archdep_remove(req->uploads[i]);
            lib_free(req->uploads[i]);
        }
    }
    lib_free(req->url);
    lib_free(req->content_type);
    lib_free(req->password);
    lib_free(req->body);
    memset(req, 0, sizeof(restapi_request_t));
}

/* ------------------------------------------------------------------------- */
/* multipart/form-data                                                       */

/** \brief  Find \a needle of \a needle_len bytes in a non NUL terminated buffer */
static const char *memfind(const char *haystack, size_t haystack_len,
                           const char *needle, size_t needle_len)
{
    if (needle_len == 0 || haystack_len < needle_len) {
        return NULL;
    }
    while (haystack_len >= needle_len) {
        if (memcmp(haystack, needle, needle_len) == 0) {
            return haystack;
        }
        haystack++;
        haystack_len--;
    }
    return NULL;
}

/** \brief  Write \a length bytes at \a data to a fresh temporary file
 *
 * \return  the file name, to be freed by the caller, or NULL on failure
 */
static char *write_tempfile(const char *data, size_t length)
{
    char *filename = NULL;
    FILE *fd = archdep_mkstemp_fd(&filename, MODE_WRITE);

    if (fd == NULL) {
        return NULL;
    }
    if (length > 0 && fwrite(data, 1, length, fd) != length) {
        fclose(fd);
        archdep_remove(filename);
        lib_free(filename);
        return NULL;
    }
    fclose(fd);
    return filename;
}

/** \brief  Extract the file parts of a multipart/form-data body into temp files
 *
 * Mirrors what the firmware's attachment_writer does: the payload of every file
 * part is spooled to a temporary file, and the route handler then acts on that
 * file exactly as it would on a file already present on the device.
 */
static void parse_multipart(restapi_request_t *req, const char *boundary)
{
    char *delim = lib_msprintf("--%s", boundary);
    size_t delim_len = strlen(delim);
    const char *cursor = req->body;
    size_t remaining = req->body_length;

    while (req->upload_count < RESTAPI_MAX_UPLOADS) {
        const char *part;
        const char *headers_end;
        const char *payload;
        const char *next;
        size_t payload_len;
        int is_file;

        part = memfind(cursor, remaining, delim, delim_len);
        if (part == NULL) {
            break;
        }
        part += delim_len;
        remaining -= (size_t)(part - cursor);
        cursor = part;

        /* the final delimiter is followed by "--" */
        if (remaining >= 2 && cursor[0] == '-' && cursor[1] == '-') {
            break;
        }

        headers_end = memfind(cursor, remaining, "\r\n\r\n", 4);
        if (headers_end == NULL) {
            break;
        }

        /* only parts that carry a file are of interest: a client is free to
           send ordinary form fields alongside, and spooling those would make
           the next handler act on a field value instead of the file */
        is_file = (memfind(cursor, (size_t)(headers_end - cursor),
                           "filename=", 9) != NULL);

        payload = headers_end + 4;
        remaining -= (size_t)(payload - cursor);
        cursor = payload;

        next = memfind(cursor, remaining, delim, delim_len);
        if (next == NULL) {
            /* no closing delimiter: take the rest, minus the trailing CRLF */
            payload_len = remaining;
        } else {
            payload_len = (size_t)(next - payload);
        }
        /* the CRLF in front of the delimiter belongs to the delimiter */
        if (payload_len >= 2 && payload[payload_len - 2] == '\r'
            && payload[payload_len - 1] == '\n') {
            payload_len -= 2;
        }

        if (payload_len > 0 && is_file) {
            char *filename = write_tempfile(payload, payload_len);

            if (filename != NULL) {
                req->uploads[req->upload_count++] = filename;
            } else {
                log_error(LOG_DEFAULT,
                          "restapi: could not spool uploaded data to a temporary file");
                break;
            }
        }

        if (next == NULL) {
            break;
        }
        remaining -= (size_t)(next - cursor);
        cursor = next;
    }

    lib_free(delim);
}

/** \brief  Turn the request body into temporary files, if it is multipart
 *
 * A body of any other content type is spooled as a single upload, which lets
 * clients that just PUT/POST raw file contents work too.
 */
void restapi_parse_body(restapi_request_t *req)
{
    const char *marker;

    if (req->body == NULL || req->body_length == 0) {
        return;
    }

    if (req->content_type != NULL
        && (marker = find_nocase(req->content_type, "boundary=")) != NULL) {
        char *boundary = lib_strdup(marker + strlen("boundary="));
        char *end = boundary;

        /* the boundary ends at a ';' or at the end of the header value, and may
           be quoted */
        if (*end == '"') {
            char *closing;

            memmove(boundary, boundary + 1, strlen(boundary));
            closing = strchr(boundary, '"');
            if (closing != NULL) {
                *closing = '\0';
            }
        } else {
            while (*end != '\0' && *end != ';' && !isspace((unsigned char)*end)) {
                end++;
            }
            *end = '\0';
        }

        if (*boundary != '\0') {
            parse_multipart(req, boundary);
        }
        lib_free(boundary);
        return;
    }

    /* not multipart: treat the whole body as one uploaded file */
    if (req->upload_count < RESTAPI_MAX_UPLOADS) {
        char *filename = write_tempfile(req->body, req->body_length);

        if (filename != NULL) {
            req->uploads[req->upload_count++] = filename;
        }
    }
}

/* ------------------------------------------------------------------------- */
/* response building                                                         */

void restapi_response_init(restapi_response_t *resp)
{
    memset(resp, 0, sizeof(restapi_response_t));
    resp->status = RESTAPI_HTTP_OK;
}

void restapi_response_free(restapi_response_t *resp)
{
    lib_free(resp->fields);
    lib_free(resp->errors);
    lib_free(resp->binary);
    memset(resp, 0, sizeof(restapi_response_t));
}

void restapi_set_binary(restapi_response_t *resp, unsigned char *data, size_t length)
{
    lib_free(resp->binary);
    resp->binary = data;
    resp->binary_length = length;
}

/** \brief  Append \a text to the comma separated list in \a list */
static void append_item(char **list, size_t *size, const char *text)
{
    size_t text_len = strlen(text);
    size_t old_len = (*list != NULL) ? *size : 0;
    size_t new_len = old_len + text_len + ((old_len > 0) ? 1 : 0);

    *list = lib_realloc(*list, new_len + 1);
    if (old_len > 0) {
        (*list)[old_len] = ',';
        memcpy(*list + old_len + 1, text, text_len);
    } else {
        memcpy(*list, text, text_len);
    }
    (*list)[new_len] = '\0';
    *size = new_len;
}

/** \brief  Escape \a text for use as a JSON string, without the quotes */
static char *json_escape(const char *text)
{
    size_t length = strlen(text);
    char *result = lib_malloc(length * 6 + 1);
    char *dest = result;

    while (*text != '\0') {
        unsigned char ch = (unsigned char)*text++;

        switch (ch) {
            case '"':   *dest++ = '\\'; *dest++ = '"';  break;
            case '\\':  *dest++ = '\\'; *dest++ = '\\'; break;
            case '\b':  *dest++ = '\\'; *dest++ = 'b';  break;
            case '\f':  *dest++ = '\\'; *dest++ = 'f';  break;
            case '\n':  *dest++ = '\\'; *dest++ = 'n';  break;
            case '\r':  *dest++ = '\\'; *dest++ = 'r';  break;
            case '\t':  *dest++ = '\\'; *dest++ = 't';  break;
            default:
                if (ch < 0x20) {
                    sprintf(dest, "\\u%04x", ch);
                    dest += 6;
                } else {
                    *dest++ = (char)ch;
                }
                break;
        }
    }
    *dest = '\0';
    return result;
}

char *restapi_json_quote(const char *text)
{
    char *escaped = json_escape((text != NULL) ? text : "");
    char *result = lib_msprintf("\"%s\"", escaped);

    lib_free(escaped);
    return result;
}

void restapi_add_string(restapi_response_t *resp, const char *key, const char *value)
{
    char *escaped = json_escape((value != NULL) ? value : "");
    char *item = lib_msprintf("\"%s\":\"%s\"", key, escaped);

    append_item(&resp->fields, &resp->fields_size, item);
    lib_free(item);
    lib_free(escaped);
}

void restapi_add_int(restapi_response_t *resp, const char *key, int value)
{
    char *item = lib_msprintf("\"%s\":%d", key, value);

    append_item(&resp->fields, &resp->fields_size, item);
    lib_free(item);
}

void restapi_add_bool(restapi_response_t *resp, const char *key, int value)
{
    char *item = lib_msprintf("\"%s\":%s", key, value ? "true" : "false");

    append_item(&resp->fields, &resp->fields_size, item);
    lib_free(item);
}

void restapi_add_raw(restapi_response_t *resp, const char *key, const char *json)
{
    char *item = lib_msprintf("\"%s\":%s", key, json);

    append_item(&resp->fields, &resp->fields_size, item);
    lib_free(item);
}

void restapi_error(restapi_response_t *resp, const char *fmt, ...)
{
    va_list ap;
    char *text;
    char *escaped;
    char *item;

    va_start(ap, fmt);
    text = lib_mvsprintf(fmt, ap);
    va_end(ap);

    escaped = json_escape(text);
    item = lib_msprintf("\"%s\"", escaped);
    append_item(&resp->errors, &resp->errors_size, item);

    lib_free(item);
    lib_free(escaped);
    lib_free(text);
}

static const char *status_text(int status)
{
    switch (status) {
        case RESTAPI_HTTP_OK:                   return "OK";
        case RESTAPI_HTTP_BAD_REQUEST:          return "Bad Request";
        case RESTAPI_HTTP_FORBIDDEN:            return "Forbidden";
        case RESTAPI_HTTP_NOT_FOUND:            return "Not Found";
        case RESTAPI_HTTP_PRECONDITION_FAILED:  return "Precondition Failed";
        case RESTAPI_HTTP_PAYLOAD_TOO_LARGE:    return "Payload Too Large";
        case RESTAPI_HTTP_UNSUPPORTED_MEDIA_TYPE: return "Unsupported Media Type";
        case RESTAPI_HTTP_NOT_IMPLEMENTED:      return "Not Implemented";
        default:                                return "Internal Server Error";
    }
}

char *restapi_response_render(restapi_response_t *resp, size_t *length)
{
    char *json;
    char *message;

    if (resp->binary != NULL && resp->status == RESTAPI_HTTP_OK) {
        char *header = lib_msprintf("HTTP/1.1 200 OK\r\n"
                                    "Content-Type: application/octet-stream\r\n"
                                    "Content-Disposition: attachment\r\n"
                                    "Content-Length: %lu\r\n"
                                    "Connection: close\r\n"
                                    "\r\n",
                                    (unsigned long)resp->binary_length);
        size_t header_length = strlen(header);

        /* the body may hold NUL bytes, so it is copied, not formatted */
        message = lib_malloc(header_length + resp->binary_length + 1);
        memcpy(message, header, header_length);
        memcpy(message + header_length, resp->binary, resp->binary_length);
        message[header_length + resp->binary_length] = '\0';
        lib_free(header);

        if (length != NULL) {
            *length = header_length + resp->binary_length;
        }
        return message;
    }

    json = lib_msprintf("{%s%s\"errors\":[%s]}",
                        (resp->fields != NULL) ? resp->fields : "",
                        (resp->fields != NULL) ? "," : "",
                        (resp->errors != NULL) ? resp->errors : "");

    message = lib_msprintf("HTTP/1.1 %d %s\r\n"
                           "Content-Type: application/json\r\n"
                           "Content-Length: %lu\r\n"
                           "Connection: close\r\n"
                           "\r\n"
                           "%s",
                           resp->status, status_text(resp->status),
                           (unsigned long)strlen(json), json);
    lib_free(json);

    if (length != NULL) {
        *length = strlen(message);
    }
    return message;
}
