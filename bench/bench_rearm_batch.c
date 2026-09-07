/* Scalar versus batch explicit acknowledgement of repeatedly writable UDP
 * sockets. Every timed acknowledgement changes delivered/disarmed state;
 * each roundtrip includes the following native readiness delivery. */
#include "wepoll_ex.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MAX_BATCH = 256, WARMUP_ROUNDS = 20 };

static uint64_t timer_now(void)
{
    LARGE_INTEGER value;
    QueryPerformanceCounter(&value);
    return (uint64_t)value.QuadPart;
}

static uint64_t elapsed_ns(uint64_t start, uint64_t end, uint64_t frequency)
{
    double value = (double)(end - start) * 1000000000.0 / (double)frequency;
    return value < 1 ? 1 : (uint64_t)value;
}

static int compare_u64(const void *left, const void *right)
{
    uint64_t a = *(const uint64_t *)left;
    uint64_t b = *(const uint64_t *)right;
    return a < b ? -1 : a > b ? 1 : 0;
}

static void report(const char *name, int count, uint64_t *samples, int iterations)
{
    long double total = 0;
    qsort(samples, (size_t)iterations, sizeof(*samples), compare_u64);
    for (int i = 0; i < iterations; i++) total += samples[i];
    printf("%s,batch=%d,%d,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%.0f\n",
           name, count, iterations, samples[((iterations - 1) * 50 + 99) / 100],
           samples[((iterations - 1) * 95 + 99) / 100],
           samples[((iterations - 1) * 99 + 99) / 100],
           (double)((long double)count * iterations * 1000000000.0L / total));
}

static int drain_ready(int epfd, int count)
{
    struct epoll_event events[MAX_BATCH];
    unsigned char seen[MAX_BATCH] = {0};
    int total = 0;
    ULONGLONG deadline = GetTickCount64() + 5000;

    while (total < count && GetTickCount64() < deadline) {
        int returned = epoll_wait(epfd, events, count, 1000);
        if (returned <= 0) return -1;
        for (int i = 0; i < returned; i++) {
            uint32_t index = events[i].data.u32;
            if (index >= (uint32_t)count || seen[index] ||
                events[i].events != EPOLLOUT) return -1;
            seen[index] = 1;
            total++;
        }
    }
    return total == count ? 0 : -1;
}

static int run_case(int batched, int count, int iterations, uint64_t frequency)
{
    epoll_fd_t fds[MAX_BATCH];
    uint32_t classes[MAX_BATCH];
    int errors[MAX_BATCH];
    uint64_t *rearm_samples = NULL;
    uint64_t *roundtrip_samples = NULL;
    int epfd = -1;
    int result = -1;

    for (int i = 0; i < count; i++) {
        fds[i] = INVALID_SOCKET;
        classes[i] = WEPOLL_EX_REARM_WRITE;
    }
    rearm_samples = calloc((size_t)iterations, sizeof(*rearm_samples));
    roundtrip_samples = calloc((size_t)iterations, sizeof(*roundtrip_samples));
    if (rearm_samples == NULL || roundtrip_samples == NULL) goto cleanup;
    epfd = epoll_create_ex(0, WEPOLL_EX_CREATE_EXPLICIT_REARM);
    if (epfd < 0) goto cleanup;
    for (int i = 0; i < count; i++) {
        struct sockaddr_in address = {0};
        struct epoll_event event = {0};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        fds[i] = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (fds[i] == INVALID_SOCKET ||
            bind(fds[i], (struct sockaddr *)&address, sizeof(address)) != 0) goto cleanup;
        event.events = EPOLLOUT | EPOLLET;
        event.data.u32 = (uint32_t)i;
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, fds[i], &event) != 0) goto cleanup;
    }
    if (drain_ready(epfd, count) != 0) goto cleanup;
    for (int round = -WARMUP_ROUNDS; round < iterations; round++) {
        uint64_t start = timer_now();
        if (batched) {
            if (epoll_rearm_classes_batch(epfd, fds, classes, errors, count) != 0)
                goto cleanup;
        } else {
            for (int i = 0; i < count; i++) {
                if (epoll_rearm_classes(epfd, fds[i], classes[i]) != 0) goto cleanup;
            }
        }
        uint64_t rearmed = timer_now();
        if (drain_ready(epfd, count) != 0) goto cleanup;
        uint64_t delivered = timer_now();
        if (batched) {
            for (int i = 0; i < count; i++) {
                if (errors[i] != 0) goto cleanup;
            }
        }
        if (round >= 0) {
            rearm_samples[round] = elapsed_ns(start, rearmed, frequency);
            roundtrip_samples[round] = elapsed_ns(start, delivered, frequency);
        }
    }
    result = 0;
cleanup:
    if (result != 0) {
        fprintf(stderr, "rearm benchmark failed: mode=%s count=%d errno=%d WSA=%d\n",
                batched ? "batch" : "scalar", count, errno, WSAGetLastError());
    }
    if (epfd >= 0 && wepoll_close(epfd) != 0) result = -1;
    for (int i = 0; i < count; i++) {
        if (fds[i] != INVALID_SOCKET && closesocket(fds[i]) != 0) result = -1;
    }
    if (result == 0) {
        report("explicit_rearm", count, rearm_samples, iterations);
        report("explicit_roundtrip", count, roundtrip_samples, iterations);
    }
    free(rearm_samples);
    free(roundtrip_samples);
    return result;
}

int main(int argc, char **argv)
{
    WSADATA data;
    LARGE_INTEGER frequency;
    const int sizes[] = {1, 16, 64, MAX_BATCH};
    char *end = NULL;
    long iterations;
    int result = 0;
    int batched;

    if (argc != 3 || (strcmp(argv[1], "scalar") != 0 && strcmp(argv[1], "batch") != 0)) {
        fprintf(stderr, "usage: %s scalar|batch ITERATIONS\n", argv[0]);
        return 2;
    }
    errno = 0;
    iterations = strtol(argv[2], &end, 10);
    if (errno != 0 || end == argv[2] || *end != '\0' || iterations < 1 || iterations > 1000000)
        return 2;
    batched = strcmp(argv[1], "batch") == 0;
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return 1;
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) {
        WSACleanup();
        return 1;
    }
    printf("# mode=%s iterations=%ld lifetime_policy=%d\n", argv[1], iterations,
           (int)wepoll_ex_get_socket_lifetime_policy());
    printf("benchmark,parameter,samples,p50_ns,p95_ns,p99_ns,operations_per_second\n");
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        if (run_case(batched, sizes[i], (int)iterations, (uint64_t)frequency.QuadPart) != 0) {
            result = 1;
            break;
        }
    }
    if (fflush(stdout) != 0 || ferror(stdout)) result = 1;
    WSACleanup();
    return result;
}
