#include "event_loop.h"

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include <unistd.h>

#ifdef __linux__
#include <sys/epoll.h>
#else
#include <sys/event.h>
#include <sys/time.h>
#endif

#include "clock.h"
#include "mem.h"

enum {
    NO_TIMEOUT = -1,
    MS_PER_SEC = 1000,
    NS_PER_MS = 1000000,
};

/* 백엔드가 돌려준 이벤트를 fd 하나에 mask 하나로 맞춘 것 */
struct el_fired {
    int fd;
    int mask;
};

/* 불변식: watches[fd].handler 가 NULL 이 아니면 그 fd 는 백엔드에 등록돼 있다 */
struct event_loop {
    int backend_fd;
    struct el_watch *watches; /* fd 번호로 바로 찾는다 */
    size_t watch_cap;
    struct el_timer timer;
    bool has_timer;
    int64_t next_timer_ms;
    bool should_stop;
    struct el_fired fired[EL_MAX_EVENTS];
#ifdef __linux__
    struct epoll_event events[EL_MAX_EVENTS];
#else
    struct kevent events[EL_MAX_EVENTS];
#endif
};

/* 시그널 핸들러는 루프를 모르므로 전역 플래그만 세우고, run 이 매 바퀴 확인한다 */
static volatile sig_atomic_t g_stop_signal_received = 0;

static struct el_watch *find_watch(struct event_loop *loop, int fd);
static enum el_result reserve_watches(struct event_loop *loop, int fd);
static int compute_timeout_ms(const struct event_loop *loop);
static enum el_result poll_and_dispatch(struct event_loop *loop, int timeout_ms);
static void dispatch_fired(struct event_loop *loop, int fired_count);
static void run_timer_if_due(struct event_loop *loop);
static void handle_stop_signal(int signal_number);
static enum el_result backend_init(struct event_loop *loop);
static enum el_result backend_add(struct event_loop *loop, int fd, int mask);
static enum el_result backend_modify(struct event_loop *loop, const struct el_watch *watch,
                                     int new_mask);
static enum el_result backend_remove(struct event_loop *loop, const struct el_watch *watch);
static int backend_poll(struct event_loop *loop, int timeout_ms);

struct event_loop *event_loop_create(void)
{
    struct event_loop *loop = mem_calloc(1, sizeof(*loop));
    if (loop == NULL) {
        return NULL;
    }
    loop->backend_fd = -1;
    if (backend_init(loop) != EL_OK) {
        event_loop_destroy(loop);
        return NULL;
    }
    return loop;
}

void event_loop_destroy(struct event_loop *loop)
{
    if (loop == NULL) {
        return;
    }
    if (loop->backend_fd >= 0) {
        close(loop->backend_fd);
    }
    mem_free(loop->watches);
    mem_free(loop);
}

enum el_result event_loop_add(struct event_loop *loop, const struct el_watch *watch)
{
    if (find_watch(loop, watch->fd) != NULL) {
        return EL_ALREADY_EXISTS;
    }
    enum el_result result = reserve_watches(loop, watch->fd);
    if (result != EL_OK) {
        return result;
    }
    if (backend_add(loop, watch->fd, watch->mask) != EL_OK) {
        return EL_SYSTEM_ERROR;
    }
    loop->watches[watch->fd] = *watch;
    return EL_OK;
}

enum el_result event_loop_modify(struct event_loop *loop, int fd, int mask)
{
    struct el_watch *watch = find_watch(loop, fd);
    if (watch == NULL) {
        return EL_NOT_FOUND;
    }
    if (backend_modify(loop, watch, mask) != EL_OK) {
        return EL_SYSTEM_ERROR;
    }
    watch->mask = mask;
    return EL_OK;
}

enum el_result event_loop_remove(struct event_loop *loop, int fd)
{
    struct el_watch *watch = find_watch(loop, fd);
    if (watch == NULL) {
        return EL_NOT_FOUND;
    }
    enum el_result result = backend_remove(loop, watch);
    *watch = (struct el_watch){0};
    return result;
}

void event_loop_set_timer(struct event_loop *loop, const struct el_timer *timer)
{
    loop->timer = *timer;
    loop->has_timer = true;
    loop->next_timer_ms = clock_now_ms() + timer->interval_ms;
}

/* 한 바퀴: 다음 타이머까지 기다림 → 온 이벤트 처리 → 타이머 확인.
 * 시그널로 깨면 대기가 EINTR 로 끝나고 다음 바퀴 앞에서 플래그를 보고 멈춘다 */
enum el_result event_loop_run(struct event_loop *loop)
{
    while (!loop->should_stop) {
        if (g_stop_signal_received) {
            g_stop_signal_received = 0;
            break;
        }
        if (poll_and_dispatch(loop, compute_timeout_ms(loop)) != EL_OK) {
            return EL_SYSTEM_ERROR;
        }
    }
    loop->should_stop = false;
    return EL_OK;
}

