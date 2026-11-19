/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

/* ESP-IDF WS writes its header and PCM separately. Send both promptly. */
static inline int voice_socket_nodelay(int fd)
{
    int enabled = 1;
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled)) != 0)
        return -1;
    enabled = 0;
    socklen_t size = sizeof(enabled);
    if (getsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled, &size) != 0)
        return -1;
    if (size != sizeof(enabled) || enabled != 1) {
        errno = EIO;
        return -1;
    }
    return 0;
}
