/*
 * test_windows_backpressure.c -- EPOLLOUT backpressure regression.
 *
 * A writable notification must not turn into a busy loop after a TCP
 * send-buffer and the peer's receive window are full.  Once the peer drains
 * data, the same registration must become writable again without a MOD.
 */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif

#include "wepoll_ex.h"

#ifdef _WIN32

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
    TEST_SOCKET_BUFFER = 4096,
    TEST_SEND_CHUNK = 16384,
    TEST_SETTLE_ROUNDS = 8,
    TEST_SETTLE_MAX_ROUNDS = 64,
    TEST_SETTLE_DELAY_MS = 10,
    TEST_ZERO_PROBES = 32,
    TEST_FULL_WAIT_MS = 150,
    TEST_PROGRESS_WAIT_MS = 2000,
    TEST_MAX_QUEUED_BYTES = 64 * 1024 * 1024,
    TEST_MAX_RESTABILIZE = 4
};

typedef struct tcp_pair {
    SOCKET listener;
    SOCKET client;
    SOCKET server;
} tcp_pair_t;

static void tcp_pair_init(tcp_pair_t *pair)
{
    pair->listener = INVALID_SOCKET;
    pair->client = INVALID_SOCKET;
    pair->server = INVALID_SOCKET;
}

static void tcp_pair_close(tcp_pair_t *pair)
{
    if (pair->server != INVALID_SOCKET) {
        closesocket(pair->server);
        pair->server = INVALID_SOCKET;
    }
    if (pair->client != INVALID_SOCKET) {
        closesocket(pair->client);
        pair->client = INVALID_SOCKET;
    }
    if (pair->listener != INVALID_SOCKET) {
        closesocket(pair->listener);
        pair->listener = INVALID_SOCKET;
    }
}

static int set_socket_buffer(SOCKET socket_fd, int option, int value)
{
    return setsockopt(socket_fd, SOL_SOCKET, option, (const char *)&value,
                      (int)sizeof(value)) == SOCKET_ERROR ? -1 : 0;
}

static int set_nonblocking(SOCKET socket_fd)
{
    u_long enabled = 1;

    return ioctlsocket(socket_fd, FIONBIO, &enabled) == SOCKET_ERROR ? -1 : 0;
}

static int make_tcp_pair(tcp_pair_t *pair)
{
    struct sockaddr_in address;
    int address_length = (int)sizeof(address);

    tcp_pair_init(pair);
    pair->listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (pair->listener == INVALID_SOCKET ||
        set_socket_buffer(pair->listener, SO_RCVBUF,
                          TEST_SOCKET_BUFFER) != 0) {
        tcp_pair_close(pair);
        return -1;
    }

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(0);
    if (bind(pair->listener, (const struct sockaddr *)&address,
             (int)sizeof(address)) == SOCKET_ERROR ||
        listen(pair->listener, 1) == SOCKET_ERROR ||
        getsockname(pair->listener, (struct sockaddr *)&address,
                    &address_length) == SOCKET_ERROR) {
        tcp_pair_close(pair);
        return -1;
    }

    pair->client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (pair->client == INVALID_SOCKET ||
        set_socket_buffer(pair->client, SO_SNDBUF,
                          TEST_SOCKET_BUFFER) != 0 ||
        connect(pair->client, (const struct sockaddr *)&address,
                (int)sizeof(address)) == SOCKET_ERROR) {
        tcp_pair_close(pair);
        return -1;
    }

    pair->server = accept(pair->listener, NULL, NULL);
    if (pair->server == INVALID_SOCKET ||
        set_socket_buffer(pair->server, SO_RCVBUF,
                          TEST_SOCKET_BUFFER) != 0 ||
        set_socket_buffer(pair->client, SO_SNDBUF,
                          TEST_SOCKET_BUFFER) != 0 ||
        set_nonblocking(pair->client) != 0 ||
        set_nonblocking(pair->server) != 0) {
        tcp_pair_close(pair);
        return -1;
    }

    closesocket(pair->listener);
    pair->listener = INVALID_SOCKET;
    return 0;
}

/* Fill the sender until Winsock reports that it would block. */
static int fill_until_blocked(SOCKET client, size_t *queued_bytes)
{
    char buffer[TEST_SEND_CHUNK];

    memset(buffer, 'w', sizeof(buffer));
    for (;;) {
        int sent = send(client, buffer, (int)sizeof(buffer), 0);

        if (sent > 0) {
            *queued_bytes += (size_t)sent;
            if (*queued_bytes > TEST_MAX_QUEUED_BYTES) {
                fprintf(stderr,
                        "backpressure: sender accepted more than %llu bytes\n",
                        (unsigned long long)TEST_MAX_QUEUED_BYTES);
                return -1;
            }
            continue;
        }
        if (sent == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) {
            return 0;
        }
        fprintf(stderr, "backpressure: send failed, WSA=%d\n",
                WSAGetLastError());
        return -1;
    }
}

