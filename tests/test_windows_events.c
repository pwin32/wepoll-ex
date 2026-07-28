/*
 * test_windows_events.c -- focused Windows AFD/epoll mapping regressions.
 *
 * Modes are intentionally selectable so the CPU-spin regression can run in
 * its own CTest process:
 *
 *     test_wepoll_ex_windows_events mapping
 *     test_wepoll_ex_windows_events status
 *     test_wepoll_ex_windows_events fin
 *     test_wepoll_ex_windows_events spin
 */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif

#include "wepoll_ex_internal.h"

#ifdef _WIN32

#include <errno.h>
#include <stdio.h>
#include <string.h>

#ifndef STATUS_PORT_UNREACHABLE
#define STATUS_PORT_UNREACHABLE ((NTSTATUS)0xC000023F)
#endif

typedef struct tcp_pair {
    SOCKET client;
    SOCKET server;
} tcp_pair_t;

static int check_mask(const char *name, uint32_t actual, uint32_t expected)
{
    if (actual == expected) {
        return 0;
    }
    fprintf(stderr, "%s: expected 0x%08lx, got 0x%08lx\n",
            name, (unsigned long)expected, (unsigned long)actual);
    return -1;
}

static int check_protocol(const char *name, uint8_t actual, uint8_t expected)
{
    if (actual == expected) {
        return 0;
    }
    fprintf(stderr, "%s: expected protocol %u, got %u\n",
            name, (unsigned)expected, (unsigned)actual);
    return -1;
}

static void tcp_pair_init(tcp_pair_t *pair)
{
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
}

static int make_tcp_pair(tcp_pair_t *pair)
{
    SOCKET listener = INVALID_SOCKET;
    struct sockaddr_in address;
    int address_length = (int)sizeof(address);

    tcp_pair_init(pair);
    listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) {
        return -1;
    }

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(0);
    if (bind(listener, (const struct sockaddr *)&address,
             (int)sizeof(address)) == SOCKET_ERROR ||
        listen(listener, 1) == SOCKET_ERROR ||
        getsockname(listener, (struct sockaddr *)&address,
                    &address_length) == SOCKET_ERROR) {
        closesocket(listener);
        return -1;
    }

    pair->client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (pair->client == INVALID_SOCKET ||
        connect(pair->client, (const struct sockaddr *)&address,
                (int)sizeof(address)) == SOCKET_ERROR) {
        closesocket(listener);
        tcp_pair_close(pair);
        return -1;
    }

    pair->server = accept(listener, NULL, NULL);
    closesocket(listener);
    if (pair->server == INVALID_SOCKET) {
        tcp_pair_close(pair);
        return -1;
    }
    return 0;
}

static int add_socket(int epfd, SOCKET socket, uint32_t events,
                      uint64_t data)
{
    struct epoll_event event;

    memset(&event, 0, sizeof(event));
    event.events = events;
    event.data.u64 = data;
    return epoll_ctl(epfd, EPOLL_CTL_ADD, socket, &event);
}

static ep_sock_t *find_registered_socket(ep_port_t *port, SOCKET fd)
{
    ep_sock_t *result = NULL;

    pthread_mutex_lock(&port->fd_table_lock);
    for (size_t i = 0; i < port->fd_table_size; i++) {
        ep_sock_t *sock = port->fd_table[i];

        if (sock != NULL && sock->fd == fd) {
            result = sock;
            break;
        }
    }
    pthread_mutex_unlock(&port->fd_table_lock);
    return result;
}

static NTSTATUS NTAPI submit_pending_stub(
    HANDLE file_handle,
    HANDLE event,
    PIO_APC_ROUTINE apc_routine,
    PVOID apc_context,
    PIO_STATUS_BLOCK io_status_block,
    ULONG io_control_code,
    PVOID input_buffer,
    ULONG input_buffer_length,
    PVOID output_buffer,
    ULONG output_buffer_length)
{
    (void)file_handle;
    (void)event;
    (void)apc_routine;
    (void)apc_context;
    (void)io_status_block;
    (void)io_control_code;
    (void)input_buffer;
    (void)input_buffer_length;
    (void)output_buffer;
    (void)output_buffer_length;
    return STATUS_PENDING;
}

