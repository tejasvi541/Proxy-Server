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
    char* token = strtok_s(pr->raw_request_line, " ", &saveptr);
    if (!token) return -1;
    pr->method = _strdup(token);

    token = strtok_s(NULL, " ", &saveptr);
    if (!token) return -1;
    char* full_url = token;                      /* still holds scheme */

    token = strtok_s(NULL, "\r", &saveptr);
    if (!token) return -1;
    pr->version = _strdup(token);

    /*─────────────────── 2) Decompose absolute URL ─────────────────*/
    if (strncasecmp(full_url, "http://", 7) != 0) return -1;
    full_url += 7;                               /* skip 'http://'   */

    char* path_start = strchr(full_url, '/');   /* host[:port]/path */
    char* colon_in_hp = strchr(full_url, ':');

    if (colon_in_hp && (!path_start || colon_in_hp < path_start)) {
        *colon_in_hp = '\0';
        pr->host = _strdup(full_url);
        pr->port = _strdup(colon_in_hp + 1);
    }
    else {
        pr->host = _strdup(full_url);
        pr->port = NULL;                         /* implies :80      */
    }
    pr->path = _strdup(path_start ? path_start : "/");
    pr->protocol = _strdup("http");

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
        hdr->key = _strdup(key);
    }
    else {
        free(hdr->value);
    }
    hdr->value = _strdup(val);
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
#include <stdarg.h>
void debug_proxy_parse(const char* fmt, ...)
{
    if (!DEBUG_PROXY_PARSE) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}


/* Example usage:

   const char *c =
   "GET http://www.google.com:80/index.html/ HTTP/1.0\r\nContent-Length:"
   " 80\r\nIf-Modified-Since: Sat, 29 Oct 1994 19:43:31 GMT\r\n\r\n";

   int len = strlen(c);
   //Create a ParsedRequest to use. This ParsedRequest
   //is dynamically allocated.
   ParsedRequest *req = ParsedRequest_create();
   if (ParsedRequest_parse(req, c, len) < 0) {
       printf("parse failed\n");
       return -1;
   }

   printf("Method:%s\n", req->method);
   printf("Host:%s\n", req->host);

   // Turn ParsedRequest into a string.
   // Friendly reminder: Be sure that you need to
   // dynamically allocate string and if you
   // do, remember to free it when you are done.
   // (Dynamic allocation wasn't necessary here,
   // but it was used as an example.)
   int rlen = ParsedRequest_totalLen(req);
   char *b = (char *)malloc(rlen+1);
   if (ParsedRequest_unparse(req, b, rlen) < 0) {
      printf("unparse failed\n");
      return -1;
   }
   b[rlen]='\0';
   // print out b for text request
   free(b);


   // Turn the headers from the request into a string.
   rlen = ParsedHeader_headersLen(req);
   char buf[rlen+1];
   if (ParsedRequest_unparse_headers(req, buf, rlen) < 0) {
      printf("unparse failed\n");
      return -1;
   }
   buf[rlen] ='\0';
   //print out buf for text headers only

   // Get a specific header (key) from the headers. A key is a header field
   // such as "If-Modified-Since" which is followed by ":"
   struct ParsedHeader *r = ParsedHeader_get(req, "If-Modified-Since");
   printf("Modified value: %s\n", r->value);

   // Remove a specific header by name. In this case remove
   // the "If-Modified-Since" header.
   if (ParsedHeader_remove(req, "If-Modified-Since") < 0){
      printf("remove header key not work\n");
     return -1;
   }

   // Set a specific header (key) to a value. In this case,
   //we set the "Last-Modified" key to be set to have as
   //value  a date in February 2014

    if (ParsedHeader_set(req, "Last-Modified", " Wed, 12 Feb 2014 12:43:31 GMT") < 0){
     printf("set header key not work\n");
     return -1;

    }

   // Check the modified Header key value pair
    r = ParsedHeader_get(req, "Last-Modified");
    printf("Last-Modified value: %s\n", r->value);

   // Call destroy on any ParsedRequests that you
   // create once you are done using them. This will
   // free memory dynamically allocated by the proxy_parse library.
   ParsedRequest_destroy(req);
*/