/* Let loopback ACKs settle, then require repeated would-block probes. */
static int settle_full(SOCKET client, size_t *queued_bytes)
{
    int stable_rounds = 0;

    for (int round = 0; round < TEST_SETTLE_MAX_ROUNDS; round++) {
        size_t before = *queued_bytes;

        if (fill_until_blocked(client, queued_bytes) != 0) {
            return -1;
        }
        if (*queued_bytes == before) {
            stable_rounds++;
        } else {
            stable_rounds = 0;
        }
        if (stable_rounds == TEST_SETTLE_ROUNDS) {
            return 0;
        }
        Sleep(TEST_SETTLE_DELAY_MS);
    }

    fprintf(stderr, "backpressure: send window did not settle full\n");
    return -1;
}

static int drain_peer(SOCKET server, size_t *drained_bytes)
{
    char buffer[TEST_SEND_CHUNK];

    for (;;) {
        int received = recv(server, buffer, (int)sizeof(buffer), 0);

        if (received > 0) {
            *drained_bytes += (size_t)received;
            continue;
        }
        if (received == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) {
            return 0;
        }
        if (received == 0) {
            fprintf(stderr, "backpressure: peer closed unexpectedly\n");
        } else {
            fprintf(stderr, "backpressure: recv failed, WSA=%d\n",
                    WSAGetLastError());
        }
        return -1;
    }
}

static int check_not_writable_while_full(int epfd, SOCKET client,
                                         size_t *queued_bytes)
{
    struct epoll_event output;
    int remained_full = 0;

    /* An event which also makes a one-byte send succeed is legitimate: the
     * loopback stack may have just opened a little room.  Re-fill and retry
     * a bounded number of times before declaring a false writable event. */
    for (int attempt = 0; attempt < TEST_MAX_RESTABILIZE; attempt++) {
        int restabilize = 0;

        if (settle_full(client, queued_bytes) != 0) {
            return -1;
        }
        for (int probe = 0; probe < TEST_ZERO_PROBES; probe++) {
            int result = epoll_wait(epfd, &output, 1, 0);

            if (result < 0) {
                fprintf(stderr,
                        "backpressure: zero-timeout wait failed, errno=%d\n",
                        errno);
                return -1;
            }
            if (result == 0) {
                continue;
            }
            if ((output.events & (EPOLLERR | EPOLLHUP)) != 0 ||
                (output.events & EPOLLOUT) == 0) {
                fprintf(stderr,
                        "backpressure: unexpected full-window event 0x%08lx\n",
                        (unsigned long)output.events);
                return -1;
            }

            {
                char byte = 'p';
                int sent = send(client, &byte, 1, 0);

                if (sent == 1) {
                    *queued_bytes += 1;
                    restabilize = 1;
                    break;
                }
                if (sent == SOCKET_ERROR &&
                    WSAGetLastError() == WSAEWOULDBLOCK) {
                    fprintf(stderr,
                            "backpressure: EPOLLOUT while send would block\n");
                } else {
                    fprintf(stderr,
                            "backpressure: EPOLLOUT probe send failed, "
                            "WSA=%d\n",
                            WSAGetLastError());
                }
                return -1;
            }
        }
        if (!restabilize) {
            remained_full = 1;
            break;
        }
    }

    if (!remained_full) {
        fprintf(stderr,
                "backpressure: send window did not remain full\n");
        return -1;
    }

    /* A finite wait must actually consume its timeout while the peer is
     * still not reading; this catches an immediate-return busy loop too. */
    {
        struct epoll_event output = {0};
        uint64_t started = GetTickCount64();
        int result = epoll_wait(epfd, &output, 1, TEST_FULL_WAIT_MS);
        uint64_t elapsed = GetTickCount64() - started;

        if (result < 0) {
            fprintf(stderr,
                    "backpressure: short wait failed, errno=%d\n", errno);
            return -1;
        }
        if (result != 0 || elapsed + 25 < TEST_FULL_WAIT_MS ||
            elapsed > (uint64_t)TEST_FULL_WAIT_MS + 1000) {
            fprintf(stderr,
                    "backpressure: full-window wait result=%d elapsed=%llu "
                    "ms\n",
                    result, (unsigned long long)elapsed);
            return -1;
        }
    }
    return 0;
}