static SOCKET make_udp_socket(void)
{
    struct sockaddr_in address;
    SOCKET socket_fd;

    socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_fd == INVALID_SOCKET) {
        return INVALID_SOCKET;
    }
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(0);
    if (bind(socket_fd, (const struct sockaddr *)&address,
             (int)sizeof(address)) == SOCKET_ERROR) {
        closesocket(socket_fd);
        return INVALID_SOCKET;
    }
    return socket_fd;
}

static SOCKET make_tcp_listener_socket(void)
{
    struct sockaddr_in address;
    SOCKET socket_fd;

    socket_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_fd == INVALID_SOCKET) {
        return INVALID_SOCKET;
    }
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(0);
    if (bind(socket_fd, (const struct sockaddr *)&address,
             (int)sizeof(address)) == SOCKET_ERROR ||
        listen(socket_fd, 1) == SOCKET_ERROR) {
        closesocket(socket_fd);
        return INVALID_SOCKET;
    }
    return socket_fd;
}

static int test_protocol_metadata(void)
{
    WSAPROTOCOL_INFOW protocol_info;

    memset(&protocol_info, 0, sizeof(protocol_info));
    protocol_info.iAddressFamily = AF_INET;
    protocol_info.iSocketType = SOCK_DGRAM;
    protocol_info.iProtocol = IPPROTO_UDP;
    if (check_protocol("UDP IPv4 metadata",
                       ep_socket_protocol_from_info(
                           &protocol_info, (int)sizeof(protocol_info)),
                       EP_SOCKET_PROTOCOL_UDP) != 0) {
        return -1;
    }

    protocol_info.iAddressFamily = AF_INET6;
    if (check_protocol("UDP IPv6 metadata",
                       ep_socket_protocol_from_info(
                           &protocol_info, (int)sizeof(protocol_info)),
                       EP_SOCKET_PROTOCOL_UDP) != 0 ||
        check_mask("UDP IPv6 metadata abort",
                   ep_afd_to_epoll_events(
                       AFD_POLL_ABORT, STATUS_SUCCESS,
                       ep_socket_protocol_from_info(
                           &protocol_info, (int)sizeof(protocol_info))),
                   EPOLLERR) != 0) {
        return -1;
    }

    protocol_info.iProtocolMaxOffset = 1;
    if (check_protocol("UDP protocol range metadata",
                       ep_socket_protocol_from_info(
                           &protocol_info, (int)sizeof(protocol_info)),
                       EP_SOCKET_PROTOCOL_UNKNOWN) != 0) {
        return -1;
    }

    protocol_info.iProtocolMaxOffset = 0;
    protocol_info.iAddressFamily = AF_UNSPEC;
    if (check_protocol("UDP wrong-family metadata",
                       ep_socket_protocol_from_info(
                           &protocol_info, (int)sizeof(protocol_info)),
                       EP_SOCKET_PROTOCOL_UNKNOWN) != 0) {
        return -1;
    }

    protocol_info.iAddressFamily = AF_INET;
    protocol_info.iSocketType = SOCK_STREAM;
    protocol_info.iProtocol = IPPROTO_UDP;
    if (check_protocol("UDP wrong-type metadata",
                       ep_socket_protocol_from_info(
                           &protocol_info, (int)sizeof(protocol_info)),
                       EP_SOCKET_PROTOCOL_UNKNOWN) != 0) {
        return -1;
    }

    protocol_info.iSocketType = SOCK_DGRAM;
    protocol_info.iProtocol = IPPROTO_TCP;
    if (check_protocol("UDP wrong-protocol metadata",
                       ep_socket_protocol_from_info(
                           &protocol_info, (int)sizeof(protocol_info)),
                       EP_SOCKET_PROTOCOL_UNKNOWN) != 0 ||
        check_protocol("missing protocol metadata",
                       ep_socket_protocol_from_info(NULL, 0),
                       EP_SOCKET_PROTOCOL_UNKNOWN) != 0 ||
        check_protocol("short protocol metadata",
                       ep_socket_protocol_from_info(
                           &protocol_info, (int)sizeof(protocol_info) - 1),
                       EP_SOCKET_PROTOCOL_UNKNOWN) != 0) {
        return -1;
    }

    puts("protocol metadata: OK");
    return 0;
}

