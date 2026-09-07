#include "wepoll_ex.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { SOCKET_COUNT = 129 };

typedef struct fixture {
    int epfd;
    int count;
    SOCKET client[SOCKET_COUNT];
    epoll_fd_t server[SOCKET_COUNT];
} fixture_t;

#define CHECK(expression) do { \
    if (!(expression)) { \
        fprintf(stderr, "line %d: %s (errno=%d WSA=%d)\n", \
                __LINE__, #expression, errno, WSAGetLastError()); \
        goto cleanup; \
    } \
} while (0)

static void fixture_close(fixture_t *fixture)
{
    if (fixture->epfd >= 0) (void)wepoll_close(fixture->epfd);
    for (int i = 0; i < fixture->count; i++) {
        if (fixture->server[i] != INVALID_SOCKET) closesocket(fixture->server[i]);
        if (fixture->client[i] != INVALID_SOCKET) closesocket(fixture->client[i]);
    }
}

static int fixture_open(fixture_t *fixture, int count, int flags, uint32_t events)
{
    SOCKET listener = INVALID_SOCKET;
    struct sockaddr_in address;
    int length = sizeof(address);
    int result = -1;

    memset(fixture, 0, sizeof(*fixture));
    fixture->epfd = -1;
    fixture->count = count;
    for (int i = 0; i < count; i++) {
        fixture->client[i] = fixture->server[i] = INVALID_SOCKET;
    }
    fixture->epfd = epoll_create_ex(0, flags);
    CHECK(fixture->epfd >= 0);
    listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    CHECK(listener != INVALID_SOCKET);
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(bind(listener, (struct sockaddr *)&address, length) == 0);
    CHECK(listen(listener, 1) == 0);
    CHECK(getsockname(listener, (struct sockaddr *)&address, &length) == 0);
    for (int i = 0; i < count; i++) {
        struct epoll_event event = {0};
        u_long nonblocking = 1;

        fixture->client[i] = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        CHECK(fixture->client[i] != INVALID_SOCKET);
        CHECK(connect(fixture->client[i], (struct sockaddr *)&address, length) == 0);
        fixture->server[i] = accept(listener, NULL, NULL);
        CHECK(fixture->server[i] != INVALID_SOCKET);
        CHECK(ioctlsocket(fixture->server[i], FIONBIO, &nonblocking) == 0);
        event.events = events;
        event.data.u64 = (uint64_t)i + 100;
        CHECK(epoll_ctl_ctx(fixture->epfd, EPOLL_CTL_ADD, fixture->server[i],
                            &event, &fixture->server[i]) == 0);
    }
    result = 0;
cleanup:
    if (listener != INVALID_SOCKET) closesocket(listener);
    return result;
}

static int expect_ready(fixture_t *fixture, int count, uint32_t mask)
{
    struct epoll_event_ex events[SOCKET_COUNT];
    int seen[SOCKET_COUNT] = {0};
    int total = 0;

    while (total < count) {
        int returned = epoll_wait_ex(fixture->epfd, events, SOCKET_COUNT, 2000);
        if (returned <= 0) return -1;
        for (int i = 0; i < returned; i++) {
            uint64_t index = events[i].data.u64 - 100;
            if (index >= (uint64_t)fixture->count || seen[index] ||
                events[i].user_ctx != &fixture->server[index] ||
                (events[i].events & mask) != mask) return -1;
            seen[index] = 1;
            total++;
        }
    }
    return total == count ? 0 : -1;
}

