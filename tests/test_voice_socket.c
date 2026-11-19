/* SPDX-License-Identifier: Apache-2.0 */
#include "voice_socket.h"
#include <assert.h>
#include <unistd.h>

int main(void)
{
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    int client = socket(AF_INET, SOCK_STREAM, 0);
    assert(listener >= 0 && client >= 0);
    struct sockaddr_in address = {
        .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    assert(bind(listener, (struct sockaddr *)&address, sizeof(address)) == 0);
    assert(listen(listener, 1) == 0);
    socklen_t size = sizeof(address);
    assert(getsockname(listener, (struct sockaddr *)&address, &size) == 0);
    assert(connect(client, (struct sockaddr *)&address, size) == 0);
    int accepted = accept(listener, NULL, NULL);
    assert(accepted >= 0);
    int enabled = 0;
    assert(setsockopt(accepted, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled)) == 0);
    assert(voice_socket_nodelay(accepted) == 0);
    size = sizeof(enabled);
    assert(getsockopt(accepted, IPPROTO_TCP, TCP_NODELAY, &enabled, &size) == 0);
    assert(enabled == 1);
    assert(voice_socket_nodelay(accepted) == 0);
    close(accepted);
    close(client);
    close(listener);
    assert(voice_socket_nodelay(-1) == -1);
    return 0;
}
