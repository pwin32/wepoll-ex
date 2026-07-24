/*
 * Seeded public-API stress for Windows registration and wait lifecycles.
 *
 * The default run is deliberately bounded for pull-request validation.  Use
 * --long, or the WEPOLL_EX_STRESS_* environment variables, for soak runs.
 * Every failure prints a seed and operation index that can be replayed.
 */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif

#include "wepoll_ex.h"

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32

#define STRESS_TOKEN_MAGIC UINT64_C(0x5750000000000000)
#define STRESS_TOKEN_MASK  UINT64_C(0xffff000000000000)
#define STRESS_MAX_SLOTS   1024U

typedef struct stress_socket {
    SOCKET receiver;
    struct sockaddr_in address;
    int registered;
    int oneshot;
    int oneshot_fired;
    uint64_t token;
} stress_socket_t;

typedef struct stress_context {
    CRITICAL_SECTION lock;
    stress_socket_t *sockets;
    unsigned socket_count;
    SOCKET sender;
    int epfd;
    HANDLE waiter;
    volatile LONG stop;
    volatile LONG closing;
    volatile LONG failure;
    volatile LONG waiter_errno;
    volatile LONG event_count;
    unsigned registered_count;
} stress_context_t;

typedef struct stress_config {
    uint64_t seed;
    uint64_t operations;
    uint64_t seconds;
    unsigned sockets;
} stress_config_t;

static uint64_t rng_next(uint64_t *state)
{
    uint64_t value = *state;

    value ^= value >> 12;
    value ^= value << 25;
    value ^= value >> 27;
    *state = value;
    return value * UINT64_C(2685821657736338717);
}

static uint64_t make_token(uint64_t sequence)
{
    return STRESS_TOKEN_MAGIC |
           ((sequence + UINT64_C(1)) & ~STRESS_TOKEN_MASK);
}

static int parse_u64(const char *text, uint64_t minimum, uint64_t maximum,
                     uint64_t *value_out)
{
    char *end = NULL;
    unsigned long long value;

    if (text == NULL || *text == '\0') {
        return -1;
    }
    errno = 0;
    value = strtoull(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0' ||
        value < minimum || value > maximum) {
        return -1;
    }
    *value_out = (uint64_t)value;
    return 0;
}

static int apply_environment(stress_config_t *config)
{
    const char *seed = getenv("WEPOLL_EX_STRESS_SEED");
    const char *operations = getenv("WEPOLL_EX_STRESS_OPERATIONS");
    const char *seconds = getenv("WEPOLL_EX_STRESS_SECONDS");
    const char *sockets = getenv("WEPOLL_EX_STRESS_SOCKETS");
    uint64_t parsed;

    if (seed != NULL && parse_u64(seed, 1, UINT64_MAX, &config->seed) != 0)
        return -1;
    if (operations != NULL &&
        parse_u64(operations, 1, UINT64_C(100000000),
                  &config->operations) != 0)
        return -1;
    if (seconds != NULL &&
        parse_u64(seconds, 1, UINT64_C(86400), &config->seconds) != 0)
        return -1;
    if (sockets != NULL) {
        if (parse_u64(sockets, 1, STRESS_MAX_SLOTS, &parsed) != 0)
            return -1;
        config->sockets = (unsigned)parsed;
    }
    return 0;
}

static void print_usage(const char *program)
{
    fprintf(stderr,
            "usage: %s [--long] [--seed N] [--operations N] "
            "[--seconds N] [--sockets N]\n",
            program);
}

