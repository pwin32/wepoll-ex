#include "wepoll_ex_internal.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#define CHECK(expression) do { \
    if (!(expression)) { \
        fprintf(stderr, "line %d: %s (errno=%d)\n", __LINE__, #expression, errno); \
        goto cleanup; \
    } \
} while (0)

typedef struct fixture {
    ep_port_t *port;
    epoll_fd_t fds[3];
    SOCKET clients[3];
    ep_sock_t *socks[3];
} fixture_t;

static int fixture_open(fixture_t *fixture, int oneshot, int tcp)
{
    struct sockaddr_in address = {0};
    SOCKET listener = INVALID_SOCKET;
    int length = sizeof(address);
    epoll_event_ex events[3];
    int seen[3] = {0};
    int total = 0;
    int result = -1;

    memset(fixture, 0, sizeof(*fixture));
    for (int i = 0; i < 3; i++) {
        fixture->fds[i] = fixture->clients[i] = INVALID_SOCKET;
    }
    CHECK(ep_port_create(0, WEPOLL_EX_CREATE_EXPLICIT_REARM, &fixture->port) == 0);
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (tcp) {
        listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        CHECK(listener != INVALID_SOCKET);
        CHECK(bind(listener, (struct sockaddr *)&address, length) == 0);
        CHECK(listen(listener, 1) == 0);
        CHECK(getsockname(listener, (struct sockaddr *)&address, &length) == 0);
    }
    for (int i = 0; i < 3; i++) {
        epoll_data_t data = {0};
        if (tcp) {
            fixture->clients[i] = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            CHECK(fixture->clients[i] != INVALID_SOCKET);
            CHECK(connect(fixture->clients[i], (struct sockaddr *)&address, length) == 0);
            fixture->fds[i] = accept(listener, NULL, NULL);
            CHECK(fixture->fds[i] != INVALID_SOCKET);
        } else {
            fixture->fds[i] = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            CHECK(fixture->fds[i] != INVALID_SOCKET);
            CHECK(bind(fixture->fds[i], (struct sockaddr *)&address, length) == 0);
        }
        data.u32 = (uint32_t)i;
        CHECK(ep_port_register(fixture->port, fixture->fds[i], EPOLLOUT,
                                EPOLLET | (oneshot ? EPOLLONESHOT : 0),
                                data, NULL) == 0);
    }
    while (total < 3) {
        int count = ep_port_wait(fixture->port, events, 3, 2000, NULL);
        CHECK(count > 0);
        for (int i = 0; i < count; i++) {
            CHECK(events[i].data.u32 < 3 && !seen[events[i].data.u32]);
            seen[events[i].data.u32] = 1;
            total++;
        }
    }
    for (ep_sock_t *sock = fixture->port->sock_list_head; sock != NULL; sock = sock->next) {
        fixture->socks[sock->user_data.u32] = sock;
    }
    result = 0;
cleanup:
    if (listener != INVALID_SOCKET) closesocket(listener);
    return result;
}

static void fixture_close(fixture_t *fixture)
{
    ep_fault_reset();
    if (fixture->port != NULL) {
        atomic_store(&fixture->port->waiter_active, 0);
        (void)ep_port_destroy(fixture->port);
    }
    for (int i = 0; i < 3; i++) {
        if (fixture->fds[i] != INVALID_SOCKET) closesocket(fixture->fds[i]);
        if (fixture->clients[i] != INVALID_SOCKET) closesocket(fixture->clients[i]);
    }
}

static int test_submit_failure(void)
{
    fixture_t fixture;
    uint32_t classes[3] = {WEPOLL_EX_REARM_WRITE, WEPOLL_EX_REARM_WRITE,
                           WEPOLL_EX_REARM_WRITE};
    int errors[3];
    int result = -1;
    uint64_t generation;
    wepoll_ex_error_info info;

    if (fixture_open(&fixture, 1, 0) != 0) goto cleanup;
    generation = fixture.socks[1]->generation;
    atomic_store(&fixture.port->waiter_active, 1);
    CHECK(ep_fault_configure(EP_FAULT_AFD_SUBMIT, 2, EACCES) == 0);
    CHECK(ep_port_rearm_classes_batch(fixture.port, fixture.fds, classes, errors, 3) == -1);
    CHECK(errno == EACCES && errors[0] == 0 && errors[1] == EACCES && errors[2] == 0);
    CHECK(ep_fault_hits(EP_FAULT_AFD_SUBMIT) == 3);
    CHECK(wepoll_ex_get_last_error_info(&info, sizeof(info)) == 0 &&
          info.portable_error == EACCES);
    CHECK(fixture.socks[1]->oneshot_fired &&
          fixture.socks[1]->explicit_disarmed_classes == WEPOLL_EX_REARM_WRITE &&
          fixture.socks[1]->generation == generation);
    CHECK(!fixture.socks[0]->oneshot_fired && !fixture.socks[2]->oneshot_fired);
    CHECK(ep_port_worklists_valid_locked(fixture.port));
    ep_fault_reset();
    atomic_store(&fixture.port->waiter_active, 0);
    CHECK(ep_port_rearm_classes_batch(fixture.port, &fixture.fds[1], classes, errors, 1) == 0);
    CHECK(errors[0] == 0 && !fixture.socks[1]->oneshot_fired);
    result = 0;
cleanup:
    fixture_close(&fixture);
    return result;
}

