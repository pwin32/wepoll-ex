/*
 * Windows qualification benchmark for registration, ready-batch,
 * EPOLLONESHOT rearm, and control-churn costs.
 *
 * Output is CSV so qualification runs can be archived and compared.  The
 * default caps setup at 1,000 sockets; --production enables the 10,000 and
 * 50,000 registration points and increases the sample count.
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

#define BENCH_MAX_SOCKETS 50000U
#define BENCH_MAX_BATCH   512U

typedef struct receiver_state {
    SOCKET fd;
    struct sockaddr_in address;
} receiver_state_t;

typedef struct bench_context {
    receiver_state_t *receivers;
    unsigned receiver_count;
    SOCKET sender;
    LARGE_INTEGER frequency;
    int timeout_ms;
} bench_context_t;

typedef struct bench_config {
    unsigned max_sockets;
    unsigned iterations;
    int timeout_ms;
} bench_config_t;

static int compare_u64(const void *left, const void *right)
{
    uint64_t a = *(const uint64_t *)left;
    uint64_t b = *(const uint64_t *)right;

    return a < b ? -1 : a > b ? 1 : 0;
}

static uint64_t timer_now(void)
{
    LARGE_INTEGER value;

    QueryPerformanceCounter(&value);
    return (uint64_t)value.QuadPart;
}

static uint64_t elapsed_ns(const bench_context_t *context,
                           uint64_t start, uint64_t end)
{
    double ticks = (double)(end - start);
    double ns = ticks * 1000000000.0 /
                (double)context->frequency.QuadPart;

    return ns < 1.0 ? 1 : (uint64_t)ns;
}

static uint64_t percentile(const uint64_t *sorted, size_t count,
                           unsigned percentile_value)
{
    size_t index = ((count - 1) * percentile_value + 99) / 100;

    return sorted[index];
}

static void report_samples(const char *benchmark, const char *parameter,
                           uint64_t *samples, size_t sample_count,
                           unsigned logical_operations)
{
    long double total = 0;
    double operations_per_second;
    uint64_t p50;
    uint64_t p95;
    uint64_t p99;

    qsort(samples, sample_count, sizeof(*samples), compare_u64);
    for (size_t i = 0; i < sample_count; i++)
        total += (long double)samples[i];
    p50 = percentile(samples, sample_count, 50);
    p95 = percentile(samples, sample_count, 95);
    p99 = percentile(samples, sample_count, 99);
    operations_per_second =
        (double)logical_operations * 1000000000.0 /
        (double)(total / (long double)sample_count);

    printf("%s,%s,%zu,%" PRIu64 ",%" PRIu64 ",%" PRIu64
           ",%" PRIu64 ",%.0f\n",
           benchmark, parameter, sample_count, p50, p95, p99,
           samples[sample_count - 1], operations_per_second);
}

static int parse_unsigned(const char *text, unsigned minimum,
                          unsigned maximum, unsigned *value_out)
{
    char *end = NULL;
    unsigned long value;

    if (text == NULL || *text == '\0')
        return -1;
    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' ||
        value < minimum || value > maximum)
        return -1;
    *value_out = (unsigned)value;
    return 0;
}

static int apply_environment(bench_config_t *config)
{
    const char *sockets = getenv("WEPOLL_EX_BENCH_SOCKETS");
    const char *iterations = getenv("WEPOLL_EX_BENCH_ITERATIONS");
    const char *timeout = getenv("WEPOLL_EX_BENCH_TIMEOUT_MS");
    unsigned parsed;

    if (sockets != NULL &&
        parse_unsigned(sockets, 1, BENCH_MAX_SOCKETS,
                       &config->max_sockets) != 0)
        return -1;
    if (iterations != NULL &&
        parse_unsigned(iterations, 1, 1000000,
                       &config->iterations) != 0)
        return -1;
    if (timeout != NULL) {
        if (parse_unsigned(timeout, 1, 60000, &parsed) != 0)
            return -1;
        config->timeout_ms = (int)parsed;
    }
    return 0;
}

static void print_usage(const char *program)
{
    fprintf(stderr,
            "usage: %s [--production] [--max-sockets N] "
            "[--iterations N] [--timeout-ms N]\n",
            program);
}

static int parse_arguments(int argc, char **argv, bench_config_t *config)
{
    int production = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--production") == 0)
            production = 1;
    }
    if (production) {
        config->max_sockets = BENCH_MAX_SOCKETS;
        config->iterations = 1000;
    }
    if (apply_environment(config) != 0) {
        fprintf(stderr, "invalid WEPOLL_EX_BENCH_* environment value\n");
        return -1;
    }

    for (int i = 1; i < argc; i++) {
        const char *name = argv[i];
        unsigned parsed;

        if (strcmp(name, "--production") == 0)
            continue;
        if (strcmp(name, "--help") == 0) {
            print_usage(argv[0]);
            return 1;
        }
        if (i + 1 >= argc) {
            print_usage(argv[0]);
            return -1;
        }
        if (strcmp(name, "--max-sockets") == 0) {
            if (parse_unsigned(argv[++i], 1, BENCH_MAX_SOCKETS,
                               &config->max_sockets) != 0)
                return -1;
        } else if (strcmp(name, "--iterations") == 0) {
            if (parse_unsigned(argv[++i], 1, 1000000,
                               &config->iterations) != 0)
                return -1;
        } else if (strcmp(name, "--timeout-ms") == 0) {
            if (parse_unsigned(argv[++i], 1, 60000, &parsed) != 0)
                return -1;
            config->timeout_ms = (int)parsed;
        } else {
            print_usage(argv[0]);
            return -1;
        }
    }
    return 0;
}

static void receiver_init(receiver_state_t *receiver)
{
    memset(receiver, 0, sizeof(*receiver));
    receiver->fd = INVALID_SOCKET;
}

static int receiver_open(receiver_state_t *receiver, int bind_receiver)
{
    int address_length = (int)sizeof(receiver->address);
    u_long nonblocking = 1;

    receiver_init(receiver);
    receiver->fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (receiver->fd == INVALID_SOCKET)
        return -1;
    receiver->address.sin_family = AF_INET;
    receiver->address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    receiver->address.sin_port = htons(0);
    if ((bind_receiver &&
         (bind(receiver->fd,
               (const struct sockaddr *)&receiver->address,
               (int)sizeof(receiver->address)) == SOCKET_ERROR ||
          getsockname(receiver->fd,
                      (struct sockaddr *)&receiver->address,
                      &address_length) == SOCKET_ERROR)) ||
        ioctlsocket(receiver->fd, FIONBIO, &nonblocking) == SOCKET_ERROR) {
        closesocket(receiver->fd);
        receiver_init(receiver);
        return -1;
    }
    return 0;
}

static void receiver_close(receiver_state_t *receiver)
{
    if (receiver->fd != INVALID_SOCKET)
        (void)closesocket(receiver->fd);
    receiver_init(receiver);
}

static int initialize_context(bench_context_t *context,
                              const bench_config_t *config)
{
    u_long nonblocking = 1;

    memset(context, 0, sizeof(*context));
    context->sender = INVALID_SOCKET;
    context->receiver_count = config->max_sockets;
    context->timeout_ms = config->timeout_ms;
    if (!QueryPerformanceFrequency(&context->frequency) ||
        context->frequency.QuadPart <= 0)
        return -1;

    context->receivers = (receiver_state_t *)calloc(
        context->receiver_count, sizeof(*context->receivers));
    if (context->receivers == NULL)
        return -1;
    for (unsigned i = 0; i < context->receiver_count; i++)
        receiver_init(&context->receivers[i]);

    context->sender = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (context->sender == INVALID_SOCKET ||
        ioctlsocket(context->sender, FIONBIO, &nonblocking) == SOCKET_ERROR)
        return -1;
    for (unsigned i = 0; i < context->receiver_count; i++) {
        /* Only ready-batch participants need ports.  Leaving the remaining
         * registration-only sockets unbound avoids exhausting the Windows
         * dynamic UDP port range in the 50,000-socket production run. */
        if (receiver_open(&context->receivers[i],
                          i < BENCH_MAX_BATCH) != 0) {
            fprintf(stderr, "receiver setup failed at %u/%u: WSA=%d\n",
                    i, context->receiver_count, WSAGetLastError());
            return -1;
        }
    }
    return 0;
}

