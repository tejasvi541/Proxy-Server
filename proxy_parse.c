/*───────────────────────────────────────────────────────────────────────────
 *  proxy_parse.c      –  *implementation* of the light HTTP parser
 *
 *  Design goals:
 *      • Tiny – <300 LOC.
 *      • No libc extensions – only ISO C17 + <ctype.h>.
 *      • Forgiving – skips malformed headers instead of aborting.
 *
 *  Windows quirks:
 *      MSVC names strncasecmp → _strnicmp; alias provided below.
 *───────────────────────────────────────────────────────────────────────────*/

#define _CRT_SECURE_NO_WARNINGS      /* allow sscanf/strcpy on MSVC   */
#include "proxy_parse.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>

 /*──────────────────── Cross-platform helpers ───────────────────────*/
#ifdef _WIN32
#   define strncasecmp _strnicmp
#   define strcasecmp  _stricmp
#   define x_strdup    _strdup
/* MSVC's strtok_s(buf, delims, &ctx); POSIX equivalent is strtok_r. */
#   define x_strtok    strtok_s
#else
#   include <strings.h>   /* strncasecmp / strcasecmp on POSIX */
#   define x_strdup    strdup
#   define x_strtok    strtok_r
#endif

/* malloc wrapper that *exits* on OOM so callers stay clean. */
static void* xcalloc(size_t count, size_t bytes_each)
{
    void* ptr = calloc(count, bytes_each);
    if (!ptr) { perror("calloc"); exit(EXIT_FAILURE); }
    return ptr;
}

/* Grow header array if needed. */
static int ensure_header_capacity(ParsedRequest* req, size_t want)
{
    if (want <= req->headers_capacity) return 0;
    size_t new_cap = req->headers_capacity ? req->headers_capacity * 2 : 4;
    while (new_cap < want) new_cap *= 2;

    ParsedHeader* tmp = realloc(req->headers, new_cap * sizeof * tmp);
    if (!tmp) return -1;

    req->headers = tmp;
    req->headers_capacity = new_cap;
    return 0;
}

/*──────────────────── Public API implementation ────────────────────*/
ParsedRequest* ParsedRequest_create(void)
{
    return (ParsedRequest*)xcalloc(1, sizeof(ParsedRequest));
}

void ParsedRequest_destroy(ParsedRequest* pr)
{
    if (!pr) return;

    for (size_t i = 0; i < pr->headers_in_use; ++i) {
        free(pr->headers[i].key);
        free(pr->headers[i].value);
    }
    free(pr->headers);

    free(pr->method);
    free(pr->protocol);
    free(pr->host);
    free(pr->port);
    free(pr->path);
    free(pr->version);
    free(pr->raw_request_line);

    free(pr);
}

int ParsedRequest_parse(ParsedRequest* pr, const char* buf, int buflen)
{
    if (!pr || !buf || buflen <= 0) return -1;

    /*─────────────────── 1) Slice out request-line ─────────────────*/
    const char* eol = strstr(buf, "\r\n");
    if (!eol) return -1;                         /* must end in CRLF */

    pr->raw_request_line_length = (size_t)(eol - buf);
    pr->raw_request_line = (char*)xcalloc(1, pr->raw_request_line_length + 1);
    memcpy(pr->raw_request_line, buf, pr->raw_request_line_length);

    /* Tokenise: METHOD  SP  URL  SP  VERSION */
    char* saveptr = NULL;
    char* token = x_strtok(pr->raw_request_line, " ", &saveptr);
    if (!token) return -1;
    pr->method = x_strdup(token);

    token = x_strtok(NULL, " ", &saveptr);
    if (!token) return -1;
    char* full_url = token;                      /* still holds scheme */

    token = x_strtok(NULL, "\r", &saveptr);
    if (!token) return -1;
    pr->version = x_strdup(token);

    /*─────────────────── 2) Decompose absolute URL ─────────────────
     *  Accepts http:// (default :80) and https:// (default :443).
     *  full_url is now "host[:port][/path]". The path is snapshotted
     *  FIRST so host/port copies below never swallow it. */
    const char *scheme;
    if (strncasecmp(full_url, "https://", 8) == 0) {
        scheme = "https";
        full_url += 8;
    } else if (strncasecmp(full_url, "http://", 7) == 0) {
        scheme = "http";
        full_url += 7;
    } else {
        return -1;
    }

    char* path_start = strchr(full_url, '/');
    char* path_copy = x_strdup(path_start ? path_start : "/");
    if (path_start) *path_start = '\0';          /* isolate host[:port] */

    char* colon = strchr(full_url, ':');
    if (colon) {
        *colon = '\0';
        pr->host = x_strdup(full_url);
        pr->port = x_strdup(colon + 1);
    }
    else {
        pr->host = x_strdup(full_url);
        pr->port = NULL;                         /* implies :80      */
    }
    pr->path = path_copy;
    pr->protocol = x_strdup(scheme);

    /*─────────────────── 3) Parse headers one by one ───────────────*/
    const char* cursor = eol + 2;                /* skip first CRLF  */
    while (cursor < buf + buflen &&
        !(cursor[0] == '\r' && cursor[1] == '\n')) {

        const char* line_end = strstr(cursor, "\r\n");
        if (!line_end) break;                    /* malformed, bail  */

        const char* colon = memchr(cursor, ':', line_end - cursor);
        if (!colon) { cursor = line_end + 2; continue; } /* skip junk */

        size_t key_len = (size_t)(colon - cursor);
        size_t val_len = (size_t)(line_end - colon - 1);

        ensure_header_capacity(pr, pr->headers_in_use + 1);
        ParsedHeader* hdr = &pr->headers[pr->headers_in_use++];

        hdr->key = (char*)xcalloc(1, key_len + 1);
        hdr->value = (char*)xcalloc(1, val_len + 1);
        memcpy(hdr->key, cursor, key_len);
        memcpy(hdr->value, colon + 1, val_len);

        /* Trim leading whitespace in value */
        while (*hdr->value && isspace((unsigned char)*hdr->value))
            memmove(hdr->value, hdr->value + 1, --val_len + 1);

        hdr->key_length = strlen(hdr->key);
        hdr->value_length = strlen(hdr->value);

        cursor = line_end + 2;
    }

    return 0;
}