enum el_result event_loop_run_once(struct event_loop *loop, int max_wait_ms)
{
    int timeout_ms = compute_timeout_ms(loop);
    if (timeout_ms == NO_TIMEOUT || timeout_ms > max_wait_ms) {
        timeout_ms = max_wait_ms;
    }
    return poll_and_dispatch(loop, timeout_ms);
}

void event_loop_stop(struct event_loop *loop)
{
    loop->should_stop = true;
}

/* SA_RESTART 를 넣지 않아야 epoll_wait · kevent 가 시그널에 EINTR 로 깨어난다 */
enum el_result event_loop_stop_on_signals(void)
{
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_stop_signal;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) != 0 || sigaction(SIGTERM, &action, NULL) != 0) {
        return EL_SYSTEM_ERROR;
    }
    return EL_OK;
}

static struct el_watch *find_watch(struct event_loop *loop, int fd)
{
    if (fd < 0 || (size_t)fd >= loop->watch_cap || loop->watches[fd].handler == NULL) {
        return NULL;
    }
    return &loop->watches[fd];
}

/* fd 번호가 들어갈 만큼 표를 2배씩 늘린다. 새 칸은 0(미등록)으로 채운다 */
static enum el_result reserve_watches(struct event_loop *loop, int fd)
{
    if (fd < 0) {
        return EL_SYSTEM_ERROR;
    }
    if ((size_t)fd < loop->watch_cap) {
        return EL_OK;
    }
    size_t new_cap = loop->watch_cap == 0 ? EL_INITIAL_WATCH_CAP : loop->watch_cap;
    while (new_cap <= (size_t)fd) {
        new_cap *= 2;
    }
    struct el_watch *new_watches = mem_calloc(new_cap, sizeof(*new_watches));
    if (new_watches == NULL) {
        return EL_OUT_OF_MEMORY;
    }
    if (loop->watch_cap > 0) {
        memcpy(new_watches, loop->watches, loop->watch_cap * sizeof(*new_watches));
    }
    mem_free(loop->watches);
    loop->watches = new_watches;
    loop->watch_cap = new_cap;
    return EL_OK;
}

/* 타이머가 없으면 이벤트가 올 때까지 기다리고, 있으면 다음 울릴 때까지만 기다린다 */
static int compute_timeout_ms(const struct event_loop *loop)
{
    if (!loop->has_timer) {
        return NO_TIMEOUT;
    }
    int64_t remaining_ms = loop->next_timer_ms - clock_now_ms();
    return remaining_ms > 0 ? (int)remaining_ms : 0;
}

static enum el_result poll_and_dispatch(struct event_loop *loop, int timeout_ms)
{
    int fired_count = backend_poll(loop, timeout_ms);
    if (fired_count < 0) {
        return EL_SYSTEM_ERROR;
    }
    dispatch_fired(loop, fired_count);
    run_timer_if_due(loop);
    return EL_OK;
}

/* handler 가 다른 fd 를 지웠을 수 있으므로 부르기 직전에 표를 다시 본다.
 * 복사본을 넘겨서 handler 가 표를 늘리거나 지워도 넘긴 watch 는 그대로다 */
static void dispatch_fired(struct event_loop *loop, int fired_count)
{
    for (int i = 0; i < fired_count && !loop->should_stop; i++) {
        const struct el_watch *watch = find_watch(loop, loop->fired[i].fd);
        if (watch == NULL) {
            continue;
        }
        int mask = loop->fired[i].mask & watch->mask;
        if (mask == 0) {
            continue;
        }
        struct el_watch copy = *watch;
        copy.handler(loop, &copy, mask);
    }
}

/* 오래 막혀 여러 번 놓쳤으면 따라잡으려고 연달아 부르지 않고 지금부터 다시 센다 */
static void run_timer_if_due(struct event_loop *loop)
{
    if (!loop->has_timer || loop->should_stop) {
        return;
    }
    int64_t now_ms = clock_now_ms();
    if (now_ms < loop->next_timer_ms) {
        return;
    }
    loop->next_timer_ms += loop->timer.interval_ms;
    if (loop->next_timer_ms <= now_ms) {
        loop->next_timer_ms = now_ms + loop->timer.interval_ms;
    }
    loop->timer.handler(loop, loop->timer.ctx);
}

static void handle_stop_signal(int signal_number)
{
    (void)signal_number;
    g_stop_signal_received = 1;
}

#ifdef __linux__

static uint32_t to_epoll_events(int mask)
{
    uint32_t events = 0;
    if (mask & EL_READABLE) {
        events |= EPOLLIN;
    }
    if (mask & EL_WRITABLE) {
        events |= EPOLLOUT;
    }
    return events;
}

/* target 의 fd 에 대해 op(ADD · MOD · DEL)를 적용한다. mask 는 바꾼 뒤 기다릴 이벤트다 */
static enum el_result epoll_control(struct event_loop *loop, int op, const struct el_fired *target)
{
    struct epoll_event event = {.events = to_epoll_events(target->mask), .data.fd = target->fd};
    return epoll_ctl(loop->backend_fd, op, target->fd, &event) == 0 ? EL_OK : EL_SYSTEM_ERROR;
}

