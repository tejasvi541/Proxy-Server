# Proxy Server — Project Explained

> New to structs, threads, locks, or TLS? Start with
> [LEARN_THE_CODE.md](LEARN_THE_CODE.md) — a from-zero tutorial with
> diagrams for every concept used here. This file is the reference.

## 0. Verdict: the project is FINISHED ✅

All pieces are implemented, compile warning-free, and were verified
against a live origin server.

| Piece | Status |
|---|---|
| `proxy_parse.h` / `proxy_parse.c` (HTTP request parser) | Done |
| `proxy_server.c` — listen/accept loop | Done |
| `proxy_server.c` — one thread per client, capped at `MAX_CLIENTS` | Done |
| `proxy_server.c` — GET forwarding with `proxy_parse` | Done |
| `proxy_server.c` — HTTPS origin fetch over verified TLS (OpenSSL) | Done |
| `proxy_server.c` — LRU cache (`lookup`/`store`/eviction, mutex-guarded) | Done |
| `proxy_server.c` — CONNECT tunnel (HTTPS passes through) | Done |
| `proxy_server.c` — error replies (400/500/501/502) | Done |
| `Makefile` (macOS/Linux) + `Proxy_Server.vcxproj`/`.sln` (Windows) | Done |
| Tests (all passing, see §9) | Done |

**Test evidence** (macOS, `python3 -m http.server` as origin):

| Test | Result |
|---|---|
| GET through proxy, origin up (cache MISS) | 200, correct body |
| Same GET with origin **killed** (cache HIT) | 200, correct body |
| 20 parallel GETs | 20/20 correct |
| HTTPS GET via proxy-side TLS fetch (local `openssl s_server` origin) | 200 MISS, then 200 HIT with origin killed |
| HTTPS with wrong hostname (cert is for 127.0.0.1, asked as localhost) | 502 — verification rejects it |
| HTTPS on a `TLS=0` build | 501, with a message saying why |
| POST through proxy | 501 Not Implemented |
| Garbage bytes | 400 Bad Request |
| Dead upstream host | 502 Bad Gateway |
| Origin 404 relayed, then origin killed → re-requested | 502 (404s are **not** cached, only 200s) |
| `CONNECT 127.0.0.1:port` + pipelined GET through tunnel | `200 Connection Established`, then origin's reply |

---

## 1. What this project is and what it does

### 1.1 The idea in one paragraph

A **proxy server** sits between your browser and the real web server.
Your browser talks to the proxy; the proxy forwards the request to the
real ("origin") server, gets the answer, **caches** it, and hands it
back. This project is a **multithreaded HTTP proxy with an LRU cache**,
written in C: `http://` and `https://` GETs are parsed, fetched (TLS
with full certificate verification for https), cached, and served;
`CONNECT` requests open a blind TCP tunnel so browser HTTPS flows
through untouched.

### 1.2 Request flow

```
Browser ──GET http://site/page──▶ Proxy ──cache HIT?──▶ return saved copy
                                    │
                                    └─MISS──▶ thread ──▶ Origin :80
                                                  ◀── response ──╯
                                             store if 200 + reply to browser

Browser ──GET https://site/page─▶ Proxy ──cache HIT?──▶ return saved copy
                                    │
                                    └─MISS──▶ thread ──TLS handshake──▶ Origin :443
                                        (verify cert + hostname, SNI sent)
                                                  ◀── response ──╯
                                             store if 200 + reply to browser

Browser ──CONNECT site:443──▶ Proxy ──TCP connect──▶ Origin:443
                               ◀── 200 Established ──╯
                               ══ blind tunnel (select loop) ══
```

Each client gets its **own thread** so many browsers are served at
once. A **mutex + counter + condition variable** caps concurrency at
`MAX_CLIENTS` (the portable equivalent of a semaphore — macOS never
implemented unnamed POSIX semaphores, so `sem_init` would silently not
work there). A **mutex** guards the cache so two threads never corrupt
the linked list. **LRU** (Least Recently Used): every entry carries a
`lru` timestamp refreshed on each hit; when the cache is full, the
entry with the oldest timestamp is evicted first.

### 1.3 What the README gets wrong

`README.md` used to advertise request *encryption* and a `config.h` —
neither existed. Both are fixed now: HTTPS origin fetch is implemented
(§4.4b) and tunables are documented as `#define`s in `proxy_server.c`
(§4.1). There is still no TLS *termination* (the proxy never decrypts
CONNECT tunnels) and no `config.h`/`LICENSE` files — by design and
omission respectively, not oversight.