static int parse_arguments(int argc, char **argv, stress_config_t *config)
{
    int long_mode = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--long") == 0) {
            long_mode = 1;
        }
    }
    if (long_mode) {
        config->operations = UINT64_C(250000);
        config->seconds = UINT64_C(120);
        config->sockets = 128;
    }
    if (apply_environment(config) != 0) {
        fprintf(stderr, "invalid WEPOLL_EX_STRESS_* environment value\n");
        return -1;
    }

    for (int i = 1; i < argc; i++) {
        const char *name = argv[i];
        const char *value;
        uint64_t parsed;

        if (strcmp(name, "--long") == 0)
            continue;
        if (strcmp(name, "--help") == 0) {
            print_usage(argv[0]);
            return 1;
        }
        if (i + 1 >= argc) {
            print_usage(argv[0]);
            return -1;
        }
        value = argv[++i];
        if (strcmp(name, "--seed") == 0) {
            if (parse_u64(value, 1, UINT64_MAX, &config->seed) != 0)
                return -1;
        } else if (strcmp(name, "--operations") == 0) {
            if (parse_u64(value, 1, UINT64_C(100000000),
                          &config->operations) != 0)
                return -1;
        } else if (strcmp(name, "--seconds") == 0) {
            if (parse_u64(value, 1, UINT64_C(86400),
                          &config->seconds) != 0)
                return -1;
        } else if (strcmp(name, "--sockets") == 0) {
            if (parse_u64(value, 1, STRESS_MAX_SLOTS, &parsed) != 0)
                return -1;
            config->sockets = (unsigned)parsed;
        } else {
            print_usage(argv[0]);
            return -1;
        }
    }
    return 0;
}

static void stress_socket_init(stress_socket_t *socket_state)
{
    memset(socket_state, 0, sizeof(*socket_state));
    socket_state->receiver = INVALID_SOCKET;
}

static int stress_socket_open(stress_socket_t *socket_state)
{
    int address_length = (int)sizeof(socket_state->address);
    u_long nonblocking = 1;

    stress_socket_init(socket_state);
    socket_state->receiver = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_state->receiver == INVALID_SOCKET)
        return -1;

    memset(&socket_state->address, 0, sizeof(socket_state->address));
    socket_state->address.sin_family = AF_INET;
    socket_state->address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socket_state->address.sin_port = htons(0);
    if (bind(socket_state->receiver,
             (const struct sockaddr *)&socket_state->address,
             (int)sizeof(socket_state->address)) == SOCKET_ERROR ||
        getsockname(socket_state->receiver,
                    (struct sockaddr *)&socket_state->address,
                    &address_length) == SOCKET_ERROR ||
        ioctlsocket(socket_state->receiver, FIONBIO,
                    &nonblocking) == SOCKET_ERROR) {
        closesocket(socket_state->receiver);
        stress_socket_init(socket_state);
        return -1;
    }
    return 0;
}

static void stress_socket_close(stress_socket_t *socket_state)
{
    if (socket_state->receiver != INVALID_SOCKET)
        (void)closesocket(socket_state->receiver);
    stress_socket_init(socket_state);
}

static int drain_receiver(SOCKET receiver)
{
    char buffer[256];

    for (;;) {
        int result = recv(receiver, buffer, (int)sizeof(buffer), 0);
        if (result >= 0)
            continue;
        if (WSAGetLastError() == WSAEWOULDBLOCK)
            return 0;
        return -1;
    }
}

static stress_socket_t *find_token_locked(stress_context_t *context,
                                          uint64_t token)
{
    for (unsigned i = 0; i < context->socket_count; i++) {
        if (context->sockets[i].registered &&
            context->sockets[i].token == token)
            return &context->sockets[i];
    }
    return NULL;
}

static void record_waiter_failure(stress_context_t *context, int error)
{
    InterlockedCompareExchange(&context->waiter_errno, (LONG)error, 0);
    InterlockedExchange(&context->failure, 1);
}

static DWORD WINAPI wait_thread_proc(void *argument)
{
    stress_context_t *context = (stress_context_t *)argument;
    epoll_event_ex events[64];

    for (;;) {
        int count;

        if (InterlockedCompareExchange(&context->stop, 0, 0) != 0 ||
            InterlockedCompareExchange(&context->closing, 0, 0) != 0)
            break;

        memset(events, 0, sizeof(events));
        count = epoll_wait_ex(context->epfd, events, 64, 20);
        if (count < 0) {
            int wait_error = errno;
            if ((InterlockedCompareExchange(&context->stop, 0, 0) != 0 ||
                 InterlockedCompareExchange(&context->closing, 0, 0) != 0) &&
                wait_error == EBADF)
                break;
            record_waiter_failure(context, wait_error);
            break;
        }

        for (int i = 0; i < count; i++) {
            stress_socket_t *socket_state;
            uint64_t token = events[i].data.u64;

            if ((token & STRESS_TOKEN_MASK) != STRESS_TOKEN_MAGIC ||
                events[i].user_ctx != (void *)(uintptr_t)token ||
                (events[i].events & (EPOLLIN | EPOLLERR | EPOLLHUP)) == 0) {
                record_waiter_failure(context, EPROTO);
                return 1;
            }

            EnterCriticalSection(&context->lock);
            socket_state = find_token_locked(context, token);
            if (socket_state != NULL) {
                if (drain_receiver(socket_state->receiver) != 0) {
                    LeaveCriticalSection(&context->lock);
                    record_waiter_failure(context, EIO);
                    return 1;
                }
                if (socket_state->oneshot)
                    socket_state->oneshot_fired = 1;
            }
            LeaveCriticalSection(&context->lock);
            InterlockedIncrement(&context->event_count);
        }
    }
    return 0;
}