static void destroy_context(bench_context_t *context)
{
    if (context->receivers != NULL) {
        for (unsigned i = 0; i < context->receiver_count; i++)
            receiver_close(&context->receivers[i]);
        free(context->receivers);
    }
    if (context->sender != INVALID_SOCKET)
        (void)closesocket(context->sender);
    memset(context, 0, sizeof(*context));
    context->sender = INVALID_SOCKET;
}

static int add_receiver(int epfd, const receiver_state_t *receiver,
                        unsigned index, uint32_t flags)
{
    struct epoll_event event;

    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN | flags;
    event.data.u64 = (uint64_t)index + 1;
    return epoll_ctl(epfd, EPOLL_CTL_ADD, receiver->fd, &event);
}

static int send_one(const bench_context_t *context, unsigned index)
{
    const char byte = 'x';
    const receiver_state_t *receiver = &context->receivers[index];

    return sendto(context->sender, &byte, 1, 0,
                  (const struct sockaddr *)&receiver->address,
                  (int)sizeof(receiver->address)) == 1 ? 0 : -1;
}

static int receive_one(const bench_context_t *context, unsigned index)
{
    char byte;

    return recv(context->receivers[index].fd, &byte, 1, 0) == 1 ? 0 : -1;
}

static int drain_one(const bench_context_t *context, unsigned index)
{
    char buffer[256];

    for (;;) {
        int result = recv(context->receivers[index].fd,
                          buffer, (int)sizeof(buffer), 0);
        if (result >= 0)
            continue;
        return WSAGetLastError() == WSAEWOULDBLOCK ? 0 : -1;
    }
}

