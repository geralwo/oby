#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "native/io.h"
#include "native/native.h"
#include "native/net.h"
#include "core/string.h"

/*
 * TCP sockets. A socket is a file descriptor, so the io natives (`read`,
 * `write`, `io::close`, `io::last_error`) work on it unchanged; these natives
 * only create, connect and accept them.
 *
 *   net_connect(String host, int port) -> int               a connected socket
 *   net_listen(String host, int port, int backlog) -> int   a listening socket
 *   net_accept(int fd) -> int                               the next client
 *   net_local_port(int fd) -> int                           the bound port
 *   net_resolve(String host) -> String                      the first address
 *   net_set_nonblocking(int fd, int on) -> int              0 or -1
 *   net_poll(int fd, int timeout_ms) -> int                 1 readable, 0 timeout, -1 error
 *
 * Failures return -1 (or null) and record `errno` for `io::last_error`, like
 * the io natives. An empty host means "the local machine" for `connect` and
 * "any interface" for `listen`.
 *
 * POSIX only: a Windows build needs winsock (WSAStartup, closesocket) and its
 * own error mapping.
 */

/* getaddrinfo reports its own codes; `io::last_error` is errno, so map them. */
static int gai_errno(int code) {
    switch (code) {
        case EAI_AGAIN:  return EAGAIN;
        case EAI_MEMORY: return ENOMEM;
        case EAI_NONAME: return ENOENT;
        case EAI_SYSTEM: return errno;   /* already set */
        default:         return EINVAL;
    }
}

/* The host argument: a String, or NULL when it is empty (see above). */
static const char *host_arg(const Value *v) {
    if (v->tag != VAL_STRING || !v->as.str) return NULL;
    return v->as.str->length > 0 ? v->as.str->data : NULL;
}

/* net_connect(String host, int port) -> int : a connected socket, or -1. */
static Value nat_connect(Value *args, int argc) {
    (void)argc;
    const char *host = host_arg(&args[0]);
    char port[16];
    snprintf(port, sizeof port, "%lld", (long long)args[1].as.i);

    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = NULL;
    int rc = getaddrinfo(host, port, &hints, &res);
    if (rc != 0) {
        errno = gai_errno(rc);
        return io_fail();
    }

    int fd = -1;
    for (struct addrinfo *a = res; a; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, a->ai_addr, a->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) return io_fail();
    return val_int(fd);
}

/* net_listen(String host, int port, int backlog) -> int : a listening socket. */
static Value nat_listen(Value *args, int argc) {
    (void)argc;
    const char *host = host_arg(&args[0]);
    char port[16];
    snprintf(port, sizeof port, "%lld", (long long)args[1].as.i);
    int backlog = (int)args[2].as.i;
    if (backlog <= 0) backlog = 16;

    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;   /* an empty host binds any interface */
    struct addrinfo *res = NULL;
    int rc = getaddrinfo(host, port, &hints, &res);
    if (rc != 0) {
        errno = gai_errno(rc);
        return io_fail();
    }

    int fd = -1;
    for (struct addrinfo *a = res; a; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) continue;
        int on = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
        if (bind(fd, a->ai_addr, a->ai_addrlen) == 0 && listen(fd, backlog) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) return io_fail();
    return val_int(fd);
}

/* net_accept(int fd) -> int : the next client of a listening socket, or -1. */
static Value nat_accept(Value *args, int argc) {
    (void)argc;
    int fd = accept((int)args[0].as.i, NULL, NULL);
    if (fd < 0) return io_fail();
    return val_int(fd);
}

/* net_local_port(int fd) -> int : the port a socket is bound to (what a
   listener bound to port 0 needs to report), or -1. */
static Value nat_local_port(Value *args, int argc) {
    (void)argc;
    struct sockaddr_storage ss;
    socklen_t len = sizeof ss;
    if (getsockname((int)args[0].as.i, (struct sockaddr *)&ss, &len) != 0)
        return io_fail();
    if (ss.ss_family == AF_INET)
        return val_int(ntohs(((struct sockaddr_in *)&ss)->sin_port));
    if (ss.ss_family == AF_INET6)
        return val_int(ntohs(((struct sockaddr_in6 *)&ss)->sin6_port));
    return val_int(-1);
}

/* net_resolve(String host) -> String : the first address, as text, or null. */
static Value nat_resolve(Value *args, int argc) {
    (void)argc;
    const char *host = host_arg(&args[0]);
    if (!host) return val_null();

    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = NULL;
    int rc = getaddrinfo(host, NULL, &hints, &res);
    if (rc != 0) {
        errno = gai_errno(rc);
        return io_fail_null();
    }

    char text[INET6_ADDRSTRLEN];
    rc = getnameinfo(res->ai_addr, res->ai_addrlen, text, sizeof text, NULL, 0,
                     NI_NUMERICHOST);
    freeaddrinfo(res);
    if (rc != 0) {
        errno = gai_errno(rc);
        return io_fail_null();
    }
    String *s = string_new(text);
    return s ? val_string(s) : val_null();
}

/* net_set_nonblocking(int fd, int on) -> int : 0, or -1. */
static Value nat_set_nonblocking(Value *args, int argc) {
    (void)argc;
    int fd = (int)args[0].as.i;
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return io_fail();
    int want = args[1].as.i ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    if (fcntl(fd, F_SETFL, want) != 0) return io_fail();
    return val_int(0);
}

/* net_poll(int fd, int timeout_ms) -> int : 1 readable, 0 on timeout, -1. */
static Value nat_poll(Value *args, int argc) {
    (void)argc;
    struct pollfd p;
    p.fd = (int)args[0].as.i;
    p.events = POLLIN;
    p.revents = 0;
    int r = poll(&p, 1, (int)args[1].as.i);
    if (r < 0) return io_fail();
    if (r == 0) return val_int(0);
    return val_int(1);
}

void net_register_natives(void) {
    native_add("net_connect", nat_connect);
    native_add("net_listen", nat_listen);
    native_add("net_accept", nat_accept);
    native_add("net_local_port", nat_local_port);
    native_add("net_resolve", nat_resolve);
    native_add("net_set_nonblocking", nat_set_nonblocking);
    native_add("net_poll", nat_poll);
}