static int test_mapping(void)
{
    const uint32_t always_afd =
        AFD_POLL_ABORT | AFD_POLL_CONNECT_FAIL | AFD_POLL_LOCAL_CLOSE;
    const uint32_t all_epoll =
        EPOLLIN | EPOLLPRI | EPOLLOUT | EPOLLERR | EPOLLHUP |
        EPOLLRDNORM | EPOLLRDBAND | EPOLLWRNORM | EPOLLWRBAND |
        EPOLLRDHUP;
    const ULONG all_afd =
        AFD_POLL_RECEIVE | AFD_POLL_RECEIVE_EXPEDITED | AFD_POLL_SEND |
        AFD_POLL_DISCONNECT | AFD_POLL_ABORT | AFD_POLL_LOCAL_CLOSE |
        AFD_POLL_ACCEPT | AFD_POLL_CONNECT_FAIL;

    if (test_protocol_metadata() != 0 ||
        check_mask("AFD receive",
                   ep_afd_to_epoll_events(AFD_POLL_RECEIVE,
                                          STATUS_SUCCESS,
                                          EP_SOCKET_PROTOCOL_UNKNOWN),
                   EPOLLIN | EPOLLRDNORM) != 0 ||
        check_mask("AFD accept",
                   ep_afd_to_epoll_events(AFD_POLL_ACCEPT,
                                          STATUS_SUCCESS,
                                          EP_SOCKET_PROTOCOL_UNKNOWN),
                   EPOLLIN | EPOLLRDNORM) != 0 ||
        check_mask("AFD expedited",
                   ep_afd_to_epoll_events(AFD_POLL_RECEIVE_EXPEDITED,
                                          STATUS_SUCCESS,
                                          EP_SOCKET_PROTOCOL_UNKNOWN),
                   EPOLLPRI | EPOLLRDBAND) != 0 ||
        check_mask("AFD send",
                   ep_afd_to_epoll_events(AFD_POLL_SEND,
                                          STATUS_SUCCESS,
                                          EP_SOCKET_PROTOCOL_UNKNOWN),
                   EPOLLOUT | EPOLLWRNORM | EPOLLWRBAND) != 0 ||
        check_mask("AFD disconnect",
                   ep_afd_to_epoll_events(AFD_POLL_DISCONNECT,
                                          STATUS_SUCCESS,
                                          EP_SOCKET_PROTOCOL_UNKNOWN),
                   EPOLLIN | EPOLLRDNORM | EPOLLRDHUP) != 0 ||
        check_mask("AFD abort unknown",
                   ep_afd_to_epoll_events(AFD_POLL_ABORT,
                                          STATUS_SUCCESS,
                                          EP_SOCKET_PROTOCOL_UNKNOWN),
                   EPOLLERR | EPOLLHUP) != 0 ||
        check_mask("AFD abort UDP",
                   ep_afd_to_epoll_events(AFD_POLL_ABORT,
                                          STATUS_SUCCESS,
                                          EP_SOCKET_PROTOCOL_UDP),
                   EPOLLERR) != 0 ||
        check_mask("AFD connect failure",
                   ep_afd_to_epoll_events(AFD_POLL_CONNECT_FAIL,
                                          STATUS_SUCCESS,
                                          EP_SOCKET_PROTOCOL_UDP),
                   EPOLLIN | EPOLLOUT | EPOLLERR | EPOLLHUP | EPOLLRDNORM |
                       EPOLLWRNORM | EPOLLRDHUP) != 0 ||
        check_mask("AFD local close",
                   ep_afd_to_epoll_events(AFD_POLL_LOCAL_CLOSE,
                                          STATUS_SUCCESS,
                                          EP_SOCKET_PROTOCOL_UDP),
                   EPOLLHUP) != 0 ||
        check_mask("AFD UDP abort plus local close",
                   ep_afd_to_epoll_events(AFD_POLL_ABORT |
                                              AFD_POLL_LOCAL_CLOSE,
                                          STATUS_SUCCESS,
                                          EP_SOCKET_PROTOCOL_UDP),
                   EPOLLERR | EPOLLHUP) != 0 ||
        check_mask("AFD status error",
                   ep_afd_to_epoll_events(0, STATUS_PORT_UNREACHABLE,
                                          EP_SOCKET_PROTOCOL_UNKNOWN),
                   EPOLLERR) != 0 ||
        check_mask("AFD abort status error",
                   ep_afd_to_epoll_events(AFD_POLL_ABORT,
                                          STATUS_PORT_UNREACHABLE,
                                          EP_SOCKET_PROTOCOL_UDP),
                   EPOLLERR) != 0 ||
        check_mask("AFD combined",
                   ep_afd_to_epoll_events(all_afd, STATUS_SUCCESS,
                                          EP_SOCKET_PROTOCOL_UDP),
                   all_epoll) != 0) {
        return -1;
    }

    if (check_mask("epoll zero", ep_epoll_to_afd_events(0),
                   always_afd) != 0 ||
        check_mask("epoll IN", ep_epoll_to_afd_events(EPOLLIN),
                   always_afd | AFD_POLL_RECEIVE | AFD_POLL_ACCEPT |
                       AFD_POLL_DISCONNECT) != 0 ||
        check_mask("epoll RDNORM", ep_epoll_to_afd_events(EPOLLRDNORM),
                   always_afd | AFD_POLL_RECEIVE | AFD_POLL_ACCEPT |
                       AFD_POLL_DISCONNECT) != 0 ||
        check_mask("epoll RDHUP", ep_epoll_to_afd_events(EPOLLRDHUP),
                   always_afd | AFD_POLL_DISCONNECT) != 0 ||
        check_mask("epoll PRI", ep_epoll_to_afd_events(EPOLLPRI),
                   always_afd | AFD_POLL_RECEIVE_EXPEDITED) != 0 ||
        check_mask("epoll RDBAND", ep_epoll_to_afd_events(EPOLLRDBAND),
                   always_afd | AFD_POLL_RECEIVE_EXPEDITED) != 0 ||
        check_mask("epoll OUT", ep_epoll_to_afd_events(EPOLLOUT),
                   always_afd | AFD_POLL_SEND) != 0 ||
        check_mask("epoll ERR", ep_epoll_to_afd_events(EPOLLERR),
                   always_afd) != 0 ||
        check_mask("epoll HUP", ep_epoll_to_afd_events(EPOLLHUP),
                   always_afd) != 0) {
        return -1;
    }

    puts("mapping: OK");
    return 0;
}

