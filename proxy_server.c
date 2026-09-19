/*───────────────────────────────────────────────────────────────────
 *  proxy_server.c — multithreaded HTTP proxy with LRU cache.
 *
 *  What it does:
 *    • Listens on a TCP port (default 8080, override: ./proxy 8888).
 *    • One detached pthread per client; at most MAX_CLIENTS run at
 *      once (extra connections wait on a condition variable).
 *    • GET http://...  → serve from cache on hit, else fetch from the
 *      origin server, relay to the client, and store 200 OK replies.
 *    • GET https://... → same, over TLS (needs -DUSE_TLS + OpenSSL):
 *      verified cert (system CAs + hostname check), SNI sent.
 *    • CONNECT host:port → blind TCP tunnel (this is how HTTPS passes
 *      through an HTTP proxy; bytes are relayed, never cached).
 *    • Anything else → 501 Not Implemented.
 *
 *  Portability: POSIX (Linux/macOS, verified) + a Winsock shim for
 *  Windows. Windows threads still need pthreads-w32 — or just use WSL.
 *───────────────────────────────────────────────────────────────────*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>

#include "proxy_parse.h"

#ifdef _WIN32
#   include <winsock2.h>
#   include <ws2tcpip.h>
#   include <windows.h>
#   pragma comment(lib, "ws2_32.lib")
    typedef SOCKET sock_t;
#   define SOCK_INVALID INVALID_SOCKET
#   define sock_close   closesocket
#   define sock_shutdown_wr(fd) shutdown((fd), SD_SEND)
#   define x_strdup     _strdup
#   include <pthread.h>   /* pthreads-w32 on native Windows */
#else
#   include <unistd.h>
#   include <signal.h>
#   include <fcntl.h>
#   include <pthread.h>
#   include <semaphore.h>   /* kept for reference; see note below */
#   include <sys/types.h>
#   include <sys/socket.h>
#   include <sys/select.h>
#   include <netdb.h>
#   include <arpa/inet.h>
    typedef int sock_t;
#   define SOCK_INVALID -1
#   define sock_close   close
#   define sock_shutdown_wr(fd) shutdown((fd), SHUT_WR)
#   define x_strdup     strdup
#endif

/* NOTE on limiting concurrency: the classic textbook design uses an
 * unnamed POSIX semaphore (sem_init/sem_wait). That does NOT work on
 * macOS — Darwin never implemented unnamed semaphores — so this proxy
 * uses the portable equivalent: a mutex + counter + condition
 * variable. Same effect (block while MAX_CLIENTS are active), works
 * everywhere pthreads does. */

/*──────────────────────────── Config ────────────────────────────*/
#define DEFAULT_PORT      8080
#define MAX_CLIENTS       100
#define LISTEN_BACKLOG    128
#define BUFFER_SIZE       8192
#define HEADER_LIMIT      (64 * 1024)          /* max request head */
#define MAX_CACHE_SIZE    (4 * 1024 * 1024)    /* 4 MB total       */
#define MAX_ELEMENT_SIZE  (512 * 1024)         /* 512 KB per entry */
#define CONNECT_TIMEOUT_S 10
#define IO_TIMEOUT_S      30

/*─────────────────────── Connection limiter ─────────────────────*/
static pthread_mutex_t active_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  active_cond = PTHREAD_COND_INITIALIZER;
static int             active_clients = 0;

/*──────────────────────────── Cache ─────────────────────────────
 * Singly-linked list of responses. head/total_size/lock are the only
 * shared state in the program; every access runs under cache_lock. */
typedef struct cache_element {
    char                 *data;  /* raw response bytes            */
    int                   len;   /* byte count                    */
    char                 *url;   /* cache key: http://h:port/path */
    time_t                lru;   /* last-use timestamp            */
    struct cache_element *next;
} cache_element;

static cache_element  *cache_head  = NULL;
static size_t          cache_total = 0;
static pthread_mutex_t cache_lock  = PTHREAD_MUTEX_INITIALIZER;

/* Build the canonical key "scheme://host:port/path" — scheme and an
 * explicit port included, so http://h/p, http://h:80/p and
 * https://h:443/p are three different entries. Returns 0, or -1 if
 * dst is too small. */
