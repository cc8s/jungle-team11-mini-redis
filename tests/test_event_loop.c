#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <fcntl.h>
#include <unistd.h>

#include "clock.h"
#include "event_loop.h"
#include "mem.h"
#include "test.h"

enum {
    SHORT_RUN_MS = 50,
    SAFETY_TIMEOUT_MS = 1000,
    TIMER_INTERVAL_MS = 50,
    TIMER_RUN_MS = 200,
    EXPECTED_TIMER_CALLS = TIMER_RUN_MS / TIMER_INTERVAL_MS,
    READ_BUF_BYTES = 64,
    HIGH_FD = 200,
};

/* handler 가 무엇을 받았는지 기록한다 */
struct record {
    int read_count;
    int write_count;
    int last_mask;
    int other_fd; /* 0 보다 크면 handler 가 이 fd 를 지운다 */
    bool should_remove_self;
    bool should_stop;
    bool should_stop_writing;
};

/* 타이머 테스트와 멈춤 테스트용 */
struct timer_record {
    int call_count;
    int64_t start_ms;
    int64_t stop_after_ms;
    bool should_raise_sigint;
    bool did_time_out;
};

struct pipe_pair {
    int read_fd;
    int write_fd;
};

static struct pipe_pair open_pipe(void)
{
    int fds[2];
    CHECK(pipe(fds) == 0);
    return (struct pipe_pair){.read_fd = fds[0], .write_fd = fds[1]};
}

static void close_pipe(struct pipe_pair *p)
{
    close(p->read_fd);
    close(p->write_fd);
}

static void write_byte(int fd)
{
    CHECK(write(fd, "x", 1) == 1);
}

static void record_handler(struct event_loop *loop, const struct el_watch *watch, int fired_mask)
{
    struct record *rec = watch->ctx;
    rec->last_mask = fired_mask;
    if (fired_mask & EL_READABLE) {
        char buf[READ_BUF_BYTES];
        CHECK(read(watch->fd, buf, sizeof(buf)) > 0);
        rec->read_count++;
    }
    if (fired_mask & EL_WRITABLE) {
        rec->write_count++;
    }
    if (rec->should_stop_writing) {
        CHECK(event_loop_modify(loop, watch->fd, 0) == EL_OK);
    }
    if (rec->should_remove_self) {
        CHECK(event_loop_remove(loop, watch->fd) == EL_OK);
    }
    if (rec->other_fd > 0) {
        event_loop_remove(loop, rec->other_fd);
    }
    if (rec->should_stop) {
        event_loop_stop(loop);
    }
}

static void add_watch(struct event_loop *loop, int fd, int mask, struct record *rec)
{
    const struct el_watch watch = {.fd = fd, .mask = mask, .handler = record_handler, .ctx = rec};
    CHECK(event_loop_add(loop, &watch) == EL_OK);
}

static void stop_timer_handler(struct event_loop *loop, void *ctx)
{
    struct timer_record *rec = ctx;
    rec->did_time_out = true;
    event_loop_stop(loop);
}

/* interval_ms 뒤에 루프를 멈추는 타이머를 걸고 실행한다 */
static void run_for_ms(struct event_loop *loop, int64_t interval_ms, struct timer_record *rec)
{
    const struct el_timer timer = {
        .interval_ms = interval_ms, .handler = stop_timer_handler, .ctx = rec};
    event_loop_set_timer(loop, &timer);
    CHECK(event_loop_run(loop) == EL_OK);
}

static void test_pipe_write_calls_read_handler_once(void)
{
    struct event_loop *loop = event_loop_create();
    struct pipe_pair p = open_pipe();
    struct record rec = {0};
    struct timer_record timer_rec = {0};
    add_watch(loop, p.read_fd, EL_READABLE, &rec);
    write_byte(p.write_fd);

    run_for_ms(loop, SHORT_RUN_MS, &timer_rec);
    CHECK(rec.read_count == 1);
    CHECK(rec.last_mask == EL_READABLE);
    event_loop_destroy(loop);
    close_pipe(&p);
}

static void test_removed_fd_is_not_dispatched(void)
{
    struct event_loop *loop = event_loop_create();
    struct pipe_pair p = open_pipe();
    struct record rec = {0};
    struct timer_record timer_rec = {0};
    add_watch(loop, p.read_fd, EL_READABLE, &rec);
    CHECK(event_loop_remove(loop, p.read_fd) == EL_OK);
    write_byte(p.write_fd);

    run_for_ms(loop, SHORT_RUN_MS, &timer_rec);
    CHECK(rec.read_count == 0);
    event_loop_destroy(loop);
    close_pipe(&p);
}