typedef SOCKET (*socket_factory_fn)(void);

static int run_status_delivery_case(const char *name,
                                    socket_factory_fn socket_factory,
                                    uint8_t expected_protocol,
                                    int force_unknown_protocol,
                                    uint32_t interest, ULONG afd_events,
                                    NTSTATUS afd_status, uint32_t expected,
                                    uint64_t expected_data)
{
    ep_port_t *port = NULL;
    ep_sock_t *sock = NULL;
    epoll_data_t data;
    epoll_event_ex ignored;
    epoll_event_ex output;
    PNtDeviceIoControlFile original_submit = NULL;
    SOCKET socket_fd = INVALID_SOCKET;
    int submit_stub_installed = 0;
    int completion_posted = 0;
    int synthetic_pending = 0;
    int registered = 0;
    int result = -1;
    int wait_result;

    memset(&data, 0, sizeof(data));
    data.u64 = expected_data;
    socket_fd = socket_factory();
    if (socket_fd == INVALID_SOCKET || ep_port_create(0, 0, &port) != 0) {
        fprintf(stderr, "%s: setup failed, errno=%d WSA=%d\n",
                name, errno, WSAGetLastError());
        goto cleanup;
    }

    original_submit = g_ntdll.NtDeviceIoControlFile;
    g_ntdll.NtDeviceIoControlFile = submit_pending_stub;
    submit_stub_installed = 1;
    if (ep_port_register(port, socket_fd, interest, 0, data, NULL) != 0) {
        fprintf(stderr, "%s: registration failed, errno=%d WSA=%d\n",
                name, errno, WSAGetLastError());
        goto cleanup;
    }
    registered = 1;
    memset(&ignored, 0, sizeof(ignored));
    if (ep_port_wait(port, &ignored, 1, 0, NULL) < 0) {
        fprintf(stderr, "%s: deferred arm failed, errno=%d\n", name, errno);
        goto cleanup;
    }
    synthetic_pending = 1;
    g_ntdll.NtDeviceIoControlFile = original_submit;
    submit_stub_installed = 0;

    sock = find_registered_socket(port, socket_fd);
    if (sock == NULL) {
        fprintf(stderr, "%s: registration lookup failed\n", name);
        goto cleanup;
    }
    pthread_mutex_lock(&port->fd_table_lock);
    if (sock->socket_protocol != expected_protocol) {
        fprintf(stderr, "%s: cached protocol expected %u, got %u\n",
                name, (unsigned)expected_protocol,
                (unsigned)sock->socket_protocol);
        pthread_mutex_unlock(&port->fd_table_lock);
        goto cleanup;
    }
    if (force_unknown_protocol) {
        sock->socket_protocol = EP_SOCKET_PROTOCOL_UNKNOWN;
    }
    sock->afd_info->NumberOfHandles = 1;
    sock->afd_info->Handles[0].Events = afd_events;
    sock->afd_info->Handles[0].Status = afd_status;
    sock->io_status_block.Status = STATUS_SUCCESS;
    pthread_mutex_unlock(&port->fd_table_lock);
    if (!PostQueuedCompletionStatus(port->iocp, 0, 0,
                                    (OVERLAPPED *)&sock->io_status_block)) {
        fprintf(stderr, "%s: completion post failed, WSA=%lu\n",
                name, (unsigned long)GetLastError());
        goto cleanup;
    }
    completion_posted = 1;
    synthetic_pending = 0;

    memset(&output, 0, sizeof(output));
    wait_result = ep_port_wait(port, &output, 1, 1000, NULL);
    completion_posted = 0;
    if (wait_result != 1 || output.data.u64 != expected_data ||
        check_mask(name, output.events, expected) != 0) {
        fprintf(stderr,
                "%s: wait=%d errno=%d data=0x%llx events=0x%08lx\n",
                name, wait_result, errno,
                (unsigned long long)output.data.u64,
                (unsigned long)output.events);
        goto cleanup;
    }
    result = 0;

cleanup:
    if (submit_stub_installed) {
        g_ntdll.NtDeviceIoControlFile = original_submit;
    }
    if (port != NULL) {
        if (sock == NULL && socket_fd != INVALID_SOCKET) {
            sock = find_registered_socket(port, socket_fd);
        }
        if (sock != NULL && synthetic_pending) {
            sock->io_status_block.Status = STATUS_CANCELLED;
            if (PostQueuedCompletionStatus(
                    port->iocp, 0, 0,
                    (OVERLAPPED *)&sock->io_status_block)) {
                completion_posted = 1;
                synthetic_pending = 0;
            }
        }
        if (sock != NULL && completion_posted) {
            memset(&output, 0, sizeof(output));
            (void)ep_port_wait(port, &output, 1, 1000, NULL);
        }
        if (registered) {
            (void)ep_port_unregister(port, socket_fd);
        }
        (void)ep_port_destroy(port);
    }
    if (socket_fd != INVALID_SOCKET) {
        closesocket(socket_fd);
    }
    return result;
}