static int cache_key(const ParsedRequest *pr, char *dst, size_t dst_len)
{
    int is_https = pr->protocol && strcmp(pr->protocol, "https") == 0;
    const char *defport = is_https ? "443" : "80";
    int n = snprintf(dst, dst_len, "%s://%s:%s%s",
                     is_https ? "https" : "http",
                     pr->host, pr->port ? pr->port : defport, pr->path);
    return (n < 0 || (size_t)n >= dst_len) ? -1 : 0;
}

/* Hit → malloc'd copy of the body in *data_out (caller frees), 1.
 * Miss → 0. The copy under lock means the entry can be evicted right
 * after we unlock without the sender holding a dangling pointer. */
static int cache_lookup(const char *url, char **data_out, int *len_out)
{
    int found = 0;
    pthread_mutex_lock(&cache_lock);
    for (cache_element *e = cache_head; e; e = e->next) {
        if (strcmp(e->url, url) == 0) {
            char *copy = malloc((size_t)e->len);
            if (copy) {
                memcpy(copy, e->data, (size_t)e->len);
                *data_out = copy;
                *len_out  = e->len;
                e->lru    = time(NULL);   /* mark recently used */
                found     = 1;
            }
            break;
        }
    }
    pthread_mutex_unlock(&cache_lock);
    return found;
}

/* Drop the least-recently-used entry. Caller must hold cache_lock. */
static void cache_evict_lru_locked(void)
{
    cache_element *victim = NULL, *victim_prev = NULL;
    cache_element *prev = NULL;
    for (cache_element *e = cache_head; e; prev = e, e = e->next) {
        if (!victim || e->lru < victim->lru) {
            victim = e;
            victim_prev = prev;
        }
    }
    if (!victim) return;
    if (victim_prev) victim_prev->next = victim->next;
    else             cache_head = victim->next;
    cache_total -= (size_t)victim->len;
    free(victim->data);
    free(victim->url);
    free(victim);
}

/* Store a response. Oversize bodies are refused; room is made by
 * evicting LRU entries. Re-storing an existing URL replaces it. */
static void cache_store(const char *url, const char *data, int len)
{
    if (len <= 0 || (size_t)len > MAX_ELEMENT_SIZE) return;
    pthread_mutex_lock(&cache_lock);
    for (cache_element *e = cache_head; e; e = e->next) {
        if (strcmp(e->url, url) == 0) {   /* refresh existing entry */
            char *copy = malloc((size_t)len);
            if (copy) {
                memcpy(copy, data, (size_t)len);
                cache_total += (size_t)(len - e->len);
                free(e->data);
                e->data = copy;
                e->len  = len;
                e->lru  = time(NULL);
            }
            pthread_mutex_unlock(&cache_lock);
            return;
        }
    }
    while (cache_head && cache_total + (size_t)len > MAX_CACHE_SIZE)
        cache_evict_lru_locked();
    cache_element *e = malloc(sizeof *e);
    if (e) {
        e->data = malloc((size_t)len);
        e->url  = x_strdup(url);
        if (!e->data || !e->url) {
            free(e->data);
            free(e->url);
            free(e);
        } else {
            memcpy(e->data, data, (size_t)len);
            e->len  = len;
            e->lru  = time(NULL);
            e->next = cache_head;
            cache_head = e;
            cache_total += (size_t)len;
        }
    }
    pthread_mutex_unlock(&cache_lock);
}

/*────────────────────────── Socket utils ────────────────────────*/

/* Send exactly len bytes (loop: one send() rarely takes it all).
 * Returns 0 on success, -1 on error/closed peer. */