---

## 2. Prerequisites — what you need before reading the code

1. **C basics**: variables, `if`/`while`/`for`, functions, arrays.
2. **Pointers and the heap** (§6.1–6.2): `*`, `&`, `->`,
   `malloc`/`calloc`/`realloc`/`free`. The parser is 90% string
   ownership management.
3. **C strings**: `char*` = bytes ending in `'\0'`; `strlen`,
   `snprintf`, and why bounded functions exist.
4. **HTTP/1.x wire format**: `METHOD SP URL SP VERSION CRLF`, then
   `Key: Value CRLF` lines, then one empty `CRLF` line. A proxy
   receives the *absolute* form (`GET http://host/path …`) but must
   forward the *origin* form (`GET /path …`) — `unparse()` does exactly
   that conversion.
5. **Sockets at concept level**: `socket → bind → listen → accept` on
   the server side; `getaddrinfo → socket → connect` on the client
   side; `send`/`recv` move bytes; the server's `FIN` (read as `recv`
   returning 0) marks end-of-response when `Connection: close` is used.
6. **Threads at concept level**: one `pthread` per client; a mutex
   makes threads take turns touching shared data; a condition variable
   lets a thread sleep until another thread signals a change.
7. **Build toolchain**: compiler (`cc`/MSVC); `.c` → `.o` → linked
   program (§7, §8); OpenSSL dev libraries for the TLS build
   (`brew install openssl` on macOS, `libssl-dev` on Debian/Ubuntu).
8. **TLS at concept level**: a handshake negotiates encryption before
   any HTTP flows; the client checks the server's certificate against
   trusted CAs *and* that it names the host it asked for (hostname
   verification); SNI tells a multi-site server which cert to present.
   The proxy is a TLS *client* to origins — it never holds a server
   cert and never decrypts tunnels.

---

## 3. File map

```
proxy_parse.h    Declarations: structs + function signatures (the "menu").
proxy_parse.c    Definitions: the HTTP parser (the "kitchen").
proxy_server.c   The proxy: config, cache, sockets, threads, handlers.
Makefile         macOS/Linux build: compile each .c, then link.
Proxy_Server.vcxproj / .sln   Same job as the Makefile, for Visual Studio.
PROJECT_EXPLAINED.md (this file)  The docs; not compiled.
```

---

## 4. `proxy_server.c`, block by block

### 4.1 Platform shim and config

```c
#ifdef _WIN32
#   include <winsock2.h> ... typedef SOCKET sock_t;
#else
#   include <sys/socket.h> ... typedef int sock_t;
#endif
```

Windows sockets ("Winsock") need startup/teardown (`WSAStartup`) and
use `SOCKET` handles closed with `closesocket()`; POSIX uses plain
`int` fds closed with `close()`. The `sock_t` / `sock_close` /
`sock_shutdown_wr` aliases let the other 500 lines stay identical on
both platforms. Threads stay `pthread` everywhere (on native Windows
that means installing pthreads-w32 — or just building under WSL).

Config lives in `#define`s: `DEFAULT_PORT 8080`, `MAX_CLIENTS 100`,
`LISTEN_BACKLOG 128`, `BUFFER_SIZE 8192`, `HEADER_LIMIT 64KB`,
`MAX_CACHE_SIZE 4MB`, `MAX_ELEMENT_SIZE 512KB`, plus
`CONNECT_TIMEOUT_S 10` and `IO_TIMEOUT_S 30`. `#define` is preprocessor
text substitution — no memory, no type. `USE_TLS` is *not* here: it is
a build flag from the Makefile (`-DUSE_TLS`), so the same source
compiles with or without OpenSSL.

### 4.2 Connection limiter (mutex + cond, not semaphore)

```c
static pthread_mutex_t active_lock;
static pthread_cond_t  active_cond;
static int             active_clients = 0;
```

The accept loop increments under lock and *waits* while the count is at
the cap; each finishing thread decrements and signals. `static` = file
scope. This trio is a hand-rolled counting semaphore and exists only
because macOS lacks `sem_init` (see §1.2).

### 4.3 Cache: struct + `cache_key` + `lookup` + `evict` + `store`