static int test_status_delivery(void)
{
    if (run_status_delivery_case("status-only error", make_udp_socket,
                                 EP_SOCKET_PROTOCOL_UDP, 0, EPOLLIN, 0,
                                 STATUS_PORT_UNREACHABLE, EPOLLERR,
                                 UINT64_C(0x3001)) != 0 ||
        run_status_delivery_case("UDP IPv4 abort", make_udp_socket,
                                 EP_SOCKET_PROTOCOL_UDP, 0, EPOLLPRI,
                                 AFD_POLL_ABORT, STATUS_SUCCESS,
                                 EPOLLERR,
                                 UINT64_C(0x3002)) != 0 ||
        run_status_delivery_case("TCP abort", make_tcp_listener_socket,
                                 EP_SOCKET_PROTOCOL_UNKNOWN, 0, EPOLLPRI,
                                 AFD_POLL_ABORT, STATUS_SUCCESS,
                                 EPOLLERR | EPOLLHUP,
                                 UINT64_C(0x3003)) != 0 ||
        run_status_delivery_case("unknown protocol abort", make_udp_socket,
                                 EP_SOCKET_PROTOCOL_UDP, 1, EPOLLPRI,
                                 AFD_POLL_ABORT, STATUS_SUCCESS,
                                 EPOLLERR | EPOLLHUP,
                                 UINT64_C(0x3004)) != 0 ||
        run_status_delivery_case("connect failure", make_udp_socket,
                                 EP_SOCKET_PROTOCOL_UDP, 0, EPOLLOUT,
                                 AFD_POLL_CONNECT_FAIL, STATUS_SUCCESS,
                                 EPOLLOUT | EPOLLERR | EPOLLHUP,
                                 UINT64_C(0x3005)) != 0 ||
        run_status_delivery_case("connect failure filtered",
                                 make_udp_socket, EP_SOCKET_PROTOCOL_UDP, 0,
                                 EPOLLPRI,
                                 AFD_POLL_CONNECT_FAIL, STATUS_SUCCESS,
                                 EPOLLERR | EPOLLHUP,
                                 UINT64_C(0x3006)) != 0 ||
        run_status_delivery_case("read plus status", make_udp_socket,
                                 EP_SOCKET_PROTOCOL_UDP, 0, EPOLLIN,
                                 AFD_POLL_RECEIVE,
                                 STATUS_PORT_UNREACHABLE,
                                 EPOLLIN | EPOLLERR,
                                 UINT64_C(0x3007)) != 0) {
        return -1;
    }
    puts("status: OK");
    return 0;
}