static int test_validation(void)
{
    fixture_t fixture;
    uint32_t classes = WEPOLL_EX_REARM_WRITE;
    int error = 123;
    int result = -1;

    if (fixture_open(&fixture, 1, 0, EPOLLOUT | EPOLLET) != 0) goto cleanup;
    CHECK(epoll_rearm_classes_batch(fixture.epfd, fixture.server, &classes,
                                    &error, 0) == -1 && errno == EINVAL);
    CHECK(error == 123);
    CHECK(epoll_rearm_classes_batch(fixture.epfd, fixture.server, &classes,
                                    &error, -1) == -1 && errno == EINVAL);
    CHECK(epoll_rearm_classes_batch(fixture.epfd, NULL, &classes,
                                    &error, 1) == -1 && errno == EFAULT);
    CHECK(epoll_rearm_classes_batch(fixture.epfd, fixture.server, NULL,
                                    &error, 1) == -1 && errno == EFAULT);
    CHECK(epoll_rearm_classes_batch(fixture.epfd, fixture.server, &classes,
                                    NULL, 1) == -1 && errno == EFAULT);
    CHECK(epoll_rearm_classes_batch(-1, fixture.server, &classes,
                                    &error, 1) == -1 && errno == EBADF);
    CHECK(error == 123);
    CHECK(epoll_rearm_classes_batch(fixture.epfd, fixture.server, &classes,
                                    &error, 1) == -1 && errno == EOPNOTSUPP);
    CHECK(error == EOPNOTSUPP);
    result = 0;
cleanup:
    fixture_close(&fixture);
    return result;
}

static int test_bulk(void)
{
    fixture_t fixture;
    epoll_fd_t fds[SOCKET_COUNT];
    uint32_t classes[SOCKET_COUNT];
    int errors[SOCKET_COUNT];
    int result = -1;

    if (fixture_open(&fixture, SOCKET_COUNT, WEPOLL_EX_CREATE_EXPLICIT_REARM,
                      EPOLLOUT | EPOLLET) != 0) goto cleanup;
    for (int i = 0; i < SOCKET_COUNT; i++) classes[i] = WEPOLL_EX_REARM_WRITE;
    for (int round = 0; round < 3; round++) {
        CHECK(expect_ready(&fixture, SOCKET_COUNT, EPOLLOUT) == 0);
        memset(errors, 0x7f, sizeof(errors));
        CHECK(epoll_rearm_classes_batch(fixture.epfd, fixture.server, classes,
                                        errors, SOCKET_COUNT) == 0);
        for (int i = 0; i < SOCKET_COUNT; i++) CHECK(errors[i] == 0);
    }
    CHECK(expect_ready(&fixture, SOCKET_COUNT, EPOLLOUT) == 0);
    memcpy(fds, fixture.server, sizeof(fds));
    classes[63] = 0;
    fds[64] = EPOLL_FD_INVALID;
    CHECK(epoll_rearm_classes_batch(fixture.epfd, fds, classes,
                                    errors, SOCKET_COUNT) == -1 && errno == EINVAL);
    for (int i = 0; i < SOCKET_COUNT; i++) {
        CHECK(errors[i] == (i == 63 ? EINVAL : i == 64 ? EBADF : 0));
    }
    CHECK(expect_ready(&fixture, SOCKET_COUNT - 2, EPOLLOUT) == 0);
    classes[63] = WEPOLL_EX_REARM_WRITE;
    CHECK(epoll_rearm_classes_batch(fixture.epfd, &fixture.server[63],
                                    &classes[63], errors, 2) == 0);
    CHECK(expect_ready(&fixture, 2, EPOLLOUT) == 0);
    result = 0;
cleanup:
    fixture_close(&fixture);
    return result;
}