static int send_all(sock_t fd, const char *buf, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        int n = (int)send(fd, buf + sent, len - sent, 0);
        if (n <= 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

static void send_error(sock_t client, int code, const char *reason)
{
    char msg[256];
    int n = snprintf(msg, sizeof msg,
                     "HTTP/1.0 %d %s\r\n"
                     "Content-Type: text/plain\r\n"
                     "Connection: close\r\n"
                     "\r\n%d %s\n",
                     code, reason, code, reason);
    if (n > 0) send_all(client, msg, (size_t)n);
}

/* Read until the end of the HTTP head ("\r\n\r\n") or HEADER_LIMIT.
 * Returns a malloc'd buffer (caller frees) with *len_out bytes, or
 * NULL on error/overflow/closed connection. */
static char *recv_headers(sock_t fd, int *len_out)
{
    size_t cap = BUFFER_SIZE, len = 0;
    char *buf = malloc(cap + 1);
    if (!buf) return NULL;
    for (;;) {
        if (len == cap) {
            if (cap >= HEADER_LIMIT) { free(buf); return NULL; }
            cap *= 2;
            if (cap > HEADER_LIMIT) cap = HEADER_LIMIT;
            char *bigger = realloc(buf, cap + 1);
            if (!bigger) { free(buf); return NULL; }
            buf = bigger;
        }
        int n = (int)recv(fd, buf + len, cap - len, 0);
        if (n <= 0) { free(buf); return NULL; }
        len += (size_t)n;
        buf[len] = '\0';
        if (strstr(buf, "\r\n\r\n")) break;
    }
    *len_out = (int)len;
    return buf;
}

/* Offset just past "\r\n\r\n" — bytes after it were already read and
 * (for tunnels) must be forwarded, not dropped. */
static int headers_end(const char *buf)
{
    const char *p = strstr(buf, "\r\n\r\n");
    return p ? (int)(p - buf) + 4 : -1;
}

/* First line looks like "HTTP/1.1 200 OK"? Only 200s are cached. */
static int is_cacheable_status(const char *chunk, int len)
{
    if (len < 12 || strncmp(chunk, "HTTP/", 5) != 0) return 0;
    const char *eol = strstr(chunk, "\r\n");
    int linelen = eol ? (int)(eol - chunk) : len;
    for (int i = 5; i + 5 <= linelen; i++)
        if (chunk[i] == ' ' && chunk[i+1] == '2'
            && chunk[i+2] == '0' && chunk[i+3] == '0'
            && chunk[i+4] == ' ') return 1;
    return 0;
}

#ifndef _WIN32
static int set_nonblocking(sock_t fd, int on)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, on ? flags | O_NONBLOCK : flags & ~O_NONBLOCK);
}
#endif

static void set_timeouts(sock_t fd)
{
#ifdef _WIN32
    DWORD ms = (DWORD)(IO_TIMEOUT_S * 1000);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&ms, sizeof ms);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&ms, sizeof ms);
#else
    struct timeval tv = { .tv_sec = IO_TIMEOUT_S, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#endif
}

/* Resolve host:port and connect with a CONNECT_TIMEOUT_S deadline.
 * Non-blocking connect + select is the standard trick: a blocking
 * connect() to a dead host can stall for minutes. */
static sock_t connect_remote(const char *host, const char *port)
{
    struct addrinfo hints, *list = NULL, *ai;
    memset(&hints, 0, sizeof hints);
    hints.ai_family   = AF_UNSPEC;    /* IPv4 or IPv6 */
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &list) != 0) return SOCK_INVALID;

    sock_t winner = SOCK_INVALID;
    for (ai = list; ai; ai = ai->ai_next) {
        sock_t fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd == SOCK_INVALID) continue;
#ifdef _WIN32
        u_long mode = 1;
        ioctlsocket(fd, FIONBIO, &mode);
#else
        if (set_nonblocking(fd, 1) < 0) { sock_close(fd); continue; }
#endif
        int rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
#ifdef _WIN32
        int in_progress = (rc != 0 && WSAGetLastError() == WSAEWOULDBLOCK);
#else
        int in_progress = (rc != 0 && errno == EINPROGRESS);
#endif
        if (rc == 0 || in_progress) {
            fd_set wfds;
            FD_ZERO(&wfds);
            FD_SET(fd, &wfds);
            struct timeval tv = { .tv_sec = CONNECT_TIMEOUT_S, .tv_usec = 0 };
            int ready = select((int)fd + 1, NULL, &wfds, NULL, &tv);
            int err = 0;
            socklen_t errlen = sizeof err;
            if (ready > 0) {
                getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *)&err, &errlen);
#ifdef _WIN32
                if (err == 0) { u_long m = 0; ioctlsocket(fd, FIONBIO, &m); }