static int benchmark_registration(bench_context_t *context, unsigned count)
{
    char parameter[32];
    uint64_t *add_samples = NULL;
    uint64_t *del_samples = NULL;
    int epfd = -1;
    int result = -1;

    add_samples = (uint64_t *)calloc(count, sizeof(*add_samples));
    del_samples = (uint64_t *)calloc(count, sizeof(*del_samples));
    if (add_samples == NULL || del_samples == NULL)
        goto cleanup;
    epfd = epoll_create_ex((int)count, 0);
    if (epfd < 0)
        goto cleanup;

    for (unsigned i = 0; i < count; i++) {
        uint64_t start = timer_now();
        int ctl_result = add_receiver(epfd, &context->receivers[i], i, 0);
        uint64_t end = timer_now();

        if (ctl_result != 0)
            goto cleanup;
        add_samples[i] = elapsed_ns(context, start, end);
    }
    if (epoll_fd_count(epfd) != (int)count)
        goto cleanup;
    for (unsigned offset = 0; offset < count; offset++) {
        unsigned i = count - offset - 1;
        uint64_t start = timer_now();
        int ctl_result = epoll_ctl(epfd, EPOLL_CTL_DEL,
                                   context->receivers[i].fd, NULL);
        uint64_t end = timer_now();

        if (ctl_result != 0)
            goto cleanup;
        del_samples[offset] = elapsed_ns(context, start, end);
    }

    snprintf(parameter, sizeof(parameter), "sockets=%u", count);
    report_samples("registration_add", parameter, add_samples, count, 1);
    report_samples("registration_del", parameter, del_samples, count, 1);
    result = 0;

cleanup:
    if (epfd >= 0 && wepoll_close(epfd) != 0)
        result = -1;
    free(del_samples);
    free(add_samples);
    return result;
}

static int ready_iteration(bench_context_t *context, int epfd,
                           unsigned batch, uint32_t *seen,
                           uint32_t generation, uint64_t *sample_out)
{
    struct epoll_event events[BENCH_MAX_BATCH];
    unsigned completed = 0;
    uint64_t start;

    for (unsigned i = 0; i < batch; i++) {
        if (drain_one(context, i) != 0)
            return -1;
    }
    start = timer_now();
    for (unsigned i = 0; i < batch; i++) {
        if (send_one(context, i) != 0)
            return -1;
    }
    while (completed < batch) {
        int count = epoll_wait(epfd, events, (int)batch,
                               context->timeout_ms);

        if (count <= 0)
            return -1;
        for (int i = 0; i < count; i++) {
            uint64_t data = events[i].data.u64;
            unsigned index;

            if (data == 0 || data > batch ||
                (events[i].events & EPOLLIN) == 0)
                return -1;
            index = (unsigned)data - 1;
            if (seen[index] != generation) {
                if (receive_one(context, index) != 0)
                    return -1;
                seen[index] = generation;
                completed++;
            }
        }
    }
    *sample_out = elapsed_ns(context, start, timer_now());
    return 0;
}

