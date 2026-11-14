#ifndef MINI_REDIS_EVENT_LOOP_H
#define MINI_REDIS_EVENT_LOOP_H

#include <stdint.h>

enum {
    EL_READABLE = 1 << 0,
    EL_WRITABLE = 1 << 1,
    EL_MAX_EVENTS = 128,
    EL_INITIAL_WATCH_CAP = 64,
};

enum el_result {
    EL_OK = 0,
    EL_OUT_OF_MEMORY,
    EL_SYSTEM_ERROR, /* epoll / kqueue 호출 실패. errno 를 본다 */
    EL_NOT_FOUND,
    EL_ALREADY_EXISTS,
};

struct event_loop;
struct el_watch;

/* fd 에 이벤트가 오면 부른다. fired_mask 는 실제로 온 이벤트(EL_READABLE / EL_WRITABLE)다.
 * watch 는 등록 정보의 복사본이라 handler 안에서 add · modify · remove 를 해도 안전하다. */
typedef void el_file_handler_fn(struct event_loop *loop, const struct el_watch *watch,
                                int fired_mask);

/* 주기 타이머가 울리면 부른다. */
typedef void el_timer_handler_fn(struct event_loop *loop, void *ctx);

/* fd 하나의 등록 정보. mask 는 기다릴 이벤트이고 0 이면 등록만 해 두고 기다리지 않는다. */
struct el_watch {
    int fd;
    int mask;
    el_file_handler_fn *handler;
    void *ctx;
};

/* 주기 타이머. interval_ms 마다 handler 를 부른다. */
struct el_timer {
    int64_t interval_ms;
    el_timer_handler_fn *handler;
    void *ctx;
};

/* 빈 루프를 만든다. 실패하면 NULL, event_loop_destroy() 로 해제한다. */
struct event_loop *event_loop_create(void);

/* 루프를 해제한다. 등록된 fd 는 닫지 않는다. NULL 이면 아무것도 하지 않는다. */
void event_loop_destroy(struct event_loop *loop);

/* fd 를 등록한다. 이미 등록된 fd 면 EL_ALREADY_EXISTS. watch 는 복사해 둔다. */
enum el_result event_loop_add(struct event_loop *loop, const struct el_watch *watch);

/* 등록된 fd 가 기다릴 이벤트를 mask 로 바꾼다. 없으면 EL_NOT_FOUND. */
enum el_result event_loop_modify(struct event_loop *loop, int fd, int mask);

/* fd 등록을 지운다. fd 를 close 하기 전에 부른다. 없으면 EL_NOT_FOUND. */
enum el_result event_loop_remove(struct event_loop *loop, int fd);

/* 주기 타이머를 정한다(루프에 하나). 첫 호출은 지금부터 interval_ms 뒤다. */
void event_loop_set_timer(struct event_loop *loop, const struct el_timer *timer);

/* event_loop_stop() 이 불리거나 SIGINT · SIGTERM 을 받을 때까지 이벤트를 처리한다.
 * 멈추면 EL_OK, epoll / kqueue 가 실패하면 EL_SYSTEM_ERROR. */
enum el_result event_loop_run(struct event_loop *loop);

/* 지금 처리 중인 이벤트를 마친 뒤 run 이 반환하게 한다. handler 안에서 불러도 된다. */
void event_loop_stop(struct event_loop *loop);

/* SIGINT · SIGTERM 을 받으면 실행 중인 루프가 멈추게 한다. 서버 시작 시 한 번 부른다. */
enum el_result event_loop_stop_on_signals(void);

#endif