#else
                if (err == 0) set_nonblocking(fd, 0);
#endif
            } else {
                err = -1;   /* timeout or select error */
            }
            if (err == 0) { winner = fd; break; }
        }
        sock_close(fd);
    }
    freeaddrinfo(list);
    if (winner != SOCK_INVALID) set_timeouts(winner);
    return winner;
}

/*──────────────────── TLS (https:// origins) ────────────────────
 * Built only with -DUSE_TLS (needs OpenSSL; see Makefile).
 * A shared SSL_CTX is thread-safe for opening client connections.
 * Verification is ON: system CA store + hostname check, so a forged
 * cert fails the handshake and the client gets 502 — the proxy never
 * relays bytes it cannot authenticate. */
#ifdef USE_TLS
#   include <openssl/ssl.h>
#   include <openssl/err.h>

static SSL_CTX *tls_ctx = NULL;

static int tls_init(void)
{
    tls_ctx = SSL_CTX_new(TLS_client_method());
    if (!tls_ctx) return -1;
    SSL_CTX_set_default_verify_paths(tls_ctx);  /* system CA bundle */
    SSL_CTX_set_verify(tls_ctx, SSL_VERIFY_PEER, NULL);
    return 0;
}

/* Block until fd is readable/writable (for WANT_READ/WANT_WRITE). */
static int tls_wait(sock_t fd, int want_read)
{
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(fd, &fds);
    struct timeval tv = { .tv_sec = IO_TIMEOUT_S, .tv_usec = 0 };
    if (want_read) return select((int)fd + 1, &fds, NULL, NULL, &tv);
    return select((int)fd + 1, NULL, &fds, NULL, &tv);
}
#endif

/* One upstream connection: plain TCP, or TCP+TLS when use_tls.
 * Same send/recv/close verbs either way, so handle_get() never
 * branches on encryption mid-relay. */
typedef struct {
    sock_t fd;
#ifdef USE_TLS
    SSL *ssl;   /* NULL → plain HTTP */
#endif
} upstream_t;

static int upstream_connect(upstream_t *u, const char *host,
                            const char *port, int use_tls)
{
    u->fd = connect_remote(host, port);
    if (u->fd == SOCK_INVALID) return -1;
#ifdef USE_TLS
    u->ssl = NULL;
    if (!use_tls) return 0;
    if (!tls_ctx) { sock_close(u->fd); u->fd = SOCK_INVALID; return -1; }
    SSL *ssl = SSL_new(tls_ctx);
    if (!ssl) { sock_close(u->fd); u->fd = SOCK_INVALID; return -1; }
    SSL_set_tlsext_host_name(ssl, host);  /* SNI: which cert to serve */
    SSL_set1_host(ssl, host);             /* abort if cert ≠ host    */
    SSL_set_fd(ssl, (int)u->fd);
    int ok = 0;
    for (;;) {                            /* handshake (WANT-aware)  */
        int rc = SSL_connect(ssl);
        if (rc == 1) { ok = 1; break; }
        int err = SSL_get_error(ssl, rc);
        if ((err == SSL_ERROR_WANT_READ && tls_wait(u->fd, 1) > 0) ||
            (err == SSL_ERROR_WANT_WRITE && tls_wait(u->fd, 0) > 0))
            continue;
        break;
    }
    if (!ok) {
        SSL_free(ssl);
        sock_close(u->fd);
        u->fd = SOCK_INVALID;
        return -1;
    }
    u->ssl = ssl;
    return 0;
#else
    if (use_tls) { sock_close(u->fd); u->fd = SOCK_INVALID; return -1; }
    return 0;
#endif
}