static int wait_for_writable_after_drain(int epfd, SOCKET client, SOCKET server,
                                         size_t *drained_bytes)
{
    struct epoll_event output;
    uint64_t deadline = GetTickCount64() + TEST_PROGRESS_WAIT_MS;

    while (GetTickCount64() < deadline) {
        if (drain_peer(server, drained_bytes) != 0) {
            return -1;
        }
        if (*drained_bytes == 0) {
            /* Give the stack a chance to deliver the queued bytes before the
             * next drain attempt. */
            Sleep(1);
        }

        {
            int result = epoll_wait(epfd, &output, 1, 50);

            if (result < 0) {
                fprintf(stderr,
                        "backpressure: progress wait failed, errno=%d\n",
                        errno);
                return -1;
            }
            if (result == 0) {
                continue;
            }
            if ((output.events & (EPOLLERR | EPOLLHUP)) != 0 ||
                (output.events & EPOLLOUT) == 0) {
                fprintf(stderr,
                        "backpressure: unexpected progress event 0x%08lx\n",
                        (unsigned long)output.events);
                return -1;
            }

            {
                char byte = 'r';
                int sent = send(client, &byte, 1, 0);

                if (sent == 1) {
                    return 0;
                }
                if (sent == SOCKET_ERROR &&
                    WSAGetLastError() == WSAEWOULDBLOCK) {
                    /* AFD readiness raced with the stack's window update;
                     * keep draining and wait for the next level indication. */
                    continue;
                }
                fprintf(stderr,
                        "backpressure: writable probe send failed, WSA=%d\n",
                        WSAGetLastError());
                return -1;
            }
        }
    }

    fprintf(stderr,
            "backpressure: no writable progress within %d ms (drained=%llu)\n",
            TEST_PROGRESS_WAIT_MS, (unsigned long long)*drained_bytes);
    return -1;
}

static int test_backpressure(void)
{
    static const uint64_t event_data = UINT64_C(0xbadc0ffee0ddf00d);
    tcp_pair_t pair;
    struct epoll_event event;
    size_t queued_bytes = 0;
    size_t drained_bytes = 0;
    int epfd = -1;
    int result = -1;

    if (make_tcp_pair(&pair) != 0) {
        fprintf(stderr, "backpressure: make_tcp_pair failed, WSA=%d\n",
                WSAGetLastError());
        return -1;
    }

    epfd = epoll_create1(0);
    memset(&event, 0, sizeof(event));
    event.events = EPOLLOUT;
    event.data.u64 = event_data;
    if (epfd < 0 ||
        epoll_ctl(epfd, EPOLL_CTL_ADD, pair.client, &event) != 0) {
        fprintf(stderr, "backpressure: registration failed, errno=%d WSA=%d\n",
                errno, WSAGetLastError());
        goto cleanup;
    }

    /* Consume the initial level-triggered writable notification before
     * filling the socket.  The next wait must therefore arm a fresh AFD
     * poll against the now-full send window. */
    {
        struct epoll_event output;
        int count = epoll_wait(epfd, &output, 1, 1000);

        if (count != 1 || (output.events & EPOLLOUT) == 0 ||
            output.data.u64 != event_data) {
            fprintf(stderr,
                    "backpressure: initial EPOLLOUT missing (count=%d "
                    "events=0x%08lx)\n",
                    count, (unsigned long)output.events);
            goto cleanup;
        }
    }

    if (check_not_writable_while_full(epfd, pair.client,
                                      &queued_bytes) != 0) {
        goto cleanup;
    }
    if (wait_for_writable_after_drain(epfd, pair.client, pair.server,
                                      &drained_bytes) != 0 ||
        drained_bytes == 0) {
        goto cleanup;
    }

    printf("backpressure: queued=%llu drained=%llu OK\n",
           (unsigned long long)queued_bytes,
           (unsigned long long)drained_bytes);
    result = 0;

cleanup:
    if (epfd >= 0 && wepoll_close(epfd) != 0) {
        fprintf(stderr, "backpressure: wepoll_close failed, errno=%d\n",
                errno);
        result = -1;
    }
    tcp_pair_close(&pair);
    return result;
}

int main(void)
{
    WSADATA wsa_data;
    int result;

    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        return 2;
    }
    result = test_backpressure();
    WSACleanup();
    return result == 0 ? 0 : 1;
}

#else

int main(void)
{
    return 0;
}

#endif
