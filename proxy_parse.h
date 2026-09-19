/*───────────────────────────────────────────────────────────────────────────
 *  proxy_parse.h
 *
 *  A *minimal but sufficient* HTTP request parser used by our proxy.
 *  – Pure ISO-C17  →  works on Windows / Linux / macOS unchanged
 *  – Understands absolute-URI request lines, http:// and https://
 *
 *  Copyright notes:
 *      Skeleton was written for Princeton COS-518 (Matvey Arye).
 *      Re-commented and tidied by Tejasvi, May 2025.
 *───────────────────────────────────────────────────────────────────────────*/

#ifndef PROXY_PARSE_H
#define PROXY_PARSE_H

#include <stddef.h>     /* size_t */

 /* Toggle noisy stdout debugging by flipping this to 1. */
#define DEBUG_PROXY_PARSE  0

/*──────────────────────────────────────────────────────────────────────
 *  ParsedHeader – a single “Key: Value\r\n” pair.
 *  We store the *exact* strings (no canonicalisation) so we can replay
 *  or edit them later.
 *──────────────────────────────────────────────────────────────────────*/
typedef struct ParsedHeader {
    char* key;            size_t key_length;
    char* value;          size_t value_length;
} ParsedHeader;

/*──────────────────────────────────────────────────────────────────────
 *  ParsedRequest – one fully-split HTTP/1.x request.
 *
 *      Example:
 *          GET http://example.com:8080/index.html HTTP/1.1\r\n
 *          Host: example.com\r\n
 *          Connection: close\r\n
 *          \r\n
 *
 *  After parsing, the above becomes:
 *      method   = "GET"
 *      protocol = "http"
 *      host     = "example.com"
 *      port     = "8080"
 *      path     = "/index.html"
 *      version  = "HTTP/1.1"
 *      headers  = {{"Host"," example.com"}, {"Connection"," close"}}
 *
 *  raw_request_line keeps a *copy* of “GET http://… HTTP/1.1”.
 *──────────────────────────────────────────────────────────────────────*/
typedef struct ParsedRequest {

    /* ───────── Tokens from the request-line */
    char* method;          /* GET / POST / CONNECT …                */
    char* protocol;        /* "http" or "https" (from the URL scheme) */
    char* host;            /* hostname part of URL                  */
    char* port;            /* NULL → default 80                     */
    char* path;            /* resource path, starts with “/”        */
    char* version;         /* “HTTP/1.0” or “HTTP/1.1”              */

    /* ───────── Original request-line (for cheap substring copies)   */
    char* raw_request_line;
    size_t  raw_request_line_length;

    /* ───────── Dynamic header array                                 */
    ParsedHeader* headers;           /* malloc-grown list             */
    size_t        headers_in_use;    /* number of valid entries       */
    size_t        headers_capacity;  /* slots currently allocated     */

} ParsedRequest;

/*──────────────────────── Public API – implemented in proxy_parse.c ─────────*/

/* Memory lifecycle */
ParsedRequest* ParsedRequest_create(void);
void            ParsedRequest_destroy(ParsedRequest* pr);

/* From wire-format to struct – returns 0 on success, -1 otherwise. */
int             ParsedRequest_parse(ParsedRequest* pr,
    const char* buffer,
    int           buffer_len);

/* Struct  →  wire-format (complete request or headers-only)         */
int             ParsedRequest_unparse(ParsedRequest* pr,
    char* dst,
    size_t         dst_len);
int             ParsedRequest_unparse_headers(ParsedRequest* pr,
    char* dst,
    size_t         dst_len);

/* Convenience length helpers                                        */
size_t          ParsedRequest_totalLen(ParsedRequest* pr);
size_t          ParsedHeader_headersLen(ParsedRequest* pr);

/* Header CRUD                                                        */
int             ParsedHeader_set(ParsedRequest* pr,
    const char* key,
    const char* value);
ParsedHeader* ParsedHeader_get(ParsedRequest* pr,
    const char* key);
int             ParsedHeader_remove(ParsedRequest* pr,
    const char* key);

/* printf-style debug helper (only prints when DEBUG_PROXY_PARSE=1). */
void            debug_proxy_parse(const char* fmt, ...);

#endif /* PROXY_PARSE_H */

