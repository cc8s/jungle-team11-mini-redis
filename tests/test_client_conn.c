#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "client_conn.h"
#include "command.h"
#include "db.h"
#include "mem.h"
#include "test.h"

enum {
    NOW_MS = 1000,
    BIG_VALUE_BYTES = 600 * 1024,
    BIG_GET_COUNT = 4,
    WRITE_TEST_BYTES = 8 * 1024 * 1024,
    DRAIN_BUF_BYTES = 64 * 1024,
    MAX_DRAIN_ROUNDS = 100000,
    REQUEST_BUF_BYTES = 128,
};

static const char SET_KV[] = "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n";
static const char GET_K[] = "*2\r\n$3\r\nGET\r\n$1\r\nk\r\n";
static const char DEL_K[] = "*2\r\n$3\r\nDEL\r\n$1\r\nk\r\n";
static const char PROTOCOL_ERROR_REPLY[] = "-ERR Protocol error: invalid request\r\n";

/* 소켓 없이 연결 하나와 db 를 묶는다. fd 는 쓰지 않는 테스트라 -1 이다 */
struct fixture {
    struct db *db;
    struct command_ctx ctx;
    struct client_conn *conn;
};

static void fixture_init(struct fixture *fx)
{
    fx->db = db_create();
    fx->ctx = (struct command_ctx){.db = fx->db, .now_ms = NOW_MS};
    fx->conn = conn_create(-1, NOW_MS);
}

static void fixture_deinit(struct fixture *fx)
{
    conn_destroy(fx->conn);
    db_destroy(fx->db);
}

static void feed(struct fixture *fx, const char *text)
{
    CHECK(buffer_append_text(&fx->conn->read_buf, text) == BUFFER_OK);
}

static bool output_equals(const struct fixture *fx, const char *expected)
{
    const struct buffer *out = &fx->conn->write_buf;
    return out->len == strlen(expected) && memcmp(out->data, expected, out->len) == 0;
}

static void test_process_single_command_writes_reply(void)
{
    struct fixture fx;
    fixture_init(&fx);
    feed(&fx, SET_KV);
    CHECK(conn_process_input(fx.conn, &fx.ctx) == CONN_OK);
    CHECK(output_equals(&fx, "+OK\r\n"));
    CHECK(fx.conn->read_buf.len == 0);
    fixture_deinit(&fx);
}

static void test_process_pipelined_commands_in_order(void)
{
    struct fixture fx;
    fixture_init(&fx);
    feed(&fx, SET_KV);
    feed(&fx, GET_K);
    feed(&fx, DEL_K);
    CHECK(conn_process_input(fx.conn, &fx.ctx) == CONN_OK);
    CHECK(output_equals(&fx, "+OK\r\n$1\r\nv\r\n:1\r\n"));
    CHECK(fx.conn->read_buf.len == 0);
    fixture_deinit(&fx);
}

static void test_process_partial_command_waits_for_rest(void)
{
    struct fixture fx;
    fixture_init(&fx);
    char first_half[REQUEST_BUF_BYTES] = {0};
    size_t half_len = strlen(SET_KV) / 2;
    memcpy(first_half, SET_KV, half_len);

    feed(&fx, first_half);
    CHECK(conn_process_input(fx.conn, &fx.ctx) == CONN_OK);
    CHECK(fx.conn->write_buf.len == 0);
    CHECK(fx.conn->read_buf.len == half_len);

    feed(&fx, SET_KV + half_len);
    CHECK(conn_process_input(fx.conn, &fx.ctx) == CONN_OK);
    CHECK(output_equals(&fx, "+OK\r\n"));
    CHECK(fx.conn->read_buf.len == 0);
    fixture_deinit(&fx);
}

static void test_process_keeps_partial_tail_after_full_command(void)
{
    struct fixture fx;
    fixture_init(&fx);
    feed(&fx, SET_KV);
    feed(&fx, "*2\r\n$3\r\nGET");
    CHECK(conn_process_input(fx.conn, &fx.ctx) == CONN_OK);
    CHECK(output_equals(&fx, "+OK\r\n"));
    CHECK(fx.conn->read_buf.len == strlen("*2\r\n$3\r\nGET"));
    CHECK(memcmp(fx.conn->read_buf.data, "*2\r\n$3\r\nGET", fx.conn->read_buf.len) == 0);
    fixture_deinit(&fx);
}

/* 깨진 요청 앞의 명령은 실행하고, 그 뒤에 에러를 붙인 다음 닫기로 표시한다 */
static void test_process_protocol_error_replies_and_marks_close(void)
{
    struct fixture fx;
    fixture_init(&fx);
    feed(&fx, GET_K);
    feed(&fx, "*x\r\n");
    feed(&fx, SET_KV);
    CHECK(conn_process_input(fx.conn, &fx.ctx) == CONN_OK);

    char expected[REQUEST_BUF_BYTES];
    snprintf(expected, sizeof(expected), "$-1\r\n%s", PROTOCOL_ERROR_REPLY);
    CHECK(output_equals(&fx, expected));
    CHECK(fx.conn->read_buf.len == 0);
    CHECK(db_count(fx.db) == 0); /* 깨진 요청 뒤의 SET 은 실행하지 않는다 */
    CHECK(conn_is_input_paused(fx.conn));
    CHECK(!conn_should_close(fx.conn));
    buffer_clear(&fx.conn->write_buf);
    CHECK(conn_should_close(fx.conn));
    fixture_deinit(&fx);
}