static enum el_result backend_init(struct event_loop *loop)
{
    loop->backend_fd = epoll_create1(EPOLL_CLOEXEC);
    return loop->backend_fd >= 0 ? EL_OK : EL_SYSTEM_ERROR;
}

static enum el_result backend_add(struct event_loop *loop, int fd, int mask)
{
    const struct el_fired target = {.fd = fd, .mask = mask};
    return epoll_control(loop, EPOLL_CTL_ADD, &target);
}

static enum el_result backend_modify(struct event_loop *loop, const struct el_watch *watch,
                                     int new_mask)
{
    const struct el_fired target = {.fd = watch->fd, .mask = new_mask};
    return epoll_control(loop, EPOLL_CTL_MOD, &target);
}

static enum el_result backend_remove(struct event_loop *loop, const struct el_watch *watch)
{
    const struct el_fired target = {.fd = watch->fd, .mask = 0};
    return epoll_control(loop, EPOLL_CTL_DEL, &target);
}

/* 에러 · 끊김(EPOLLERR · EPOLLHUP)은 읽기와 쓰기 둘 다로 알려 handler 가 read · write 에서
 * 0 이나 에러를 보고 정리하게 한다 (Redis 와 같은 방식) */
static int backend_poll(struct event_loop *loop, int timeout_ms)
{
    int count = epoll_wait(loop->backend_fd, loop->events, EL_MAX_EVENTS, timeout_ms);
    if (count < 0) {
        return errno == EINTR ? 0 : -1;
    }
    for (int i = 0; i < count; i++) {
        uint32_t events = loop->events[i].events;
        int mask = 0;
        if (events & (EPOLLIN | EPOLLERR | EPOLLHUP)) {
            mask |= EL_READABLE;
        }
        if (events & (EPOLLOUT | EPOLLERR | EPOLLHUP)) {
            mask |= EL_WRITABLE;
        }
        loop->fired[i] = (struct el_fired){.fd = loop->events[i].data.fd, .mask = mask};
    }
    return count;
}

#else /* kqueue: macOS 빌드용 */

/* kqueue 는 읽기와 쓰기를 별도 필터로 등록하므로 mask 가 바뀐 필터만 넣고 뺀다 */
static enum el_result kqueue_apply(struct event_loop *loop, const struct el_watch *watch,
                                   int new_mask)
{
    int fd = watch->fd;
    int old_mask = watch->mask;
    struct kevent changes[2];
    int change_count = 0;
    static const int masks[] = {EL_READABLE, EL_WRITABLE};
    static const short filters[] = {EVFILT_READ, EVFILT_WRITE};
    for (int i = 0; i < 2; i++) {
        bool was_on = (old_mask & masks[i]) != 0;
        bool is_on = (new_mask & masks[i]) != 0;
        if (was_on != is_on) {
            EV_SET(&changes[change_count++], fd, filters[i], is_on ? EV_ADD : EV_DELETE, 0, 0,
                   NULL);
        }
    }
    if (change_count == 0) {
        return EL_OK;
    }
    return kevent(loop->backend_fd, changes, change_count, NULL, 0, NULL) == 0 ? EL_OK
                                                                               : EL_SYSTEM_ERROR;
}

static enum el_result backend_init(struct event_loop *loop)
{
    loop->backend_fd = kqueue();
    return loop->backend_fd >= 0 ? EL_OK : EL_SYSTEM_ERROR;
}

static enum el_result backend_add(struct event_loop *loop, int fd, int mask)
{
    const struct el_watch unregistered = {.fd = fd, .mask = 0};
    return kqueue_apply(loop, &unregistered, mask);
}

static enum el_result backend_modify(struct event_loop *loop, const struct el_watch *watch,
                                     int new_mask)
{
    return kqueue_apply(loop, watch, new_mask);
}

static enum el_result backend_remove(struct event_loop *loop, const struct el_watch *watch)
{
    return kqueue_apply(loop, watch, 0);
}

/* 같은 fd 의 읽기 · 쓰기는 이벤트 두 개로 온다. EV_EOF 는 그 필터의 이벤트로 그대로 알린다 */
static int backend_poll(struct event_loop *loop, int timeout_ms)
{
    struct timespec timeout = {.tv_sec = timeout_ms / MS_PER_SEC,
                               .tv_nsec = (long)(timeout_ms % MS_PER_SEC) * NS_PER_MS};
    const struct timespec *timeout_ptr = timeout_ms == NO_TIMEOUT ? NULL : &timeout;
    int count = kevent(loop->backend_fd, NULL, 0, loop->events, EL_MAX_EVENTS, timeout_ptr);
    if (count < 0) {
        return errno == EINTR ? 0 : -1;
    }
    for (int i = 0; i < count; i++) {
        int mask = loop->events[i].filter == EVFILT_READ ? EL_READABLE : EL_WRITABLE;
        loop->fired[i] = (struct el_fired){.fd = (int)loop->events[i].ident, .mask = mask};
    }
    return count;
}

#endif