static int benchmark_ready_batch(bench_context_t *context,
                                 unsigned batch, unsigned iterations)
{
    uint64_t *samples = NULL;
    uint32_t *seen = NULL;
    char parameter[32];
    int epfd = -1;
    int result = -1;

    samples = (uint64_t *)calloc(iterations, sizeof(*samples));
    seen = (uint32_t *)calloc(batch, sizeof(*seen));
    if (samples == NULL || seen == NULL)
        goto cleanup;
    epfd = epoll_create_ex((int)batch, 0);
    if (epfd < 0)
        goto cleanup;
    for (unsigned i = 0; i < batch; i++) {
        if (add_receiver(epfd, &context->receivers[i], i, 0) != 0)
            goto cleanup;
    }

    for (uint32_t warmup = 1; warmup <= 5; warmup++) {
        uint64_t ignored;
        if (ready_iteration(context, epfd, batch, seen, warmup,
                            &ignored) != 0)
            goto cleanup;
    }
    memset(seen, 0, batch * sizeof(*seen));
    for (uint32_t i = 1; i <= iterations; i++) {
        if (ready_iteration(context, epfd, batch, seen, i,
                            &samples[i - 1]) != 0)
            goto cleanup;
    }

    snprintf(parameter, sizeof(parameter), "batch=%u", batch);
    report_samples("ready_batch", parameter, samples, iterations, batch);
    result = 0;

cleanup:
    if (epfd >= 0 && wepoll_close(epfd) != 0)
        result = -1;
    free(seen);
    free(samples);
    return result;
}

static int oneshot_iteration(bench_context_t *context, int epfd,
                             uint64_t *roundtrip_out, uint64_t *rearm_out)
{
    struct epoll_event event;
    uint64_t start;
    uint64_t rearm_start;

    if (drain_one(context, 0) != 0)
        return -1;
    start = timer_now();
    if (send_one(context, 0) != 0)
        return -1;
    if (epoll_wait(epfd, &event, 1, context->timeout_ms) != 1 ||
        event.data.u64 != 1 || (event.events & EPOLLIN) == 0)
        return -1;
    *roundtrip_out = elapsed_ns(context, start, timer_now());
    if (receive_one(context, 0) != 0)
        return -1;
    rearm_start = timer_now();
    if (epoll_rearm(epfd, context->receivers[0].fd) != 0)
        return -1;
    *rearm_out = elapsed_ns(context, rearm_start, timer_now());
    return 0;
}

static int benchmark_oneshot(bench_context_t *context, unsigned iterations)
{
    uint64_t *roundtrip = NULL;
    uint64_t *rearm = NULL;
    int epfd = -1;
    int result = -1;

    roundtrip = (uint64_t *)calloc(iterations, sizeof(*roundtrip));
    rearm = (uint64_t *)calloc(iterations, sizeof(*rearm));
    if (roundtrip == NULL || rearm == NULL)
        goto cleanup;
    epfd = epoll_create_ex(1, 0);
    if (epfd < 0 ||
        add_receiver(epfd, &context->receivers[0], 0,
                     EPOLLONESHOT) != 0)
        goto cleanup;

    for (unsigned i = 0; i < 10; i++) {
        uint64_t ignored_roundtrip;
        uint64_t ignored_rearm;
        if (oneshot_iteration(context, epfd, &ignored_roundtrip,
                              &ignored_rearm) != 0)
            goto cleanup;
    }
    for (unsigned i = 0; i < iterations; i++) {
        if (oneshot_iteration(context, epfd, &roundtrip[i],
                              &rearm[i]) != 0)
            goto cleanup;
    }
    report_samples("oneshot_roundtrip", "socket=1", roundtrip,
                   iterations, 1);
    report_samples("oneshot_rearm", "socket=1", rearm, iterations, 1);
    result = 0;

cleanup:
    if (epfd >= 0 && wepoll_close(epfd) != 0)
        result = -1;
    free(rearm);
    free(roundtrip);
    return result;
}

static int control_cycle(bench_context_t *context, int epfd,
                         unsigned sequence, uint64_t *sample_out)
{
    struct epoll_event event;
    struct epoll_event ignored;
    uint64_t start;

    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN;
    event.data.u64 = (uint64_t)sequence + 1;
    start = timer_now();
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, context->receivers[0].fd,
                  &event) != 0 ||
        epoll_drain(epfd, &ignored, 1) < 0)
        return -1;
    event.data.u64++;
    if (epoll_ctl(epfd, EPOLL_CTL_MOD, context->receivers[0].fd,
                  &event) != 0 ||
        epoll_drain(epfd, &ignored, 1) < 0 ||
        epoll_ctl(epfd, EPOLL_CTL_DEL, context->receivers[0].fd,
                  NULL) != 0)
        return -1;
    *sample_out = elapsed_ns(context, start, timer_now());
    return 0;
}