static int start_waiter(stress_context_t *context)
{
    InterlockedExchange(&context->closing, 0);
    context->waiter = CreateThread(NULL, 0, wait_thread_proc,
                                   context, 0, NULL);
    return context->waiter == NULL ? -1 : 0;
}

static int join_waiter(stress_context_t *context, DWORD timeout_ms)
{
    DWORD result;

    if (context->waiter == NULL)
        return 0;
    result = WaitForSingleObject(context->waiter, timeout_ms);
    if (result != WAIT_OBJECT_0)
        return -1;
    CloseHandle(context->waiter);
    context->waiter = NULL;
    return 0;
}

static int rotate_epoll(stress_context_t *context)
{
    int close_result;

    InterlockedExchange(&context->closing, 1);
    close_result = wepoll_close(context->epfd);
    context->epfd = -1;
    if (join_waiter(context, 5000) != 0) {
        fprintf(stderr, "waiter did not exit within 5 seconds\n");
        ExitProcess(2);
    }
    if (close_result != 0)
        return -1;

    EnterCriticalSection(&context->lock);
    for (unsigned i = 0; i < context->socket_count; i++) {
        context->sockets[i].registered = 0;
        context->sockets[i].oneshot = 0;
        context->sockets[i].oneshot_fired = 0;
        context->sockets[i].token = 0;
    }
    context->registered_count = 0;
    LeaveCriticalSection(&context->lock);

    context->epfd = epoll_create_ex((int)context->socket_count, 0);
    if (context->epfd < 0)
        return -1;
    return start_waiter(context);
}

static int control_add(stress_context_t *context,
                       stress_socket_t *socket_state,
                       uint64_t token, int oneshot)
{
    struct epoll_event event;

    if (socket_state->registered)
        return 0;
    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN | (oneshot ? EPOLLONESHOT : 0);
    event.data.u64 = token;
    if (epoll_ctl_ctx(context->epfd, EPOLL_CTL_ADD,
                      socket_state->receiver, &event,
                      (void *)(uintptr_t)token) != 0)
        return -1;
    socket_state->registered = 1;
    socket_state->oneshot = oneshot;
    socket_state->oneshot_fired = 0;
    socket_state->token = token;
    context->registered_count++;
    return 0;
}

static int control_mod(stress_context_t *context,
                       stress_socket_t *socket_state,
                       uint64_t token, int oneshot)
{
    struct epoll_event event;

    if (!socket_state->registered)
        return 0;
    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN | (oneshot ? EPOLLONESHOT : 0);
    event.data.u64 = token;
    if (epoll_ctl_ctx(context->epfd, EPOLL_CTL_MOD,
                      socket_state->receiver, &event,
                      (void *)(uintptr_t)token) != 0)
        return -1;
    socket_state->oneshot = oneshot;
    socket_state->oneshot_fired = 0;
    socket_state->token = token;
    return 0;
}

static int control_del(stress_context_t *context,
                       stress_socket_t *socket_state)
{
    if (!socket_state->registered)
        return 0;
    if (epoll_ctl(context->epfd, EPOLL_CTL_DEL,
                  socket_state->receiver, NULL) != 0)
        return -1;
    socket_state->registered = 0;
    socket_state->oneshot = 0;
    socket_state->oneshot_fired = 0;
    socket_state->token = 0;
    context->registered_count--;
    return 0;
}

