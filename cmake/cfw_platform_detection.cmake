# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

include_guard(GLOBAL)

include(CheckCSourceCompiles)
include(CheckSymbolExists)

# Captured at include time: inside the macro below,
# CMAKE_CURRENT_LIST_DIR would resolve to the caller's directory instead.
set(CFW_PLATFORM_CONFIG_DIR "${CMAKE_CURRENT_LIST_DIR}")

macro(cfw_detect_platform)
    # A macro (not a function) is used deliberately: every set() below lands
    # in the calling directory scope, so generated values are visible both to
    # configure_file() here and to subdirectories such as tests/.
    foreach(_cfw_platform
            WINDOWS
            MACOS
            LINUX
            ANDROID
            FREEBSD
            OPENBSD
            NETBSD
            DRAGONFLY
            POSIX)
        set(CFW_PLATFORM_${_cfw_platform} 0)
    endforeach()

    if(WIN32)
        set(CFW_PLATFORM_WINDOWS 1)
    else()
        set(CFW_PLATFORM_POSIX 1)

        if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
            set(CFW_PLATFORM_MACOS 1)
        elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR
               CMAKE_SYSTEM_NAME STREQUAL "Android" OR CMAKE_ANDROID)
            set(CFW_PLATFORM_LINUX 1)
            if(CMAKE_SYSTEM_NAME STREQUAL "Android" OR CMAKE_ANDROID)
                set(CFW_PLATFORM_ANDROID 1)
            endif()
        elseif(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
            set(CFW_PLATFORM_FREEBSD 1)
        elseif(CMAKE_SYSTEM_NAME STREQUAL "OpenBSD")
            set(CFW_PLATFORM_OPENBSD 1)
        elseif(CMAKE_SYSTEM_NAME STREQUAL "NetBSD")
            set(CFW_PLATFORM_NETBSD 1)
        elseif(CMAKE_SYSTEM_NAME STREQUAL "DragonFly")
            set(CFW_PLATFORM_DRAGONFLY 1)
        endif()
    endif()

    foreach(_cfw_feature
            EPOLL
            KQUEUE
            POLL
            GETRANDOM
            ARC4RANDOM_BUF
            ACCEPT4
            GETADDRINFO
            SOCK_NONBLOCK
            SOCK_CLOEXEC
            MSG_NOSIGNAL
            MSG_DONTWAIT
            SO_NOSIGPIPE
            SO_REUSEPORT
            TCP_NODELAY
            O_NOFOLLOW
            O_CLOEXEC
            OPENAT
            CLOCK_MONOTONIC
            CLOCK_REALTIME
            SC_PAGESIZE
            SC_PAGE_SIZE)
        set(CFW_HAVE_${_cfw_feature} 0)
    endforeach()

    if(NOT WIN32)
        set(CFW_PLATFORM_CHECK_DEFINITIONS)
        if(CFW_PLATFORM_LINUX)
            list(APPEND CFW_PLATFORM_CHECK_DEFINITIONS "-D_GNU_SOURCE")
        elseif(CFW_PLATFORM_MACOS)
            list(APPEND CFW_PLATFORM_CHECK_DEFINITIONS
                "-D_POSIX_C_SOURCE=200809L"
                "-D_DARWIN_C_SOURCE")
        elseif(CFW_PLATFORM_FREEBSD OR CFW_PLATFORM_OPENBSD OR
               CFW_PLATFORM_NETBSD OR CFW_PLATFORM_DRAGONFLY)
            list(APPEND CFW_PLATFORM_CHECK_DEFINITIONS
                "-D_POSIX_C_SOURCE=200809L"
                "-D_BSD_SOURCE")
        else()
            list(APPEND CFW_PLATFORM_CHECK_DEFINITIONS
                "-D_POSIX_C_SOURCE=200809L")
        endif()

        set(CMAKE_REQUIRED_DEFINITIONS "${CFW_PLATFORM_CHECK_DEFINITIONS}")
        # Probes must fail on implicit function declarations: without this,
        # a missing libc function (e.g. accept4 on the BSDs) still "compiles"
        # and the feature is wrongly enabled, breaking the -Werror build.
        set(CMAKE_REQUIRED_FLAGS "-Werror=implicit-function-declaration")

        check_c_source_compiles([==[
#include <sys/epoll.h>
int main(void)
{
    struct epoll_event event;
    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd >= 0) {
        (void)epoll_ctl(epfd, EPOLL_CTL_ADD, 0, &event);
        (void)epoll_wait(epfd, &event, 1, 0);
    }
    return EPOLLIN | EPOLLOUT | EPOLLERR | EPOLLHUP | EPOLL_CTL_ADD;
}
]==] CFW_CHECK_HAVE_EPOLL)

        check_c_source_compiles([==[
#include <fcntl.h>
#include <sys/event.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
int main(void)
{
    int kqfd = kqueue();
    struct kevent changes[2];
    struct kevent events[1];
    struct timespec timeout;
    timeout.tv_sec = 0;
    timeout.tv_nsec = 0;
    EV_SET(&changes[0], 0, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, 0);
    EV_SET(&changes[1], 0, EVFILT_WRITE, EV_DELETE, 0, 0, 0);
    if (kqfd >= 0) {
        (void)kevent(kqfd, changes, 2, events, 1, &timeout);
        (void)fcntl(kqfd, F_SETFD, FD_CLOEXEC);
        (void)close(kqfd);
    }
    return EV_ERROR | EV_EOF | EVFILT_READ | EVFILT_WRITE;
}
]==] CFW_CHECK_HAVE_KQUEUE)

        check_c_source_compiles([==[
#include <poll.h>
#include <sys/types.h>
int main(void)
{
    struct pollfd pfds[1];
    int ready = poll(pfds, 1, 0);
    return ready + (int)(POLLIN | POLLOUT | POLLERR | POLLHUP | POLLNVAL);
}
]==] CFW_CHECK_HAVE_POLL)

        check_c_source_compiles([==[
#include <sys/random.h>
#include <unistd.h>
int main(void)
{
    unsigned char buffer[8];
    ssize_t got = getrandom(buffer, sizeof(buffer), 0);
    return got < 0;
}
]==] CFW_CHECK_HAVE_GETRANDOM)

        check_c_source_compiles([==[
#include <stdlib.h>
int main(void)
{
    unsigned char buffer[8];
    arc4random_buf(buffer, sizeof(buffer));
    return 0;
}
]==] CFW_CHECK_HAVE_ARC4RANDOM_BUF)

        check_c_source_compiles([==[
#include <stddef.h>
#include <sys/socket.h>
#include <sys/types.h>
int main(void)
{
    int fd = accept4(-1, NULL, NULL, 0);
    return fd;
}
]==] CFW_CHECK_HAVE_ACCEPT4)

        check_symbol_exists(SOCK_NONBLOCK "sys/socket.h"
                            CFW_CHECK_HAVE_SOCK_NONBLOCK)
        check_symbol_exists(SOCK_CLOEXEC "sys/socket.h"
                            CFW_CHECK_HAVE_SOCK_CLOEXEC)
        check_symbol_exists(MSG_NOSIGNAL "sys/socket.h"
                            CFW_CHECK_HAVE_MSG_NOSIGNAL)
        check_symbol_exists(MSG_DONTWAIT "sys/socket.h"
                            CFW_CHECK_HAVE_MSG_DONTWAIT)
        check_symbol_exists(SO_NOSIGPIPE "sys/socket.h"
                            CFW_CHECK_HAVE_SO_NOSIGPIPE)
        # SO_REUSEPORT is a macro on some platforms; compile a program
        # that performs the exact setsockopt the server issues.
        check_c_source_compiles([==[
#include <sys/socket.h>
#include <netinet/in.h>
int main(void)
{
    int one = 1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    return setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one,
                      (socklen_t)sizeof(one));
}
]==] CFW_CHECK_HAVE_SO_REUSEPORT)
        check_symbol_exists(O_NOFOLLOW "fcntl.h"
                            CFW_CHECK_HAVE_O_NOFOLLOW)
        check_symbol_exists(O_CLOEXEC "fcntl.h"
                            CFW_CHECK_HAVE_O_CLOEXEC)
        check_symbol_exists(openat "fcntl.h" CFW_CHECK_HAVE_OPENAT)
        check_symbol_exists(getaddrinfo "netdb.h"
                            CFW_CHECK_HAVE_GETADDRINFO)
        check_c_source_compiles([==[
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
int main(void)
{
    int one = 1;
    return setsockopt(0, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}
]==] CFW_CHECK_HAVE_TCP_NODELAY)

        check_c_source_compiles([==[
#include <time.h>
int main(void)
{
    struct timespec ts;
    return clock_gettime(CLOCK_MONOTONIC, &ts);
}
]==] CFW_CHECK_HAVE_CLOCK_MONOTONIC)

        check_c_source_compiles([==[
#include <time.h>
int main(void)
{
    struct timespec ts;
    return clock_gettime(CLOCK_REALTIME, &ts);
}
]==] CFW_CHECK_HAVE_CLOCK_REALTIME)

        check_c_source_compiles([==[
#include <unistd.h>
int main(void)
{
    long result = sysconf(_SC_PAGESIZE);
    return result <= 0;
}
]==] CFW_CHECK_HAVE_SC_PAGESIZE)

        check_c_source_compiles([==[
#include <unistd.h>
int main(void)
{
    long result = sysconf(_SC_PAGE_SIZE);
    return result <= 0;
}
]==] CFW_CHECK_HAVE_SC_PAGE_SIZE)

        unset(CMAKE_REQUIRED_DEFINITIONS)
        unset(CMAKE_REQUIRED_FLAGS)
    endif()

    if(WIN32)
        # Winsock2 provides getaddrinfo (ws2tcpip.h) and TCP_NODELAY on
        # every supported Windows target; the POSIX feature probes above
        # do not run on Windows and the rest stay 0 (ADR 0072).
        set(CFW_HAVE_GETADDRINFO 1)
        set(CFW_HAVE_TCP_NODELAY 1)
    endif()

    foreach(_cfw_feature
            EPOLL
            KQUEUE
            POLL
            GETRANDOM
            ARC4RANDOM_BUF
            ACCEPT4
            GETADDRINFO
            SOCK_NONBLOCK
            SOCK_CLOEXEC
            MSG_NOSIGNAL
            MSG_DONTWAIT
            SO_NOSIGPIPE
            SO_REUSEPORT
            TCP_NODELAY
            O_NOFOLLOW
            O_CLOEXEC
            OPENAT
            CLOCK_MONOTONIC
            CLOCK_REALTIME
            SC_PAGESIZE
            SC_PAGE_SIZE)
        if(CFW_CHECK_HAVE_${_cfw_feature})
            set(CFW_HAVE_${_cfw_feature} 1)
        endif()
    endforeach()

    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/include/cfw")
    configure_file(
        "${CFW_PLATFORM_CONFIG_DIR}/cfw_platform_config.h.in"
        "${CMAKE_CURRENT_BINARY_DIR}/include/cfw/cfw_platform_internal.h"
        @ONLY
    )

    unset(_cfw_platform)
    unset(_cfw_feature)
    unset(CFW_PLATFORM_CHECK_DEFINITIONS)
endmacro()