static int benchmark_control_churn(bench_context_t *context,
                                   unsigned iterations)
{
    uint64_t *samples = NULL;
    int epfd = -1;
    int result = -1;

    samples = (uint64_t *)calloc(iterations, sizeof(*samples));
    if (samples == NULL)
        goto cleanup;
    epfd = epoll_create_ex(1, 0);
    if (epfd < 0)
        goto cleanup;
    for (unsigned i = 0; i < 10; i++) {
        uint64_t ignored;
        if (control_cycle(context, epfd, i, &ignored) != 0)
            goto cleanup;
    }
    for (unsigned i = 0; i < iterations; i++) {
        if (control_cycle(context, epfd, i + 10, &samples[i]) != 0)
            goto cleanup;
    }
    report_samples("control_churn", "add+mod+del", samples,
                   iterations, 3);
    result = 0;

cleanup:
    if (epfd >= 0 && wepoll_close(epfd) != 0)
        result = -1;
    free(samples);
    return result;
}

static int run_registration_points(bench_context_t *context,
                                   unsigned maximum)
{
    static const unsigned standard_points[] = { 1000, 10000, 50000 };
    unsigned last = 0;

    for (size_t i = 0;
         i < sizeof(standard_points) / sizeof(standard_points[0]); i++) {
        unsigned point = standard_points[i];
        if (point > maximum)
            break;
        if (benchmark_registration(context, point) != 0)
            return -1;
        last = point;
    }
    if (last != maximum && benchmark_registration(context, maximum) != 0)
        return -1;
    return 0;
}

int main(int argc, char **argv)
{
    bench_config_t config = { 1000, 200, 5000 };
    bench_context_t context;
    static const unsigned batches[] = { 1, 16, 64, 512 };
    WSADATA winsock;
    uint64_t started;
    int result = 1;
    int parse_result = parse_arguments(argc, argv, &config);

    if (parse_result != 0)
        return parse_result > 0 ? 0 : 2;
    if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        return 1;
    }
    if (initialize_context(&context, &config) != 0) {
        fprintf(stderr, "benchmark setup failed: errno=%d WSA=%d\n",
                errno, WSAGetLastError());
        destroy_context(&context);
        WSACleanup();
        return 1;
    }

    started = timer_now();
    printf("# wepoll_ex_version=%s,max_sockets=%u,iterations=%u,"
           "qpc_frequency=%lld\n",
           wepoll_ex_version_string(), config.max_sockets,
           config.iterations, (long long)context.frequency.QuadPart);
    puts("benchmark,parameter,samples,p50_ns,p95_ns,p99_ns,max_ns,"
         "operations_per_second");

    if (run_registration_points(&context, config.max_sockets) != 0) {
        fprintf(stderr, "registration benchmark failed: errno=%d WSA=%d\n",
                errno, WSAGetLastError());
        goto cleanup;
    }
    for (size_t i = 0; i < sizeof(batches) / sizeof(batches[0]); i++) {
        if (batches[i] > config.max_sockets)
            continue;
        if (benchmark_ready_batch(&context, batches[i],
                                  config.iterations) != 0) {
            fprintf(stderr, "ready batch %u failed: errno=%d WSA=%d\n",
                    batches[i], errno, WSAGetLastError());
            goto cleanup;
        }
    }
    if (benchmark_oneshot(&context, config.iterations) != 0) {
        fprintf(stderr, "oneshot benchmark failed: errno=%d WSA=%d\n",
                errno, WSAGetLastError());
        goto cleanup;
    }
    if (benchmark_control_churn(&context, config.iterations) != 0) {
        fprintf(stderr, "control benchmark failed: errno=%d WSA=%d\n",
                errno, WSAGetLastError());
        goto cleanup;
    }
    fprintf(stderr, "benchmark_elapsed_ms=%" PRIu64 "\n",
            elapsed_ns(&context, started, timer_now()) / UINT64_C(1000000));
    result = 0;

cleanup:
    destroy_context(&context);
    WSACleanup();
    return result;
}

#else

int main(void)
{
    fprintf(stderr, "bench_windows is only supported on Windows\n");
    return 77;
}

#endif