/* 쓰기 끝은 늘 쓸 수 있어 계속 울리므로, 한 번 받으면 handler 가 다시 끈다 */
static void test_modify_adds_write_events(void)
{
    struct event_loop *loop = event_loop_create();
    struct pipe_pair p = open_pipe();
    struct record rec = {.should_stop_writing = true};
    struct timer_record timer_rec = {0};
    add_watch(loop, p.write_fd, 0, &rec);
    CHECK(event_loop_modify(loop, p.write_fd, EL_WRITABLE) == EL_OK);

    run_for_ms(loop, SHORT_RUN_MS, &timer_rec);
    CHECK(rec.write_count == 1);
    CHECK(rec.last_mask == EL_WRITABLE);
    event_loop_destroy(loop);
    close_pipe(&p);
}

static void test_handler_removing_own_fd_is_safe(void)
{
    struct event_loop *loop = event_loop_create();
    struct pipe_pair p = open_pipe();
    struct record rec = {.should_remove_self = true};
    struct timer_record timer_rec = {0};
    add_watch(loop, p.read_fd, EL_READABLE, &rec);
    write_byte(p.write_fd);
    write_byte(p.write_fd);

    run_for_ms(loop, SHORT_RUN_MS, &timer_rec);
    CHECK(rec.read_count == 1);
    CHECK(event_loop_remove(loop, p.read_fd) == EL_NOT_FOUND);
    event_loop_destroy(loop);
    close_pipe(&p);
}

/* 두 fd 가 같은 바퀴에 준비돼도, 먼저 불린 handler 가 다른 쪽을 지우면 그쪽은 불리지 않는다 */
static void test_fd_removed_by_earlier_handler_is_skipped(void)
{
    struct event_loop *loop = event_loop_create();
    struct pipe_pair a = open_pipe();
    struct pipe_pair b = open_pipe();
    struct record rec_a = {.other_fd = b.read_fd};
    struct record rec_b = {.other_fd = a.read_fd};
    struct timer_record timer_rec = {0};
    add_watch(loop, a.read_fd, EL_READABLE, &rec_a);
    add_watch(loop, b.read_fd, EL_READABLE, &rec_b);
    write_byte(a.write_fd);
    write_byte(b.write_fd);

    run_for_ms(loop, SHORT_RUN_MS, &timer_rec);
    CHECK(rec_a.read_count + rec_b.read_count == 1);
    event_loop_destroy(loop);
    close_pipe(&b);
    close_pipe(&a);
}

static void counting_timer_handler(struct event_loop *loop, void *ctx)
{
    struct timer_record *rec = ctx;
    rec->call_count++;
    if (rec->should_raise_sigint) {
        raise(SIGINT);
        return;
    }
    if (clock_now_ms() - rec->start_ms >= rec->stop_after_ms) {
        event_loop_stop(loop);
    }
}

static void test_timer_fires_at_interval(void)
{
    struct event_loop *loop = event_loop_create();
    struct timer_record rec = {.start_ms = clock_now_ms(), .stop_after_ms = TIMER_RUN_MS};
    const struct el_timer timer = {
        .interval_ms = TIMER_INTERVAL_MS, .handler = counting_timer_handler, .ctx = &rec};
    event_loop_set_timer(loop, &timer);

    CHECK(event_loop_run(loop) == EL_OK);
    CHECK(rec.call_count >= EXPECTED_TIMER_CALLS - 1);
    CHECK(rec.call_count <= EXPECTED_TIMER_CALLS + 1);
    event_loop_destroy(loop);
}

/* stop 이 안 먹으면 안전 타이머가 대신 멈추고 did_time_out 이 true 가 된다 */
static void test_stop_in_handler_returns_from_run(void)
{
    struct event_loop *loop = event_loop_create();
    struct pipe_pair p = open_pipe();
    struct record rec = {.should_stop = true};
    struct timer_record timer_rec = {0};
    add_watch(loop, p.read_fd, EL_READABLE, &rec);
    write_byte(p.write_fd);

    run_for_ms(loop, SAFETY_TIMEOUT_MS, &timer_rec);
    CHECK(rec.read_count == 1);
    CHECK(!timer_rec.did_time_out);
    event_loop_destroy(loop);
    close_pipe(&p);
}