static int run_fin_case(uint32_t requested, uint32_t expected,
                        uint64_t data, const char *name)
{
    tcp_pair_t pair;
    struct epoll_event output;
    char byte;
    int epfd = -1;
    int result = -1;

    if (make_tcp_pair(&pair) != 0) {
        fprintf(stderr, "%s: make_tcp_pair failed, WSA=%d\n",
                name, WSAGetLastError());
        return -1;
    }
    epfd = epoll_create1(0);
    if (epfd < 0 || add_socket(epfd, pair.server, requested, data) != 0 ||
        shutdown(pair.client, SD_SEND) == SOCKET_ERROR) {
        fprintf(stderr, "%s: setup failed, errno=%d WSA=%d\n",
                name, errno, WSAGetLastError());
        goto cleanup;
    }

    if (epoll_wait(epfd, &output, 1, 1500) != 1) {
        fprintf(stderr, "%s: wait did not deliver, errno=%d\n", name, errno);
        goto cleanup;
    }
    if (check_mask(name, output.events, expected) != 0 ||
        output.data.u64 != data) {
        fprintf(stderr, "%s: event data mismatch\n", name);
        goto cleanup;
    }
    if (recv(pair.server, &byte, 1, 0) != 0) {
        fprintf(stderr, "%s: EOF recv failed, WSA=%d\n",
                name, WSAGetLastError());
        goto cleanup;
    }
    result = 0;

cleanup:
    if (epfd >= 0) {
        (void)wepoll_close(epfd);
    }
    tcp_pair_close(&pair);
    return result;
}

static int test_fin_filters(void)
{
    tcp_pair_t pair;
    struct epoll_event output;
    int epfd = -1;
    int result = -1;

    if (run_fin_case(EPOLLIN, EPOLLIN, UINT64_C(0x1001),
                     "FIN IN-only") != 0 ||
        run_fin_case(EPOLLRDHUP, EPOLLRDHUP, UINT64_C(0x1002),
                     "FIN RDHUP-only") != 0 ||
        run_fin_case(EPOLLIN | EPOLLRDHUP, EPOLLIN | EPOLLRDHUP,
                     UINT64_C(0x1003), "FIN IN+RDHUP") != 0) {
        return -1;
    }

    if (make_tcp_pair(&pair) != 0) {
        fprintf(stderr, "FIN PRI-only: make_tcp_pair failed, WSA=%d\n",
                WSAGetLastError());
        return -1;
    }
    epfd = epoll_create1(0);
    if (epfd < 0 ||
        add_socket(epfd, pair.server, EPOLLPRI, UINT64_C(0x1004)) != 0 ||
        shutdown(pair.client, SD_SEND) == SOCKET_ERROR) {
        fprintf(stderr, "FIN PRI-only: setup failed, errno=%d WSA=%d\n",
                errno, WSAGetLastError());
        goto cleanup;
    }
    if (epoll_wait(epfd, &output, 1, 150) != 0 ||
        epoll_fd_count(epfd) != 1) {
        fprintf(stderr, "FIN PRI-only: unexpected event or registration loss\n");
        goto cleanup;
    }
    result = 0;

cleanup:
    if (epfd >= 0) {
        (void)wepoll_close(epfd);
    }
    tcp_pair_close(&pair);
    if (result == 0) {
        puts("fin: OK");
    }
    return result;
}