static int test_mixed(void)
{
    fixture_t fixture;
    epoll_fd_t fds[6];
    uint32_t classes[6] = {WEPOLL_EX_REARM_WRITE, WEPOLL_EX_REARM_WRITE, 0,
                           WEPOLL_EX_REARM_WRITE, WEPOLL_EX_REARM_WRITE,
                           UINT32_MAX};
    int errors[6];
    wepoll_ex_error_info info;
    int result = -1;

    if (fixture_open(&fixture, 3, WEPOLL_EX_CREATE_EXPLICIT_REARM,
                      EPOLLOUT | EPOLLET) != 0) goto cleanup;
    CHECK(expect_ready(&fixture, 3, EPOLLOUT) == 0);
    fds[0] = fixture.server[0];
    fds[1] = EPOLL_FD_INVALID;
    fds[2] = fixture.server[1];
    fds[3] = fixture.server[2];
    fds[4] = fixture.server[0];
    fds[5] = fixture.server[1];
    CHECK(epoll_rearm_classes_batch(fixture.epfd, fds, classes, errors, 6) == -1);
    CHECK(errno == EBADF && errors[0] == 0 && errors[1] == EBADF &&
          errors[2] == EINVAL && errors[3] == 0 && errors[4] == 0 &&
          errors[5] == EINVAL);
    CHECK(wepoll_ex_get_last_error_info(&info, sizeof(info)) == 0);
    CHECK(info.portable_error == EBADF);
    CHECK(expect_ready(&fixture, 2, EPOLLOUT) == 0);
    CHECK(epoll_ctl(fixture.epfd, EPOLL_CTL_DEL, fixture.server[1], NULL) == 0);
    classes[0] = WEPOLL_EX_REARM_WRITE;
    CHECK(epoll_rearm_classes_batch(fixture.epfd, &fixture.server[1], classes,
                                    errors, 1) == -1);
    CHECK(errno == ENOENT && errors[0] == ENOENT);
    result = 0;
cleanup:
    fixture_close(&fixture);
    return result;
}

static int test_oneshot(void)
{
    fixture_t fixture;
    struct epoll_event_ex event;
    uint32_t classes = WEPOLL_EX_REARM_WRITE;
    int error;
    char byte;
    int alias = -1;
    int result = -1;

    if (fixture_open(&fixture, 1, WEPOLL_EX_CREATE_EXPLICIT_REARM,
                      EPOLLIN | EPOLLOUT | EPOLLET | EPOLLONESHOT) != 0) goto cleanup;
    CHECK(send(fixture.client[0], "x", 1, 0) == 1);
    CHECK(expect_ready(&fixture, 1, EPOLLIN | EPOLLOUT) == 0);
    CHECK(recv(fixture.server[0], &byte, 1, 0) == 1 && byte == 'x');
    alias = wepoll_ex_dup(fixture.epfd);
    CHECK(alias >= 0);
    CHECK(epoll_rearm_classes_batch(alias, fixture.server, &classes, &error, 1) == 0);
    CHECK(error == 0 && epoll_wait_ex(fixture.epfd, &event, 1, 0) == 0);
    classes = WEPOLL_EX_REARM_READ;
    CHECK(epoll_rearm_classes_batch(alias, fixture.server, &classes, &error, 1) == 0);
    CHECK(expect_ready(&fixture, 1, EPOLLOUT) == 0);
    CHECK(wepoll_ex_shutdown_socket(fixture.epfd, fixture.server[0], SD_BOTH) == 0);
    classes = WEPOLL_EX_REARM_ALL;
    CHECK(epoll_rearm_classes_batch(alias, fixture.server, &classes, &error, 1) == 0);
    CHECK(epoll_wait_ex(fixture.epfd, &event, 1, 2000) == 1);
    CHECK((event.events & EPOLLHUP) != 0);
    CHECK(epoll_wait_ex(fixture.epfd, &event, 1, 0) == 0);
    result = 0;
cleanup:
    if (alias >= 0) (void)wepoll_close(alias);
    fixture_close(&fixture);
    return result;
}