static void test_process_after_close_mark_does_nothing(void)
{
    struct fixture fx;
    fixture_init(&fx);
    feed(&fx, "*x\r\n");
    CHECK(conn_process_input(fx.conn, &fx.ctx) == CONN_OK);
    buffer_clear(&fx.conn->write_buf);
    feed(&fx, SET_KV);
    CHECK(conn_process_input(fx.conn, &fx.ctx) == CONN_OK);
    CHECK(fx.conn->write_buf.len == 0);
    CHECK(db_count(fx.db) == 0);
    fixture_deinit(&fx);
}

/* 큰 응답이 쌓이면 나머지 요청은 남겨 두고, 응답을 비우면 이어서 처리한다 */
static void test_process_pauses_when_output_is_large(void)
{
    struct fixture fx;
    fixture_init(&fx);
    char *big_value = mem_alloc(BIG_VALUE_BYTES);
    memset(big_value, 'x', BIG_VALUE_BYTES);
    struct db_set_request req = {
        .key = str_create("k", 1),
        .value = str_create(big_value, BIG_VALUE_BYTES),
        .now_ms = NOW_MS,
    };
    mem_free(big_value);
    CHECK(db_set(fx.db, &req) == DB_OK);
    for (int i = 0; i < BIG_GET_COUNT; i++) {
        feed(&fx, GET_K);
    }

    CHECK(conn_process_input(fx.conn, &fx.ctx) == CONN_OK);
    CHECK(conn_is_input_paused(fx.conn));
    CHECK(fx.conn->read_buf.len == strlen(GET_K) * (BIG_GET_COUNT - 2));

    buffer_clear(&fx.conn->write_buf);
    CHECK(conn_process_input(fx.conn, &fx.ctx) == CONN_OK);
    CHECK(fx.conn->read_buf.len == 0);
    fixture_deinit(&fx);
}

/* ---- 소켓을 쓰는 읽기 · 쓰기 ---- */

static void open_socket_pair(int fds[2])
{
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    CHECK(fcntl(fds[0], F_SETFL, O_NONBLOCK) == 0);
    CHECK(fcntl(fds[1], F_SETFL, O_NONBLOCK) == 0);
}

static void test_read_appends_and_detects_close(void)
{
    int fds[2];
    open_socket_pair(fds);
    struct client_conn *conn = conn_create(fds[0], NOW_MS);
    CHECK(conn_read(conn) == CONN_OK); /* 아직 아무것도 없음 (EAGAIN) */
    CHECK(conn->read_buf.len == 0);

    CHECK(write(fds[1], GET_K, strlen(GET_K)) == (ssize_t)strlen(GET_K));
    CHECK(conn_read(conn) == CONN_OK);
    CHECK(conn->read_buf.len == strlen(GET_K));

    close(fds[1]);
    CHECK(conn_read(conn) == CONN_CLOSED);
    conn_destroy(conn);
    close(fds[0]);
}

/* 상대가 조금씩 읽는 동안 남은 응답을 이어서 보내고, 다 보내면 큰 버퍼를 돌려준다 */
static void test_write_partial_then_drains_all(void)
{
    int fds[2];
    open_socket_pair(fds);
    struct client_conn *conn = conn_create(fds[0], NOW_MS);
    char *data = mem_calloc(WRITE_TEST_BYTES, 1);
    CHECK(buffer_append(&conn->write_buf, data, WRITE_TEST_BYTES) == BUFFER_OK);
    mem_free(data);

    CHECK(conn_write(conn) == CONN_OK);
    CHECK(conn_has_pending_output(conn));
    size_t received_total = 0;
    char drain[DRAIN_BUF_BYTES];
    for (int i = 0; i < MAX_DRAIN_ROUNDS && received_total < WRITE_TEST_BYTES; i++) {
        ssize_t received = read(fds[1], drain, sizeof(drain));
        if (received > 0) {
            received_total += (size_t)received;
        }
        CHECK(conn_write(conn) == CONN_OK);
    }
    CHECK(received_total == WRITE_TEST_BYTES);
    CHECK(!conn_has_pending_output(conn));
    CHECK(conn->write_buf.cap == 0);
    conn_destroy(conn);
    close(fds[1]);
    close(fds[0]);
}

static void test_write_to_closed_peer_returns_closed(void)
{
    int fds[2];
    open_socket_pair(fds);
    struct client_conn *conn = conn_create(fds[0], NOW_MS);
    CHECK(buffer_append_text(&conn->write_buf, "+OK\r\n") == BUFFER_OK);
    close(fds[1]);
    signal(SIGPIPE, SIG_IGN);
    CHECK(conn_write(conn) == CONN_CLOSED);
    conn_destroy(conn);
    close(fds[0]);
}

static void test_conn_returns_used_bytes_to_baseline(void)
{
    size_t baseline_bytes = mem_used_bytes();
    struct fixture fx;
    fixture_init(&fx);
    feed(&fx, SET_KV);
    feed(&fx, GET_K);
    CHECK(conn_process_input(fx.conn, &fx.ctx) == CONN_OK);
    fixture_deinit(&fx);
    CHECK(mem_used_bytes() == baseline_bytes);
}

int main(void)
{
    RUN(test_process_single_command_writes_reply);
    RUN(test_process_pipelined_commands_in_order);
    RUN(test_process_partial_command_waits_for_rest);
    RUN(test_process_keeps_partial_tail_after_full_command);
    RUN(test_process_protocol_error_replies_and_marks_close);
    RUN(test_process_after_close_mark_does_nothing);
    RUN(test_process_pauses_when_output_is_large);
    RUN(test_read_appends_and_detects_close);
    RUN(test_write_partial_then_drains_all);
    RUN(test_write_to_closed_peer_returns_closed);
    RUN(test_conn_returns_used_bytes_to_baseline);
    TEST_DONE();
    return 0;
}
