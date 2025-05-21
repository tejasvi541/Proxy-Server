/*───────────────────────────────────────────────────────────────────────────
 *  proxy_parse.h
 *
 *  A *minimal but sufficient* HTTP request parser used by our proxy.
 *  – Pure ISO-C17  →  works on Windows / Linux / macOS unchanged
 *  – Only understands absolute-URI GET lines (enough for a teaching proxy)
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
    char* protocol;        /* always “http” for this proxy          */
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