static int control_rearm(stress_context_t *context,
                         stress_socket_t *socket_state)
{
    if (!socket_state->registered || !socket_state->oneshot)
        return 0;
    if (epoll_rearm(context->epfd, socket_state->receiver) != 0)
        return -1;
    socket_state->oneshot_fired = 0;
    return 0;
}

static int send_datagram(stress_context_t *context,
                         stress_socket_t *socket_state, char byte,
                         int *backpressure)
{
    int result = sendto(context->sender, &byte, 1, 0,
                        (const struct sockaddr *)&socket_state->address,
                        (int)sizeof(socket_state->address));

    if (result == 1)
        return 0;
    if (result == SOCKET_ERROR &&
        (WSAGetLastError() == WSAEWOULDBLOCK ||
         WSAGetLastError() == WSAENOBUFS)) {
        (*backpressure)++;
        return 0;
    }
    return -1;
}

static int validate_count(stress_context_t *context)
{
    int actual = epoll_fd_count(context->epfd);

    return actual >= 0 && (unsigned)actual == context->registered_count
        ? 0 : -1;
}

static int initialize_context(stress_context_t *context,
                              unsigned socket_count)
{
    u_long nonblocking = 1;

    memset(context, 0, sizeof(*context));
    context->epfd = -1;
    context->sender = INVALID_SOCKET;
    context->socket_count = socket_count;
    InitializeCriticalSection(&context->lock);
    context->sockets = (stress_socket_t *)calloc(
        socket_count, sizeof(*context->sockets));
    if (context->sockets == NULL)
        return -1;
    for (unsigned i = 0; i < socket_count; i++)
        stress_socket_init(&context->sockets[i]);

    context->sender = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (context->sender == INVALID_SOCKET ||
        ioctlsocket(context->sender, FIONBIO, &nonblocking) == SOCKET_ERROR)
        return -1;
    for (unsigned i = 0; i < socket_count; i++) {
        if (stress_socket_open(&context->sockets[i]) != 0)
            return -1;
    }
    context->epfd = epoll_create_ex((int)socket_count, 0);
    if (context->epfd < 0)
        return -1;
    return start_waiter(context);
}

static int shutdown_context(stress_context_t *context)
{
    int result = 0;

    InterlockedExchange(&context->stop, 1);
    InterlockedExchange(&context->closing, 1);
    if (context->epfd >= 0) {
        if (wepoll_close(context->epfd) != 0)
            result = -1;
        context->epfd = -1;
    }
    if (join_waiter(context, 5000) != 0) {
        fprintf(stderr, "waiter did not stop within 5 seconds\n");
        ExitProcess(2);
    }
    if (context->sockets != NULL) {
        for (unsigned i = 0; i < context->socket_count; i++)
            stress_socket_close(&context->sockets[i]);
        free(context->sockets);
        context->sockets = NULL;
    }
    if (context->sender != INVALID_SOCKET) {
        (void)closesocket(context->sender);
        context->sender = INVALID_SOCKET;
    }
    DeleteCriticalSection(&context->lock);
    return result;
}

