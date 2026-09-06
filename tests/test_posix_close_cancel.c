#define _GNU_SOURCE

#include "wepoll_ex.h"

#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <time.h>

static sem_t entered_wait;
static sem_t native_returned;
static sem_t release_wait;
static sem_t close_waiting;
static int original_epfd;
static int probe_result;

int __real_epoll_wait(int epfd, struct epoll_event *events,
                      int maxevents, int timeout);
int __real_pthread_cond_wait(pthread_cond_t *condition,
                             pthread_mutex_t *mutex);

int __wrap_pthread_cond_wait(pthread_cond_t *condition,
                             pthread_mutex_t *mutex)
{
    sem_post(&close_waiting);
    return __real_pthread_cond_wait(condition, mutex);
}

int __wrap_epoll_wait(int epfd, struct epoll_event *events,
                      int maxevents, int timeout)
{
    sem_post(&entered_wait);
    int result = __real_epoll_wait(epfd, events, maxevents, timeout);
    int saved_errno = errno;
    sem_post(&native_returned);
    while (sem_wait(&release_wait) != 0 && errno == EINTR) {}
    errno = saved_errno;
    return result;
}

static void *waiter(void *opaque)
{
    (void)opaque;
    struct epoll_event_ex event;
    (void)epoll_wait_ex(original_epfd, &event, 1, -1);
    return NULL;
}

static void *closer(void *opaque)
{
    (void)opaque;
    (void)wepoll_close(original_epfd);
    return NULL;
}

static void *probe(void *opaque)
{
    (void)opaque;
    probe_result = epoll_create_ex(0, 0);
    if (probe_result >= 0 && wepoll_close(probe_result) != 0) {
        probe_result = -1;
    }
    return NULL;
}

static void wait_for_sem(sem_t *semaphore)
{
    while (sem_wait(semaphore) != 0 && errno == EINTR) {}
}

static int join_with_deadline(pthread_t thread, void **result)
{
    struct timespec deadline;
    if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) return errno;
    deadline.tv_sec += 2;
    return pthread_timedjoin_np(thread, result, &deadline);
}

int main(void)
{
    pthread_t wait_thread;
    pthread_t close_thread;
    pthread_t probe_thread;
    void *close_result = NULL;
    int epfd = -1;
    int wait_created = 0;
    int close_created = 0;
    int probe_created = 0;
    int result = 1;

    if (sem_init(&entered_wait, 0, 0) != 0 ||
        sem_init(&native_returned, 0, 0) != 0 ||
        sem_init(&release_wait, 0, 0) != 0 ||
        sem_init(&close_waiting, 0, 0) != 0) {
        perror("sem_init");
        return 1;
    }
    epfd = epoll_create_ex(0, 0);
    if (epfd < 0) {
        perror("epoll_create_ex");
        goto cleanup;
    }
    original_epfd = epfd;
    if (pthread_create(&wait_thread, NULL, waiter, NULL) != 0) goto cleanup;
    wait_created = 1;
    wait_for_sem(&entered_wait);
    if (pthread_create(&close_thread, NULL, closer, NULL) != 0) goto cleanup;
    close_created = 1;
    wait_for_sem(&native_returned);
    wait_for_sem(&close_waiting);

    if (pthread_cancel(close_thread) != 0 ||
        join_with_deadline(close_thread, &close_result) != 0 ||
        close_result != PTHREAD_CANCELED) {
        fprintf(stderr, "canceled close did not terminate cleanly\n");
        goto cleanup;
    }
    close_created = 0;
    sem_post(&release_wait);
    if (join_with_deadline(wait_thread, NULL) != 0) {
        fprintf(stderr, "blocked wait did not release\n");
        goto cleanup;
    }
    wait_created = 0;

    probe_result = -1;
    if (pthread_create(&probe_thread, NULL, probe, NULL) != 0) {
        fprintf(stderr, "probe thread creation failed\n");
        goto cleanup;
    }
    probe_created = 1;
    int probe_join = join_with_deadline(probe_thread, NULL);
    if (probe_join != 0 || probe_result < 0) {
        fprintf(stderr, "registry lock remained stranded after cancel\n");
        return 1;
    }
    probe_created = 0;
    result = 0;

cleanup:
    if (probe_created) pthread_join(probe_thread, NULL);
    if (close_created) {
        pthread_cancel(close_thread);
        pthread_join(close_thread, NULL);
    }
    if (wait_created) {
        sem_post(&release_wait);
        pthread_join(wait_thread, NULL);
    }
    sem_destroy(&close_waiting);
    sem_destroy(&release_wait);
    sem_destroy(&native_returned);
    sem_destroy(&entered_wait);
    return result;
}