static int test_read(void)
{
    fixture_t fixture;
    struct epoll_event event;
    uint32_t classes = WEPOLL_EX_REARM_READ;
    int error;
    char byte;
    int result = -1;

    if (fixture_open(&fixture, 1, WEPOLL_EX_CREATE_EXPLICIT_REARM,
                      EPOLLIN | EPOLLET) != 0) goto cleanup;
    CHECK(send(fixture.client[0], "xy", 2, 0) == 2);
    CHECK(expect_ready(&fixture, 1, EPOLLIN) == 0);
    CHECK(recv(fixture.server[0], &byte, 1, 0) == 1 && byte == 'x');
    CHECK(epoll_wait(fixture.epfd, &event, 1, 0) == 0);
    CHECK(epoll_rearm_classes_batch(fixture.epfd, fixture.server,
                                    &classes, &error, 1) == 0 && error == 0);
    CHECK(expect_ready(&fixture, 1, EPOLLIN) == 0);
    CHECK(recv(fixture.server[0], &byte, 1, 0) == 1 && byte == 'y');
    CHECK(recv(fixture.server[0], &byte, 1, 0) == SOCKET_ERROR &&
          WSAGetLastError() == WSAEWOULDBLOCK);
    CHECK(epoll_rearm_classes_batch(fixture.epfd, fixture.server,
                                    &classes, &error, 1) == 0 && error == 0);
    CHECK(epoll_wait(fixture.epfd, &event, 1, 0) == 0);
    CHECK(send(fixture.client[0], "z", 1, 0) == 1);
    CHECK(expect_ready(&fixture, 1, EPOLLIN) == 0);
    result = 0;
cleanup:
    fixture_close(&fixture);
    return result;
}

typedef struct wait_context {
    int epfd;
    HANDLE started;
    int result;
    int error;
} wait_context_t;

static DWORD WINAPI wait_thread(void *opaque)
{
    wait_context_t *context = opaque;
    struct epoll_event event;
    SetEvent(context->started);
    context->result = epoll_wait(context->epfd, &event, 1, 2000);
    context->error = errno;
    return 0;
}

static int test_close_wait(void)
{
    fixture_t fixture;
    wait_context_t context = {0};
    HANDLE thread = NULL;
    uint32_t classes = WEPOLL_EX_REARM_READ;
    int error;
    int result = -1;

    if (fixture_open(&fixture, 1, WEPOLL_EX_CREATE_EXPLICIT_REARM,
                      EPOLLIN | EPOLLET) != 0) goto cleanup;
    context.epfd = fixture.epfd;
    context.started = CreateEventW(NULL, TRUE, FALSE, NULL);
    CHECK(context.started != NULL);
    thread = CreateThread(NULL, 0, wait_thread, &context, 0, NULL);
    CHECK(thread != NULL);
    CHECK(WaitForSingleObject(context.started, 2000) == WAIT_OBJECT_0);
    for (int i = 0; i < 100; i++) {
        CHECK(epoll_rearm_classes_batch(fixture.epfd, fixture.server,
                                        &classes, &error, 1) == 0 && error == 0);
    }
    CHECK(wepoll_close(fixture.epfd) == 0);
    fixture.epfd = -1;
    CHECK(WaitForSingleObject(thread, 3000) == WAIT_OBJECT_0);
    CHECK(context.result == -1 && context.error == EBADF);
    error = 123;
    CHECK(epoll_rearm_classes_batch(context.epfd, fixture.server,
                                    &classes, &error, 1) == -1 && errno == EBADF);
    CHECK(error == 123);
    result = 0;
cleanup:
    fixture_close(&fixture);
    if (thread != NULL) {
        if (WaitForSingleObject(thread, 3000) != WAIT_OBJECT_0) ExitProcess(1);
        CloseHandle(thread);
    }
    if (context.started != NULL) CloseHandle(context.started);
    return result;
}

int main(int argc, char **argv)
{
    WSADATA data;
    int result;

    if (argc != 2 || WSAStartup(MAKEWORD(2, 2), &data) != 0) return 2;
    if (strcmp(argv[1], "validation") == 0) result = test_validation();
    else if (strcmp(argv[1], "bulk") == 0) result = test_bulk();
    else if (strcmp(argv[1], "mixed") == 0) result = test_mixed();
    else if (strcmp(argv[1], "oneshot") == 0) result = test_oneshot();
    else if (strcmp(argv[1], "read") == 0) result = test_read();
    else if (strcmp(argv[1], "close-wait") == 0) result = test_close_wait();
    else result = -1;
    WSACleanup();
    return result == 0 ? 0 : 1;
}