static int test_busy_closed(void)
{
    fixture_t fixture;
    uint32_t classes[3] = {WEPOLL_EX_REARM_WRITE, WEPOLL_EX_REARM_WRITE,
                           WEPOLL_EX_REARM_WRITE};
    int errors[3];
    int result = -1;

    if (fixture_open(&fixture, 0, 0) != 0) goto cleanup;
    atomic_store(&fixture.socks[1]->ready_queued, 1);
    CHECK(ep_port_rearm_classes_batch(fixture.port, fixture.fds, classes, errors, 3) == -1);
    CHECK(errno == EBUSY && errors[0] == 0 && errors[1] == EBUSY && errors[2] == 0);
    CHECK(fixture.socks[1]->explicit_disarmed_classes == WEPOLL_EX_REARM_WRITE);
    atomic_store(&fixture.socks[1]->ready_queued, 0);
    ep_port_begin_close(fixture.port);
    CHECK(ep_port_rearm_classes_batch(fixture.port, fixture.fds, classes, errors, 3) == -1);
    CHECK(errno == EBADF && errors[0] == EBADF && errors[1] == EBADF && errors[2] == EBADF);
    result = 0;
cleanup:
    if (fixture.socks[1] != NULL) atomic_store(&fixture.socks[1]->ready_queued, 0);
    fixture_close(&fixture);
    return result;
}

static int test_wake(int post_failure)
{
    fixture_t fixture;
    epoll_fd_t fds[65];
    uint32_t classes[65];
    int errors[65];
    int count = post_failure == 2 ? 65 : 2;
    int result = -1;
    ULONG removed = 0;
    OVERLAPPED_ENTRY entries[8];
    wepoll_ex_error_info info;

    if (fixture_open(&fixture, 0, 1) != 0) goto cleanup;
    /* Isolate the synthetic-ready wake from socket scheduling.  No native
     * request is pending after the initial writable delivery. */
    fixture.socks[0]->local_shutdown = EP_LOCAL_SHUTDOWN_READ | EP_LOCAL_SHUTDOWN_WRITE;
    fixture.socks[0]->explicit_disarmed_classes = WEPOLL_EX_REARM_ALL;
    atomic_store(&fixture.port->waiter_active, 1);
    fds[0] = fixture.fds[0];
    fds[1] = post_failure ? fixture.fds[1] : EPOLL_FD_INVALID;
    classes[0] = WEPOLL_EX_REARM_ALL;
    classes[1] = WEPOLL_EX_REARM_WRITE;
    if (post_failure == 2) {
        /* Idempotent acknowledgements fill the rest of the first chunk.
         * Its failed wake closes the port before the last entry. */
        for (int i = 1; i < count; i++) {
            fds[i] = fixture.fds[1];
            classes[i] = WEPOLL_EX_REARM_READ;
        }
    }
    if (post_failure) CHECK(ep_fault_configure(EP_FAULT_IOCP_POST, 1, EIO) == 0);
    CHECK(ep_port_rearm_classes_batch(fixture.port, fds, classes, errors, count) == -1);
    CHECK(errors[0] == 0 && errors[1] == (post_failure ? 0 : EBADF));
    CHECK(errno == (post_failure ? EIO : EBADF));
    if (post_failure) {
        CHECK(wepoll_ex_get_last_error_info(&info, sizeof(info)) == 0);
        CHECK(info.portable_error == EIO &&
              info.native_domain == WEPOLL_EX_NATIVE_ERROR_WIN32 &&
              info.native_code == ERROR_GEN_FAILURE);
        CHECK(atomic_load(&fixture.port->closing));
        CHECK(ep_fault_hits(EP_FAULT_IOCP_POST) == 1);
        if (post_failure == 2) {
            for (int i = 0; i < 64; i++) CHECK(errors[i] == 0);
            CHECK(errors[64] == EBADF);
        }
    } else {
        CHECK(GetQueuedCompletionStatusEx(fixture.port->iocp, entries, 8, &removed, 0, FALSE));
        CHECK(removed == 1 && entries[0].lpOverlapped == NULL);
    }
    result = 0;
cleanup:
    fixture_close(&fixture);
    return result;
}

int main(int argc, char **argv)
{
    WSADATA data;
    int result;

    if (argc != 2 || WSAStartup(MAKEWORD(2, 2), &data) != 0) return 2;
    if (strcmp(argv[1], "submit") == 0) result = test_submit_failure();
    else if (strcmp(argv[1], "busy-close") == 0) result = test_busy_closed();
    else if (strcmp(argv[1], "wake") == 0) result = test_wake(0);
    else if (strcmp(argv[1], "wake-failure") == 0) result = test_wake(1);
    else if (strcmp(argv[1], "wake-close") == 0) result = test_wake(2);
    else result = -1;
    WSACleanup();
    return result == 0 ? 0 : 1;
}