static uint64_t filetime_to_u64(FILETIME value)
{
    ULARGE_INTEGER converted;

    converted.LowPart = value.dwLowDateTime;
    converted.HighPart = value.dwHighDateTime;
    return converted.QuadPart;
}

static int process_cpu_100ns(uint64_t *cpu_time)
{
    FILETIME create_time;
    FILETIME exit_time;
    FILETIME kernel_time;
    FILETIME user_time;

    if (!GetProcessTimes(GetCurrentProcess(), &create_time, &exit_time,
                         &kernel_time, &user_time)) {
        return -1;
    }
    *cpu_time = filetime_to_u64(kernel_time) + filetime_to_u64(user_time);
    return 0;
}

static int test_fin_spin(void)
{
    enum { WAIT_MS = 350, MAX_CPU_MS = 120 };
    tcp_pair_t pair;
    struct epoll_event output;
    uint64_t cpu_before;
    uint64_t cpu_after;
    uint64_t wall_before;
    uint64_t wall_after;
    int epfd = -1;
    int result = -1;

    if (make_tcp_pair(&pair) != 0) {
        fprintf(stderr, "spin: make_tcp_pair failed, WSA=%d\n",
                WSAGetLastError());
        return -1;
    }
    epfd = epoll_create1(0);
    if (epfd < 0 || add_socket(epfd, pair.server, EPOLLPRI,
                               UINT64_C(0x2001)) != 0 ||
        shutdown(pair.client, SD_SEND) == SOCKET_ERROR ||
        process_cpu_100ns(&cpu_before) != 0) {
        fprintf(stderr, "spin: setup failed, errno=%d WSA=%d\n",
                errno, WSAGetLastError());
        goto cleanup;
    }

    wall_before = GetTickCount64();
    if (epoll_wait(epfd, &output, 1, WAIT_MS) != 0 ||
        process_cpu_100ns(&cpu_after) != 0) {
        fprintf(stderr, "spin: wait failed or delivered an event, errno=%d\n",
                errno);
        goto cleanup;
    }
    wall_after = GetTickCount64();

    {
        uint64_t wall_ms = wall_after - wall_before;
        uint64_t cpu_ms = (cpu_after - cpu_before) / UINT64_C(10000);

        if (wall_ms + 25 < WAIT_MS || wall_ms > UINT64_C(2000) ||
            cpu_ms > MAX_CPU_MS) {
            fprintf(stderr,
                    "spin: wall=%llu ms CPU=%llu ms (limit %d ms)\n",
                    (unsigned long long)wall_ms,
                    (unsigned long long)cpu_ms, MAX_CPU_MS);
            goto cleanup;
        }
    }
    result = 0;

cleanup:
    if (epfd >= 0) {
        (void)wepoll_close(epfd);
    }
    tcp_pair_close(&pair);
    if (result == 0) {
        puts("spin: OK");
    }
    return result;
}

static int run_mode(const char *mode)
{
    if (strcmp(mode, "mapping") == 0) {
        return test_mapping();
    }
    if (strcmp(mode, "status") == 0) {
        return test_status_delivery();
    }
    if (strcmp(mode, "fin") == 0) {
        return test_fin_filters();
    }
    if (strcmp(mode, "spin") == 0) {
        return test_fin_spin();
    }
    if (strcmp(mode, "all") == 0) {
        int failed = 0;

        failed |= test_mapping() != 0;
        failed |= test_status_delivery() != 0;
        failed |= test_fin_filters() != 0;
        failed |= test_fin_spin() != 0;
        return failed ? -1 : 0;
    }
    fprintf(stderr,
            "usage: test_windows_events [mapping|status|fin|spin|all]\n");
    return -1;
}

int main(int argc, char **argv)
{
    WSADATA wsa_data;
    const char *mode = argc == 1 ? "all" : argv[1];
    int result;

    if (argc > 2) {
        fprintf(stderr,
                "usage: test_windows_events [mapping|status|fin|spin|all]\n");
        return 2;
    }
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        return 2;
    }
    result = run_mode(mode);
    WSACleanup();
    return result == 0 ? 0 : 1;
}

#else

int main(void)
{
    return 0;
}

#endif