/* send_all semantics over TCP or TLS. */
static int upstream_send(upstream_t *u, const char *buf, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
#ifdef USE_TLS
        if (u->ssl) {
            int n = SSL_write(u->ssl, buf + sent, (int)(len - sent));
            if (n > 0) { sent += (size_t)n; continue; }
            int err = SSL_get_error(u->ssl, n);
            if ((err == SSL_ERROR_WANT_READ && tls_wait(u->fd, 1) > 0) ||
                (err == SSL_ERROR_WANT_WRITE && tls_wait(u->fd, 0) > 0))
                continue;
            return -1;
        }
#endif
        int n = (int)send(u->fd, buf + sent, len - sent, 0);
        if (n <= 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

/* One recv / SSL_read. Returns bytes, 0 on clean end, -1 on error.
 * (TLS end = close_notify, mirroring TCP FIN under Connection: close.) */
static int upstream_recv(upstream_t *u, char *buf, size_t len)
{
    for (;;) {
#ifdef USE_TLS
        if (u->ssl) {
            int n = SSL_read(u->ssl, buf, (int)len);
            if (n >= 0) return n;
            int err = SSL_get_error(u->ssl, n);
            if ((err == SSL_ERROR_WANT_READ && tls_wait(u->fd, 1) > 0) ||
                (err == SSL_ERROR_WANT_WRITE && tls_wait(u->fd, 0) > 0))
                continue;
            return -1;
        }
#endif
        return (int)recv(u->fd, buf, len, 0);
    }
}

static void upstream_close(upstream_t *u)
{
#ifdef USE_TLS
    if (u->ssl) {
        SSL_shutdown(u->ssl);   /* polite close_notify */
        SSL_free(u->ssl);
        u->ssl = NULL;
    }
#endif
    if (u->fd != SOCK_INVALID) {
        sock_close(u->fd);
        u->fd = SOCK_INVALID;
    }
}

/* Relay bytes both ways until both sides close. The heart of the
 * CONNECT tunnel — and the reason HTTPS works through the proxy
 * without it ever seeing plaintext.
 *
 * Half-close handling matters: tools like nc shut down their write
 * side after sending (recv() returns 0) while still expecting a
 * reply. So EOF on side A only shuts down side B's *write* direction
 * (shutdown WR) and keeps relaying B→A until B also ends. A hard
 * error aborts everything. */
static void relay_loop(sock_t a, sock_t b)
{
    char buf[BUFFER_SIZE];
    sock_t pair[2] = { a, b };
    int open[2] = { 1, 1 };
    while (open[0] || open[1]) {
        fd_set rfds;
        FD_ZERO(&rfds);
        if (open[0]) FD_SET(a, &rfds);
        if (open[1]) FD_SET(b, &rfds);
        int maxfd = (a > b ? (int)a : (int)b) + 1;
        if (select(maxfd, &rfds, NULL, NULL, NULL) <= 0) return;
        for (int i = 0; i < 2; i++) {
            if (!open[i] || !FD_ISSET(pair[i], &rfds)) continue;
            int n = (int)recv(pair[i], buf, sizeof buf, 0);
            if (n < 0) return;                       /* hard error */
            if (n == 0) {                            /* orderly EOF */
                open[i] = 0;
                sock_shutdown_wr(pair[1 - i]);
                continue;
            }
            if (send_all(pair[1 - i], buf, (size_t)n) < 0) return;
        }
    }
}

/*────────────────────── Request handlers ────────────────────────*/

static int handle_get(sock_t client, const char *req, int reqlen)
{
    ParsedRequest *pr = ParsedRequest_create();
    if (!pr || ParsedRequest_parse(pr, req, reqlen) < 0) {
        ParsedRequest_destroy(pr);
        send_error(client, 400, "Bad Request");
        return -1;
    }
    if (!pr->method || strcmp(pr->method, "GET") != 0) {
        ParsedRequest_destroy(pr);
        send_error(client, 501, "Not Implemented");
        return -1;
    }

    char key[2048];
    if (cache_key(pr, key, sizeof key) < 0) {
        ParsedRequest_destroy(pr);
        send_error(client, 400, "Bad Request");
        return -1;
    }

    char *hit = NULL;
    int hitlen = 0;
    if (cache_lookup(key, &hit, &hitlen)) {       /* ── cache HIT ── */
        send_all(client, hit, (size_t)hitlen);
        free(hit);
        ParsedRequest_destroy(pr);
        return 0;
    }

    /* ── cache MISS: fetch from origin (TLS for https://) ── */
    int want_tls = pr->protocol && strcmp(pr->protocol, "https") == 0;
    const char *port = pr->port ? pr->port : (want_tls ? "443" : "80");
#ifndef USE_TLS
    if (want_tls) {
        ParsedRequest_destroy(pr);
        send_error(client, 501, "HTTPS origins need a USE_TLS build");
        return -1;
    }
#endif
    upstream_t srv;
    if (upstream_connect(&srv, pr->host, port, want_tls) < 0) {
        ParsedRequest_destroy(pr);
        send_error(client, 502, "Bad Gateway");
        return -1;
    }

    /* Shape the upstream request: HTTP/1.1 needs Host; force close so
     * the server's FIN (or TLS close_notify) ends our relay loop. */
    if (!ParsedHeader_get(pr, "Host"))
        ParsedHeader_set(pr, "Host", pr->host);
    ParsedHeader_set(pr, "Connection", "close");
    ParsedHeader_remove(pr, "Proxy-Connection");

    size_t reqlen2 = ParsedRequest_totalLen(pr);
    char *out = malloc(reqlen2 + 1);
    if (!out || ParsedRequest_unparse(pr, out, reqlen2 + 1) < 0) {
        free(out);
        upstream_close(&srv);
        ParsedRequest_destroy(pr);
        send_error(client, 500, "Internal Server Error");
        return -1;
    }
    ParsedRequest_destroy(pr);
    if (upstream_send(&srv, out, reqlen2) < 0) {
        free(out);
        upstream_close(&srv);
        return -1;
    }
    free(out);

    /* Relay server→client, accumulating a copy for the cache. */
    char chunk[BUFFER_SIZE];
    char *store = NULL;
    size_t stored = 0, store_cap = 0;
    int first = 1, cacheable = 0, ok = 1;
    for (;;) {
        int n = upstream_recv(&srv, chunk, sizeof chunk);
        if (n < 0) { ok = 0; break; }
        if (n == 0) break;                        /* FIN / close_notify */
        if (send_all(client, chunk, (size_t)n) < 0) { ok = 0; break; }
        if (first) { cacheable = is_cacheable_status(chunk, n); first = 0; }
        if (cacheable && stored <= MAX_ELEMENT_SIZE) {
            if (stored + (size_t)n > store_cap) {
                size_t want = stored + (size_t)n;
                if (want > MAX_ELEMENT_SIZE + 1) want = MAX_ELEMENT_SIZE + 1;
                char *bigger = realloc(store, want);
                if (!bigger) { cacheable = 0; }
                else { store = bigger; store_cap = want; }
            }
            if (cacheable && store) {
                size_t room = store_cap - stored;
                size_t take = (size_t)n < room ? (size_t)n : room;
                memcpy(store + stored, chunk, take);
                stored += take;
            }
        }
    }
    upstream_close(&srv);
    if (ok && cacheable && store && stored > 0 && stored <= MAX_ELEMENT_SIZE)
        cache_store(key, store, (int)stored);
    free(store);
    return 0;
}

/* "CONNECT example.com:443 HTTP/1.1" → TCP tunnel. Anything already
 * read past the headers (a pipelined TLS ClientHello) is pushed to
 * the server first, then relay_loop() owns both sockets. */
static int handle_connect(sock_t client, const char *req, int reqlen)
{
    if (strncmp(req, "CONNECT ", 8) != 0) {
        send_error(client, 400, "Bad Request");
        return -1;
    }
    const char *sp = strchr(req + 8, ' ');
    if (!sp || sp == req + 8) {
        send_error(client, 400, "Bad Request");
        return -1;
    }
    size_t authlen = (size_t)(sp - (req + 8));
    if (authlen == 0 || authlen >= 256) {
        send_error(client, 400, "Bad Request");
        return -1;
    }
    char authority[256];
    memcpy(authority, req + 8, authlen);
    authority[authlen] = '\0';

    char host[256], port[16] = "443";             /* HTTPS default */
    char *colon = strrchr(authority, ':');
    if (colon && colon != authority) {
        size_t hlen = (size_t)(colon - authority);
        if (hlen >= sizeof host) { send_error(client, 400, "Bad Request"); return -1; }
        memcpy(host, authority, hlen);
        host[hlen] = '\0';
        snprintf(port, sizeof port, "%s", colon + 1);
    } else {
        snprintf(host, sizeof host, "%s", authority);
    }

    sock_t server = connect_remote(host, port);
    if (server == SOCK_INVALID) {
        send_error(client, 502, "Bad Gateway");
        return -1;
    }
    static const char established[] =
        "HTTP/1.1 200 Connection Established\r\n"
        "Proxy-Agent: Proxy-Server/1.0\r\n"
        "\r\n";
    if (send_all(client, established, sizeof established - 1) < 0) {
        sock_close(server);
        return -1;
    }
    int hend = headers_end(req);                  /* pipelined bytes? */
    if (hend >= 0 && hend < reqlen)
        if (send_all(server, req + hend, (size_t)(reqlen - hend)) < 0) {
            sock_close(server);
            return -1;
        }
    relay_loop(client, server);
    sock_close(server);
    return 0;
}

/* One thread per client. The fd arrives via malloc'd memory (never a
 * pointer to the accept loop's stack variable — the loop reuses that
 * slot on the very next connection, a classic race). Detached threads
 * need no join; the active_clients counter is the only bookkeeping. */
static void *handle_client(void *arg)
{
    sock_t client = *(sock_t *)arg;
    free(arg);
    set_timeouts(client);

    int reqlen = 0;
    char *req = recv_headers(client, &reqlen);
    if (req) {
        if (strncmp(req, "CONNECT ", 8) == 0)
            handle_connect(client, req, reqlen);
        else
            handle_get(client, req, reqlen);
        free(req);
    }
    sock_close(client);

    pthread_mutex_lock(&active_lock);
    active_clients--;
    pthread_cond_signal(&active_cond);
    pthread_mutex_unlock(&active_lock);
    return NULL;
}

/*──────────────────────────────── main ──────────────────────────*/
int main(int argc, char *argv[])
{
    int port = DEFAULT_PORT;
    if (argc > 1) {
        port = atoi(argv[1]);
        if (port <= 0 || port > 65535) {
            fprintf(stderr, "Usage: %s [port 1-65535]\n", argv[0]);
            return 1;
        }
    }

#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        return 1;
    }
#else
    signal(SIGPIPE, SIG_IGN);   /* a client hanging up must not kill us */
#endif

#ifdef USE_TLS
    if (tls_init() < 0)
        fprintf(stderr, "warning: TLS init failed, https:// origins will 502\n");
#endif

    sock_t listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd == SOCK_INVALID) {
        perror("socket");
        return 1;
    }
    int reuse = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR,
               (const char *)&reuse, sizeof reuse);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("bind");
        sock_close(listen_fd);
        return 1;
    }
    if (listen(listen_fd, LISTEN_BACKLOG) < 0) {
        perror("listen");
        sock_close(listen_fd);
        return 1;
    }
    printf("proxy listening on port %d (GET cache + CONNECT tunnel%s)\n",
           port,
#ifdef USE_TLS
           ", https origins via TLS"
#else
           ", no TLS: https origins get 501"
#endif
        );
    fflush(stdout);

    for (;;) {
        sock_t client = accept(listen_fd, NULL, NULL);
#ifdef _WIN32
        if (client == INVALID_SOCKET) continue;
#else
        if (client < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }
#endif
        pthread_mutex_lock(&active_lock);
        while (active_clients >= MAX_CLIENTS)     /* semaphore-equivalent */
            pthread_cond_wait(&active_cond, &active_lock);
        active_clients++;
        pthread_mutex_unlock(&active_lock);

        sock_t *pfd = malloc(sizeof *pfd);
        if (!pfd) {
            sock_close(client);
            pthread_mutex_lock(&active_lock);
            active_clients--;
            pthread_cond_signal(&active_cond);
            pthread_mutex_unlock(&active_lock);
            continue;
        }
        *pfd = client;
        pthread_t tid;
        if (pthread_create(&tid, NULL, handle_client, pfd) != 0) {
            perror("pthread_create");
            free(pfd);
            sock_close(client);
            pthread_mutex_lock(&active_lock);
            active_clients--;
            pthread_cond_signal(&active_cond);
            pthread_mutex_unlock(&active_lock);
            continue;
        }
        pthread_detach(tid);   /* we never join; counter tracks them */
    }
    /*NOTREACHED*/
    return 0;
}