int main(int argc, char **argv)
{
    stress_config_t config = {
        UINT64_C(0x5eedc0de12345678),
        UINT64_C(3000),
        UINT64_C(10),
        32
    };
    WSADATA winsock;
    stress_context_t context;
    uint64_t rng_state;
    uint64_t token_sequence = 0;
    uint64_t completed = 0;
    ULONGLONG start_ms;
    int backpressure = 0;
    int sends = 0;
    int rotations = 0;
    int result = 1;
    int parse_result = parse_arguments(argc, argv, &config);

    if (parse_result != 0)
        return parse_result > 0 ? 0 : 2;
    if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        return 1;
    }
    if (initialize_context(&context, config.sockets) != 0) {
        fprintf(stderr, "stress setup failed: errno=%d WSA=%d\n",
                errno, WSAGetLastError());
        (void)shutdown_context(&context);
        WSACleanup();
        return 1;
    }

    rng_state = config.seed;
    start_ms = GetTickCount64();
    for (uint64_t operation = 0; operation < config.operations; operation++) {
        uint64_t random = rng_next(&rng_state);
        unsigned index = (unsigned)(random % config.sockets);
        unsigned action = (unsigned)((random >> 32) % 100U);
        stress_socket_t *socket_state = &context.sockets[index];
        int operation_result = 0;

        if (GetTickCount64() - start_ms >= config.seconds * UINT64_C(1000))
            break;
        if (InterlockedCompareExchange(&context.failure, 0, 0) != 0) {
            errno = (int)InterlockedCompareExchange(&context.waiter_errno,
                                                     0, 0);
            fprintf(stderr, "waiter failure");
            goto failure;
        }

        if (action >= 98) {
            if (rotate_epoll(&context) != 0) {
                fprintf(stderr, "epoll rotation failed");
                goto failure;
            }
            rotations++;
        } else {
            EnterCriticalSection(&context.lock);
            if (action < 24) {
                if (!socket_state->registered &&
                    drain_receiver(socket_state->receiver) != 0) {
                    operation_result = -1;
                } else if (send_datagram(&context, socket_state,
                                         (char)(operation & 0x7f),
                                         &backpressure) != 0) {
                    operation_result = -1;
                } else {
                    sends++;
                }
            } else if (action < 43) {
                operation_result = control_add(
                    &context, socket_state, make_token(token_sequence++),
                    (int)((random >> 8) & 1U));
            } else if (action < 59) {
                operation_result = control_mod(
                    &context, socket_state, make_token(token_sequence++),
                    (int)((random >> 9) & 1U));
            } else if (action < 72) {
                operation_result = control_del(&context, socket_state);
            } else if (action < 84) {
                operation_result = control_rearm(&context, socket_state);
            } else if (action < 94) {
                if (control_del(&context, socket_state) != 0) {
                    operation_result = -1;
                } else {
                    stress_socket_close(socket_state);
                    operation_result = stress_socket_open(socket_state);
                }
            } else if (validate_count(&context) != 0) {
                operation_result = -1;
            }
            LeaveCriticalSection(&context.lock);
            if (operation_result != 0) {
                fprintf(stderr, "control action %u failed", action);
                goto failure;
            }
        }

        if ((operation & UINT64_C(31)) == 0) {
            EnterCriticalSection(&context.lock);
            operation_result = validate_count(&context);
            LeaveCriticalSection(&context.lock);
            if (operation_result != 0) {
                fprintf(stderr, "registration count mismatch");
                goto failure;
            }
        }
        completed = operation + 1;
        if ((operation & UINT64_C(63)) == 0)
            SwitchToThread();
        continue;

failure:
        fprintf(stderr,
                ": seed=0x%016" PRIx64 " operation=%" PRIu64
                " errno=%d WSA=%d\n",
                config.seed, operation, errno, WSAGetLastError());
        fprintf(stderr,
                "replay: %s --seed 0x%016" PRIx64
                " --operations %" PRIu64 " --seconds %" PRIu64
                " --sockets %u\n",
                argv[0], config.seed, operation + 1,
                config.seconds, config.sockets);
        goto cleanup;
    }

    if (InterlockedCompareExchange(&context.failure, 0, 0) != 0) {
        errno = (int)InterlockedCompareExchange(&context.waiter_errno, 0, 0);
        fprintf(stderr, "waiter failure after final operation: errno=%d\n",
                errno);
        goto cleanup;
    }
    result = 0;

cleanup:
    if (shutdown_context(&context) != 0) {
        fprintf(stderr, "epoll shutdown failed: errno=%d WSA=%d\n",
                errno, WSAGetLastError());
        result = 1;
    }
    printf("windows_stress seed=0x%016" PRIx64
           " operations=%" PRIu64 " sockets=%u events=%ld sends=%d "
           "backpressure=%d rotations=%d elapsed_ms=%llu result=%s\n",
           config.seed, completed, config.sockets,
           (long)InterlockedCompareExchange(&context.event_count, 0, 0),
           sends, backpressure, rotations,
           (unsigned long long)(GetTickCount64() - start_ms),
           result == 0 ? "pass" : "fail");
    WSACleanup();
    return result;
}

#else

int main(void)
{
    fprintf(stderr, "test_windows_stress is only supported on Windows\n");
    return 77;
}

#endif