/* ───── Helpers that rebuild text from ParsedRequest ───────────────*/
int ParsedRequest_unparse_headers(ParsedRequest* pr, char* dst, size_t dst_len)
{
    size_t written = 0;

    for (size_t i = 0; i < pr->headers_in_use; ++i) {
        ParsedHeader* h = &pr->headers[i];
        int n = snprintf(dst + written, dst_len - written,
            "%s:%s\r\n", h->key, h->value);
        if (n < 0 || (size_t)n >= dst_len - written) return -1;
        written += (size_t)n;
    }
    if (written + 2 > dst_len) return -1;
    dst[written++] = '\r'; dst[written++] = '\n';
    return (int)written;
}

int ParsedRequest_unparse(ParsedRequest* pr, char* dst, size_t dst_len)
{
    int n = snprintf(dst, dst_len, "%s %s %s\r\n",
        pr->method, pr->path, pr->version);
    if (n < 0 || (size_t)n >= dst_len) return -1;
    return n + ParsedRequest_unparse_headers(pr, dst + n, dst_len - n);
}

/* ───── Tiny length helpers (avoid recomputation) ──────────────────*/
size_t ParsedHeader_headersLen(ParsedRequest* pr)
{
    size_t sum = 2;                           /* final empty line  */
    for (size_t i = 0; i < pr->headers_in_use; ++i)
        sum += pr->headers[i].key_length + 1      /* ':' */
        + pr->headers[i].value_length + 2;   /* CRLF */
    return sum;
}
size_t ParsedRequest_totalLen(ParsedRequest* pr)
{
    return strlen(pr->method) + 1 /* SP */
        + strlen(pr->path) + 1 /* SP */
        + strlen(pr->version) + 2 /* CRLF */
        + ParsedHeader_headersLen(pr);
}

/* ───── Header CRUD helpers (case-insensitive keys) ───────────────*/
ParsedHeader* ParsedHeader_get(ParsedRequest* pr, const char* key)
{
    for (size_t i = 0; i < pr->headers_in_use; ++i)
        if (strcasecmp(pr->headers[i].key, key) == 0)
            return &pr->headers[i];
    return NULL;
}

int ParsedHeader_set(ParsedRequest* pr, const char* key, const char* val)
{
    ParsedHeader* hdr = ParsedHeader_get(pr, key);
    if (!hdr) {                                             /* new   */
        ensure_header_capacity(pr, pr->headers_in_use + 1);
        hdr = &pr->headers[pr->headers_in_use++];
        hdr->key = x_strdup(key);
    }
    else {
        free(hdr->value);
    }
    hdr->value = x_strdup(val);
    hdr->key_length = strlen(hdr->key);
    hdr->value_length = strlen(hdr->value);
    return 0;
}

int ParsedHeader_remove(ParsedRequest* pr, const char* key)
{
    for (size_t i = 0; i < pr->headers_in_use; ++i) {
        if (strcasecmp(pr->headers[i].key, key) == 0) {
            free(pr->headers[i].key);
            free(pr->headers[i].value);
            memmove(&pr->headers[i], &pr->headers[i + 1],
                (pr->headers_in_use - i - 1) * sizeof(ParsedHeader));
            --pr->headers_in_use;
            return 0;
        }
    }
    return -1;
}

/*──────────────────── Debug printf helper ─────────────────────────*/
void debug_proxy_parse(const char* fmt, ...)
{
    if (!DEBUG_PROXY_PARSE) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}


/* Worked example (parse → unparse → get/set/remove → destroy) lives in
 * PROJECT_EXPLAINED.md so it is written once instead of being pasted
 * at the bottom of both proxy_parse.c and proxy_parse.h. */