The node keeps the stub's shape: `data`/`len` (raw response bytes),
`url` (key), `lru` (last-use time), `next` (linked list). Three globals
— `cache_head`, `cache_total`, `cache_lock` — are the program's only
shared mutable state.

- **`cache_key()`** builds the canonical key
  `"scheme://host:port/path"`, always spelling out scheme and port
  (443 for bare `https://`, 80 for bare `http://`), so `http://h/p`,
  `http://h:80/p`, and `https://h:443/p` are three different entries.
- **`cache_lookup()`** scans under lock; on a hit it `malloc`s a
  **copy** of the body, refreshes `lru = time(NULL)`, and returns the
  copy. Copying under lock is the key safety trick: the sender can use
  the bytes after unlock even if another thread evicts the entry a
  microsecond later — no dangling pointers, and the lock is never held
  during slow network I/O.
- **`cache_evict_lru_locked()`** removes the node with the smallest
  `lru` (the `_locked` suffix is the C convention for "caller must hold
  the mutex"). Head removal vs middle removal is the `victim_prev`
  branch.
- **`cache_store()`** refuses oversize bodies, replaces an existing URL
  in place, otherwise evicts LRU entries until the new body fits under
  `MAX_CACHE_SIZE` and pushes the node at the head. Every `malloc` is
  checked; a partial failure frees what was taken and stores nothing.

### 4.4 Socket utilities

- **`send_all()`** — one `send()` call rarely moves the whole buffer,
  so it loops until all `len` bytes are out. Returns 0/`-1`.
- **`send_error()`** — minimal `400`/`500`/`501`/`502` replies with
  `Connection: close` so the client doesn't hang waiting.
- **`recv_headers()`** — grows a buffer (`malloc` → doubling
  `realloc`, capped at `HEADER_LIMIT`) until `\r\n\r\n` appears. A peer
  that sends headers forever gets cut off (`NULL` → connection
  dropped).
- **`headers_end()`** — offset just past `\r\n\r\n`. Bytes already read
  beyond it (a TLS ClientHello pipelined after CONNECT headers) must be
  forwarded, not dropped.
- **`is_cacheable_status()`** — true only if the status line contains
  ` 200 `. Only 200s are stored (verified: a 404 is relayed but never
  cached).
- **`connect_remote()`** — `getaddrinfo` (IPv4 *or* IPv6) then, per
  address: non-blocking `connect` + `select` with a 10 s deadline, then
  verify via `SO_ERROR`. A plain blocking `connect()` to a dead host
  can stall for *minutes* and wedge a thread — this is the standard
  fix. Survivors get 30 s send/recv timeouts (`SO_RCVTIMEO`/
  `SO_SNDTIMEO`; Windows takes milliseconds, POSIX `struct timeval`).
- **`relay_loop()`** — `select()` on both sockets, forwarding each
  readable direction. Half-close correct: `recv` returning 0 on side A
  only shuts down side B's *write* direction while B→A keeps flowing.
  (The first version aborted the whole tunnel on any EOF and dropped
  pipelined replies — caught by live test, §9.) This loop is the entire
  CONNECT implementation: encrypted bytes pass through bytes the proxy
  never interprets.

### 4.4b TLS: `upstream_t`, `tls_init`, handshake, verified I/O

Compiled only with `-DUSE_TLS`. One shared `SSL_CTX` (thread-safe for
opening client connections) is created in `main` via `tls_init()`:
system CA bundle loaded, peer verification on. The `upstream_t` struct
(`fd` + `SSL*`, `NULL` for plain HTTP) gives the rest of the program
three verbs — `upstream_connect(host, port, use_tls)`,
`upstream_send` (send-all semantics), `upstream_recv` (one read, 0 on
clean end) — so `handle_get()` never branches on encryption mid-relay.

- **Handshake**: after the TCP connect, `SSL_set_tlsext_host_name`
  (SNI — tells multi-site servers which certificate to present) and
  `SSL_set1_host` (from then on, a certificate that doesn't name the
  host aborts the handshake). `SSL_connect` runs in a loop because it
  can return `WANT_READ`/`WANT_WRITE` mid-handshake; `tls_wait()`
  blocks in `select()` with the I/O timeout instead of spinning.
- **Failure = 502**: bad hostname, expired cert, self-signed cert from
  an untrusted CA — all surface as handshake failure, and the proxy
  sends `502 Bad Gateway` without relaying a single unauthenticated
  byte. Verified live: cert for `127.0.0.1` requested as `localhost`
  → 502. (Lab CAs can be trusted via `SSL_CERT_FILE=/path/ca.pem`.)
- **End of response**: the TLS `close_notify` plays the role of TCP
  FIN, mirroring the `Connection: close` design; `upstream_close()`
  sends our `close_notify` (`SSL_shutdown`) then frees and closes.
- **Without `USE_TLS`**: the same struct exists minus the `SSL*`
  field, `upstream_connect` refuses `use_tls`, and `handle_get`
  answers `501 HTTPS origins need a USE_TLS build` before dialing.

### 4.5 `handle_get()` — parse → cache? → fetch → relay → store

1. `ParsedRequest_parse`; failure → **400**. Non-`GET` method → **501**.
2. Build `key`; `cache_lookup` hit → `send_all` the copy, done.
3. Miss: pick the port (`pr->port`, else 443 for https / 80 for
   http) and `upstream_connect()` — plain TCP or verified TLS.
   Failure → **502** (connect refused *and* cert rejected alike).
4. Shape the upstream request: add `Host` if absent (HTTP/1.1
   requires it), force `Connection: close` (so the server's FIN
   terminates our relay loop), strip `Proxy-Connection` (a
   proxy-only header the origin shouldn't see). `ParsedRequest_unparse`
   emits the origin-form request line + headers into an exactly-sized
   buffer (`totalLen` + 1; `unparse` doesn't NUL-terminate, so the
   explicit `+1` and length-based `send_all` matter).
5. Relay loop: each chunk goes to the client immediately (streaming —
   the client doesn't wait for the whole body) while a copy accumulates
   via `realloc`, capped at `MAX_ELEMENT_SIZE + 1` as a tripwire.
   Reads go through `upstream_recv`, so TLS close_notify ends an https
   relay exactly like FIN ends a plain one.
6. On clean FIN, if the status was 200 and the body fit → `cache_store`.
   Client disconnect mid-stream aborts the store (`ok` flag).

### 4.6 `handle_connect()` — the tunnel entrance

Parses `CONNECT host:port` manually (the HTTP parser only accepts
`http://` URLs, so CONNECT never reaches it), defaults the port to 443,
connects, replies `200 Connection Established`, pushes any pipelined
bytes read past the headers, and hands both sockets to `relay_loop()`.

### 4.7 `handle_client()` — one thread's whole life

Receives the malloc'd fd (see §6.7 for why it's malloc'd), reads the
head, dispatches on the `CONNECT ` prefix, closes the client socket,
then decrements `active_clients` and signals the accept loop. Threads
are created **detached** (`pthread_detach`) — no `join` needed; the
counter is the only bookkeeping.

### 4.8 `main()` — setup + accept loop

Parses an optional port argument, ignores `SIGPIPE` (without this, one
client hanging up mid-`send` kills the whole proxy with a signal),
`socket → SO_REUSEADDR → bind(INADDR_ANY) → listen`, then forever:
`accept` → wait at the cap → increment → hand the fd to a new detached
thread. `SO_REUSEADDR` lets you restart the proxy immediately without
"Address already in use". Every failure path (malloc/thread-create)
unwinds the counter it just took — otherwise the cap would leak down to
zero live threads over time.

---

## 5. `proxy_parse.h` + `proxy_parse.c`, block by block

### 5.1 `proxy_parse.h` — the contract

- **Include guard** (`#ifndef PROXY_PARSE_H` … `#endif`): stops the
  header being pasted twice (§8.2).
- `#define DEBUG_PROXY_PARSE 0`: compile-time debug switch.
- **`ParsedHeader`**: one `Key: Value` pair with cached lengths.
- **`ParsedRequest`**: the split request — `method`, `protocol`
  (always `"http"`), `host`, `port` (`NULL` = default 80), `path`,
  `version`, `raw_request_line` (owned copy that tokens point into),
  and a growable `headers` array (`headers_in_use` vs
  `headers_capacity`).
- **Declarations only** — bodies live in the `.c` file (§8.1).

### 5.2 `proxy_parse.c` — the implementation

**Compat block.** MSVC spells things `_strdup`/`strtok_s`/`_stricmp`;
POSIX spells them `strdup`/`strtok_r` with `strcasecmp` in
`<strings.h>`. One `#ifdef _WIN32` maps the portable `x_strdup`/
`x_strtok` names to whichever spelling exists.

**`xcalloc`.** Zeroing `calloc` wrapper that exits on OOM, keeping
callers clean (at the cost of dying instead of degrading — fine for
teaching code).

**`ensure_header_capacity`.** Doubling array growth (4→8→16…), amortized
O(1) per header.

**`create` / `destroy`.** Zeroed struct out; frees headers, every string
field, then the struct. Every `create` pairs with exactly one
`destroy`.

**`parse`, step 1 — request line.** Finds the first CRLF, copies the
line (tokens are destructive, so never the caller's buffer), splits
`METHOD SP URL SP VERSION` with re-entrant tokenizing.

**Step 2 — URL.** Skips `http://` or `https://` (anything else → `-1`),
records the scheme in `protocol`, snapshots the path *first*, then
cuts and splits `host`/`port`. `port` stays `NULL` when absent — the
server maps that to 80/443 by scheme. (Cleanup fixed a real bug here:
host used to swallow the path, port used to swallow the path.)

**Step 3 — headers.** Line by line until the empty CRLF; split at the
first `':'` within the line bounds; colon-less lines skipped, not
fatal; value leading-space trimmed; lengths cached.

**`unparse` / `unparse_headers`.** `snprintf`-based rebuild returning
bytes written or `-1` on overflow; emits the origin-form path, exactly
what the origin server expects. **Does not NUL-terminate** — the
server code passes the returned length to `send_all` and sizes buffers
with `totalLen + 1`.

**Length helpers** let callers allocate once, `unparse` once.
**get/set/remove** are a case-insensitive (`strcasecmp`) linear scan;
`remove` closes the gap with `memmove`.
**`debug_proxy_parse`** is a `va_list` printf gated on the debug flag.

### 5.3 Worked example (the single copy)

```c
const char *c =
  "GET http://www.google.com:80/index.html HTTP/1.0\r\n"
  "Content-Length: 80\r\n"
  "If-Modified-Since: Sat, 29 Oct 1994 19:43:31 GMT\r\n\r\n";

ParsedRequest *req = ParsedRequest_create();
if (ParsedRequest_parse(req, c, (int)strlen(c)) < 0) { /* fail */ }

printf("Method:%s\n", req->method);  /* GET */
printf("Host:%s\n",   req->host);    /* www.google.com */

size_t rlen = ParsedRequest_totalLen(req);
char *b = malloc(rlen + 1);          /* +1 for YOUR '\0' */
int n = ParsedRequest_unparse(req, b, rlen + 1);
b[n] = '\0';

ParsedHeader *h = ParsedHeader_get(req, "If-Modified-Since");
ParsedHeader_remove(req, "If-Modified-Since");
ParsedHeader_set(req, "Last-Modified", "Wed, 12 Feb 2014 12:43:31 GMT");

ParsedRequest_destroy(req);          /* always, when done */
free(b);
```

---

## 6. Syntax guide — the non-beginner-friendly bits

### 6.1 Pointers, `*`, `&`, `->`

```c
char *host;          /* host HOLDS an address of chars, not chars */
host = x_strdup("example.com");
pr->host = host;     /* -> = "follow the pointer, take the field" */
```

`char *p` is an address; `*p` the char there; `&x` the address of `x`
(`&saveptr`, `&headers[i]`); `pr->host` is `(*pr).host` written short.
A C string is bytes ending in `'\0'` — every function here assumes the
terminator exists.

### 6.2 Heap management (`malloc` / `calloc` / `realloc` / `free`)

| Call | Does |
|---|---|
| `malloc(n)` | `n` uninitialized bytes |
| `calloc(c, s)` | `c*s` bytes, zeroed |
| `realloc(p, n)` | resize `p`'s block (may move — always reassign!) |
| `free(p)` | give it back; touching `p` after is use-after-free |

Rule: **whoever `strdup`s/`calloc`s owns it and must `free` it.**
`destroy`, `cache_evict`, and the handler cleanups are where each
allocation's owner pays its debt.

### 6.3 `typedef struct` and the self-pointer

```c
typedef struct cache_element {
    ...
    struct cache_element *next;  /* inside: must still say struct ... */
} cache_element;                 /* after: plain `cache_element` works */
```

Inside the braces the short name doesn't exist yet, hence the
`struct`-qualified self-pointer — that single pointer is the whole
linked list.

### 6.4 `static`, `size_t`, `const`, `socklen_t`

- `static` at file scope = visible only in this file (`cache_head`,
  `proxy` globals). (Inside a function it would instead mean "keeps
  value between calls" — same keyword, different job.)
- `size_t` = unsigned type for sizes/counts; `socklen_t` = unsigned
  type for socket address lengths. Neither can be negative, so mixing
  them with `int` needs care (the code casts at the boundary).
- `const char *buf` = "I won't modify your bytes" — honored by copying
  before tokenizing.

### 6.5 String/buffer functions

- Re-entrant tokenizers (`strtok_r`/`strtok_s`) split on delimiters,
  keep position in a caller-owned `ctx` (thread-safe, unlike `strtok`),
  and **write `'\0'` into the input** — hence the copy-first rule.
- `strchr`/`strstr` find a char/substring; `memchr(p, ':', n)` finds a
  char within `n` bytes (safe without a terminator).
- `memmove` = `memcpy` safe for overlapping blocks (header removal,
  space trimming).
- `snprintf(dst, left, …)` never writes past `left` bytes and returns
  what it *wanted* — `n >= left` means "didn't fit" (used for key
  building and error pages).

### 6.6 `va_list` (the debug helper)

```c
void debug_proxy_parse(const char *fmt, ...)  /* ... = any extra args */
{
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);       /* printf that takes a va_list */
    va_end(ap);
}
```

### 6.7 Threads: `pthread_create`, detach, and the malloc'd fd

```c
sock_t *pfd = malloc(sizeof *pfd);
*pfd = client;
pthread_create(&tid, NULL, handle_client, pfd);
pthread_detach(tid);
```

The fd is heap-allocated because the accept loop's stack slot is reused
on the *next* connection — passing `&client` would race the new thread
against the next `accept`. `pthread_detach` tells the runtime to
reclaim the thread automatically on exit (no `pthread_join`); the
`active_clients` counter replaces joining as bookkeeping. Mutex ops
bracket every touch of shared state; `pthread_cond_wait` atomically
sleeps *and* releases the mutex, re-acquiring it on wakeup.

### 6.8 Sockets: fds, `select`, `fd_set`, `getaddrinfo`

A socket is just a file descriptor you `send`/`recv` on. `select(maxfd,
&set, …)` sleeps until one of the watched fds is readable — the tunnel
sleeps in `select` instead of burning CPU polling. `FD_ZERO`/`FD_SET`/
`FD_ISSET` manage the watch-set bitmask. `getaddrinfo(host, port, …)`
does DNS + service resolution into a linked list of dialable addresses
(IPv4 and IPv6); the code tries each until one connects — this is why
the proxy needs no manual DNS code. `shutdown(fd, SHUT_WR)` closes one
direction of a connection (the half-close in `relay_loop`); `close`
closes both.

### 6.9 TLS: `SSL_CTX`, `SSL`, and `WANT_READ`

- `SSL_CTX` = factory holding *configuration* (CA store, verify mode);
  one is shared by all threads. `SSL` = one live connection's state
  (keys, buffers), created per fetch with `SSL_new` and freed after.
- `SSL_connect`/`SSL_read`/`SSL_write` don't behave like plain
  syscalls: they can ask to be called again once the socket is readable
  (`SSL_ERROR_WANT_READ`) or writable (`WANT_WRITE`) — e.g. when a
  re-handshake arrives mid-transfer. The code answers with `select()`,
  never a busy loop.
- `SSL_set_tlsext_host_name` sends SNI (Server Name Indication:
  "I'm looking for *this* hostname" — required when one IP serves many
  sites). `SSL_set1_host` arms post-handshake hostname verification
  against the peer certificate's SAN/CN fields. System CAs come from
  `SSL_CTX_set_default_verify_paths`, overridable at runtime with the
  `SSL_CERT_FILE` / `SSL_DIR` environment variables (no rebuild).

---

## 7. How compiling works here

Three stages — **preprocess → compile → link**:

```
proxy_parse.h ──┐ (pasted by #include)
                ▼
proxy_server.c ──preprocess──▶ compile ──▶ proxy_server.o ──┐
proxy_parse.c  ──preprocess──▶ compile ──▶ proxy_parse.o  ──├──link──▶ proxy
proxy_parse.h ──┘ (pasted by #include)                        ▲
                                                    -pthread (threads lib)
```

1. **Preprocess.** Headers pasted in, `#ifdef _WIN32` branches chosen,
   `#define`s expanded.
2. **Compile.** Each `.c` translated *independently* to a `.o`. Calls
   to not-yet-seen code are trusted on the header's word. Warnings
   (`-Wall -Wextra -Wpedantic`) fire here — the build is warning-free.
3. **Link.** Object files merged, every call matched to its definition,
   `-pthread` pulls in the thread library. Calling a declared-but-
   undefined function fails here (`undefined reference`) — which is why
   the old stub's empty cache declarations only survived while nothing
   called them.

Concrete commands (what `make all` runs on this Mac — OpenSSL found
via Homebrew, so `-DUSE_TLS` is on):

```bash
cc -std=c17 -Wall -Wextra -Wpedantic -I/opt/homebrew/opt/openssl@3/include -DUSE_TLS -c proxy_server.c -o proxy_server.o
cc -std=c17 -Wall -Wextra -Wpedantic -I/opt/homebrew/opt/openssl@3/include -DUSE_TLS -c proxy_parse.c -o proxy_parse.o
cc -o proxy proxy_server.o proxy_parse.o -pthread -L/opt/homebrew/opt/openssl@3/lib -lssl -lcrypto
./proxy 8080
```

The Makefile finds OpenSSL through a fallback chain — `pkg-config`,
then `brew --prefix openssl`, then well-known Homebrew prefixes, then
`/usr/include` — and degrades gracefully: with no OpenSSL it warns and
builds the plain-HTTP proxy (`-DUSE_TLS` simply absent, all TLS code
compiled out by `#ifdef`). `make TLS=0` forces that build; `make TLS=1`
forces TLS and fails loudly if OpenSSL is missing.

On Windows the same three stages run inside Visual Studio via
`Proxy_Server.vcxproj` (which files to compile) + `.sln` (which
projects), linking `ws2_32.lib` for Winsock. `make clean` / `Rebuild`
delete `.o` files and the binary for a fresh build.

---

## 8. How header files and linking work here, precisely

### 8.1 Declaration (`.h`) vs definition (`.c`)

```c
/* proxy_parse.h — "this function exists, trust me" */
ParsedRequest *ParsedRequest_create(void);

/* proxy_parse.c — the actual machine code */
ParsedRequest *ParsedRequest_create(void)
{
    return (ParsedRequest *)xcalloc(1, sizeof(ParsedRequest));
}
```

Including the `.c` instead would compile its code into *both* `.o`
files → `multiple definition` at link time. Bodies live in `.c`
exactly once; the `.h` is the shared promise.

### 8.2 Include guards

```c
#ifndef PROXY_PARSE_H
#define PROXY_PARSE_H
... everything ...
#endif
```

Second inclusion sees the symbol defined and skips the body — no double
declarations, no compile error.

### 8.3 `"..."` vs `<...>` includes

`"proxy_parse.h"` searches the **project directory first**; `<stdio.h>`
searches **system paths only**. The Makefile needs no `-I` flag because
the project header sits beside the sources.

### 8.4 Linking this project, end to end

`proxy_server.o` references parser symbols it doesn't define;
`proxy_parse.o` exports them; `-pthread` resolves the thread calls
against the system library, and `-lssl -lcrypto` resolve the
`SSL_*` calls against OpenSSL (TLS builds only — the `#ifdef USE_TLS`
means a plain build references no OpenSSL symbol at all, so nothing
dangles). On Windows: `ws2_32.lib` for sockets plus
pthreads-w32 for threads; TLS needs OpenSSL Via vcpkg plus `USE_TLS`
in the project defines (see the note in `Proxy_Server.vcxproj`). The
linker's job is set-matching: every *used* symbol must be defined
exactly once across all `.o` files and libraries.

---

## 9. Usage and testing

Yes, macOS is fully supported — the whole §0 matrix ran on a Mac
(Apple Silicon, Homebrew OpenSSL, `python3 -m http.server` and
`openssl s_server` as origins).

```bash
make all            # TLS on if OpenSSL found, else plain HTTP
make TLS=0          # force plain-HTTP build
./proxy 8080        # listen on 8080 (default if omitted)
```

Point a client at it (`curl -x http://127.0.0.1:8080 http://…`, or the
OS/browser proxy settings: host `localhost`, port `8080`). Two kinds
of HTTPS flow through:

- `curl -x … https://site/` → curl opens a **CONNECT tunnel** and does
  TLS itself end-to-end (proxy sees only ciphertext, nothing cached).
- `GET https://…` as the request line (rarer; some forward-proxy
  clients) → the **proxy fetches over its own verified TLS** and
  caches the result. Test it with a raw socket against a local TLS
  origin — full script in README spirit:

```bash
mkdir -p /tmp/tlsorg && echo HI > /tmp/tlsorg/hi.txt
openssl req -x509 -newkey rsa:2048 -keyout /tmp/tlsorg/key.pem \
  -out /tmp/tlsorg/cert.pem -days 1 -nodes -subj "/CN=127.0.0.1" \
  -addext "subjectAltName=IP:127.0.0.1"
(cd /tmp/tlsorg && openssl s_server -accept 18443 \
  -cert cert.pem -key key.pem -WWW &)
SSL_CERT_FILE=/tmp/tlsorg/cert.pem ./proxy 8080 &
printf 'GET https://127.0.0.1:18443/hi.txt HTTP/1.1\r\nHost: 127.0.0.1:18443\r\nConnection: close\r\n\r\n' \
  | nc -w 8 127.0.0.1 8080     # MISS → HI; kill s_server, repeat → HIT
```

The tunnel test pipes a CONNECT plus a pipelined GET through `nc` and
expects `200 Connection Established` followed by the origin's reply —
this exercises the half-close path (§4.4), since `nc` shuts down its
write side after sending. Debugging note this project earned the hard
way: `curl -x` with an `https://` URL always sends CONNECT, never
`GET https://…` — to exercise proxy-side TLS you must use a raw socket
as above.

---

## 10. Limitations and natural next steps

- One request per connection (`Connection: close` everywhere) — no
  keep-alive or pipelined GETs.
- Only `GET` is cached/forwarded; `POST`/`PUT`/… get 501 (as does
  `https://` on a non-TLS build).
- Only `200 OK` is cached; no `Cache-Control`/`Expires`/`ETag`
  awareness, no per-host isolation.
- No filtering, logging, auth, rate limiting, or stats endpoint.
- No graceful shutdown (Ctrl-C is the off switch); no config file
  (tune the `#define`s).
- Windows build needs pthreads-w32; verified target is POSIX.

Each of these is a clean, bounded follow-up: keep-alive needs
Content-Length/chunked framing; POST forwarding is `handle_get`
minus the cache; a stats page is a new handler branch.

---

## 11. What changed across both cleanups (for reviewers)

**First pass** (stub era): removed committed build outputs (`.vs/`,
`x64/`, `*.user`, …), fixed the broken `.gitignore`, fixed
`#include<stdio.h>;` and the fictional `Mspthrd.h`, fixed the cache
node self-pointer, added the `Makefile`, deduped the 75-line example,
fixed host/port swallowing the path, added the `x_strdup`/`x_strtok`
portability layer.

**Second pass** (completion): implemented everything in §0 —
`socket/bind/listen/accept` loop, detached-thread-per-client with
mutex+cond cap (deliberately *not* `sem_init`: broken on macOS),
`send_all`/`recv_headers`/`connect_remote` (non-blocking + `select`
deadline) utilities, GET fetch/relay/store with 200-only caching,
CONNECT tunnel with half-close-correct `relay_loop`, error replies,
SIGPIPE immunity, 30 s I/O timeouts, and a Winsock shim for Windows.
The `workers[]` array from the stub was dropped (detached threads +
counter replace it). All §0 tests pass; build is warning-free under
`-Wall -Wextra -Wpedantic`.

**Third pass** (encryption): parser accepts `https://` (scheme-aware
default ports, scheme-qualified cache keys); new `upstream_t`
abstraction (TCP or verified-TLS with SNI + hostname check + system
CAs); WANT-aware handshake/I/O; `close_notify` as end-of-response;
`501` for https on non-TLS builds; Makefile OpenSSL autodetection with
`TLS=` override; README gained macOS/Linux setup + run guide;
Windows note for enabling `USE_TLS` in `Proxy_Server.vcxproj`.
Verified: TLS MISS then HIT with origin killed, hostname-mismatch 502,
plain-HTTP regression, `TLS=0` 501. Debugging footnote: `curl -x` +
https URL always CONNECTs — proxy-side TLS needs a raw socket test.
