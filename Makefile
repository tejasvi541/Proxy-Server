# Makefile — POSIX/macOS/Linux build. (Windows uses Proxy_Server.vcxproj.)
#
#   make            TLS on if OpenSSL is found, plain HTTP otherwise
#   make TLS=0      force plain-HTTP build (no OpenSSL needed)
#   make TLS=1      force TLS build (fails if OpenSSL is missing)

CC      ?= cc
CFLAGS  ?= -std=c17 -Wall -Wextra -Wpedantic
LDFLAGS ?= -pthread
LDLIBS  ?=

TLS ?= 1
ifeq ($(TLS),1)
  OPENSSL_PC := $(shell pkg-config --libs openssl 2>/dev/null)
  BREW_SSL   := $(shell brew --prefix openssl 2>/dev/null)
  ifneq ($(OPENSSL_PC),)
    CFLAGS += $(shell pkg-config --cflags openssl) -DUSE_TLS
    LDLIBS += $(OPENSSL_PC)
  else ifneq ($(BREW_SSL),)
    CFLAGS += -I$(BREW_SSL)/include -DUSE_TLS
    LDFLAGS += -L$(BREW_SSL)/lib
    LDLIBS += -lssl -lcrypto
  else ifneq ($(wildcard /opt/homebrew/opt/openssl@3/include/openssl/ssl.h),)
    CFLAGS += -I/opt/homebrew/opt/openssl@3/include -DUSE_TLS
    LDFLAGS += -L/opt/homebrew/opt/openssl@3/lib
    LDLIBS += -lssl -lcrypto
  else ifneq ($(wildcard /usr/local/opt/openssl@3/include/openssl/ssl.h),)
    CFLAGS += -I/usr/local/opt/openssl@3/include -DUSE_TLS
    LDFLAGS += -L/usr/local/opt/openssl@3/lib
    LDLIBS += -lssl -lcrypto
  else ifneq ($(wildcard /usr/include/openssl/ssl.h),)
    CFLAGS += -DUSE_TLS
    LDLIBS += -lssl -lcrypto
  else
    $(warning OpenSSL not found - building without TLS; https:// origins will return 501)
  endif
endif

all: proxy

proxy: proxy_server.o proxy_parse.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

%.o: %.c proxy_parse.h
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f proxy *.o

.PHONY: all clean