/* SIGINT 를 받으면 run 이 반환하고, 그 뒤 다시 run 해도 바로 멈추지 않는다 */
static void test_sigint_stops_run_and_loop_is_reusable(void)
{
    CHECK(event_loop_stop_on_signals() == EL_OK);
    struct event_loop *loop = event_loop_create();
    struct timer_record rec = {.should_raise_sigint = true};
    const struct el_timer timer = {
        .interval_ms = SHORT_RUN_MS, .handler = counting_timer_handler, .ctx = &rec};
    event_loop_set_timer(loop, &timer);
    CHECK(event_loop_run(loop) == EL_OK);
    CHECK(rec.call_count == 1);

    struct timer_record stop_rec = {0};
    run_for_ms(loop, SHORT_RUN_MS, &stop_rec);
    CHECK(stop_rec.did_time_out);
    event_loop_destroy(loop);
    signal(SIGINT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
}

static void test_add_modify_remove_errors(void)
{
    struct event_loop *loop = event_loop_create();
    struct pipe_pair p = open_pipe();
    struct record rec = {0};
    add_watch(loop, p.read_fd, EL_READABLE, &rec);

    const struct el_watch duplicate = {
        .fd = p.read_fd, .mask = EL_READABLE, .handler = record_handler, .ctx = &rec};
    CHECK(event_loop_add(loop, &duplicate) == EL_ALREADY_EXISTS);
    CHECK(event_loop_modify(loop, p.write_fd, EL_WRITABLE) == EL_NOT_FOUND);
    CHECK(event_loop_remove(loop, p.write_fd) == EL_NOT_FOUND);
    CHECK(event_loop_remove(loop, -1) == EL_NOT_FOUND);

    const struct el_watch negative = {.fd = -1, .mask = EL_READABLE, .handler = record_handler};
    CHECK(event_loop_add(loop, &negative) == EL_SYSTEM_ERROR);
    event_loop_destroy(loop);
    close_pipe(&p);
}

/* 초기 표(64칸)보다 큰 fd 번호도 등록되고 이벤트가 온다 */
static void test_high_fd_number_is_dispatched(void)
{
    struct event_loop *loop = event_loop_create();
    struct pipe_pair p = open_pipe();
    int high_fd = fcntl(p.read_fd, F_DUPFD, HIGH_FD);
    CHECK(high_fd >= HIGH_FD);
    struct record rec = {0};
    struct timer_record timer_rec = {0};
    add_watch(loop, high_fd, EL_READABLE, &rec);
    write_byte(p.write_fd);

    run_for_ms(loop, SHORT_RUN_MS, &timer_rec);
    CHECK(rec.read_count == 1);
    CHECK(event_loop_remove(loop, high_fd) == EL_OK);
    event_loop_destroy(loop);
    close(high_fd);
    close_pipe(&p);
}

static void test_destroy_with_watches_returns_used_bytes(void)
{
    size_t baseline_bytes = mem_used_bytes();
    struct event_loop *loop = event_loop_create();
    struct pipe_pair p = open_pipe();
    struct record rec = {0};
    add_watch(loop, p.read_fd, EL_READABLE, &rec);
    add_watch(loop, p.write_fd, EL_WRITABLE, &rec);
    event_loop_destroy(loop);
    CHECK(mem_used_bytes() == baseline_bytes);
    close_pipe(&p);
}

/* 이벤트가 없으면 max_wait_ms 만큼만 기다리고 돌아온다 */
static void test_run_once_returns_after_events_or_wait(void)
{
    struct event_loop *loop = event_loop_create();
    struct pipe_pair p = open_pipe();
    struct record rec = {0};
    add_watch(loop, p.read_fd, EL_READABLE, &rec);

    int64_t start_ms = clock_now_ms();
    CHECK(event_loop_run_once(loop, SHORT_RUN_MS) == EL_OK);
    CHECK(clock_now_ms() - start_ms < SAFETY_TIMEOUT_MS);
    CHECK(rec.read_count == 0);

    write_byte(p.write_fd);
    CHECK(event_loop_run_once(loop, SHORT_RUN_MS) == EL_OK);
    CHECK(rec.read_count == 1);
    event_loop_destroy(loop);
    close_pipe(&p);
}

static void run_file_event_tests(void)
{
    RUN(test_pipe_write_calls_read_handler_once);
    RUN(test_removed_fd_is_not_dispatched);
    RUN(test_modify_adds_write_events);
    RUN(test_handler_removing_own_fd_is_safe);
    RUN(test_fd_removed_by_earlier_handler_is_skipped);
    RUN(test_add_modify_remove_errors);
    RUN(test_high_fd_number_is_dispatched);
    RUN(test_destroy_with_watches_returns_used_bytes);
    RUN(test_run_once_returns_after_events_or_wait);
}

static void run_timer_and_stop_tests(void)
{
    RUN(test_timer_fires_at_interval);
    RUN(test_stop_in_handler_returns_from_run);
    RUN(test_sigint_stops_run_and_loop_is_reusable);
}

int main(void)
{
    run_file_event_tests();
    run_timer_and_stop_tests();
    TEST_DONE();
    return 0;
}
