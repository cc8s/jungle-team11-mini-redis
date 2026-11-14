#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "clock.h"
#include "db.h"
#include "mem.h"
#include "server.h"
#include "test.h"

enum {
    TICK_MS = 10,
    MAX_TICKS = 500,
    QUIET_TICKS = 5,
    REPLY_BUF_BYTES = 256,
    LARGE_VALUE_BYTES = 1024 * 1024,
    IDLE_TIMEOUT_MS = 50,
    IDLE_WAIT_MS = 400,
    CLIENT_COUNT = 3,
    FAKE_NOW_MS = 1000,
    TTL_MS = 1000,
    TTL_KEY_COUNT = 1000,
    REHASH_KEY_LIMIT = 10000,
    CRON_FLUSH_KEY_COUNT = 200000,
    MAX_CRON_CALLS = 100000,
    CRON_MAX_CALL_MS = 50,
    KEY_BUF_BYTES = 32,
};

static const char SET_KV[] = "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n";
static const char GET_K[] = "*2\r\n$3\r\nGET\r\n$1\r\nk\r\n";
static const char DEL_K[] = "*2\r\n$3\r\nDEL\r\n$1\r\nk\r\n";
static const char FLUSHALL[] = "*1\r\n$8\r\nFLUSHALL\r\n";

static struct server *start_server(struct server_config *cfg)
{
    cfg->port = 0;
    struct server *server = server_create(cfg);
    CHECK(server != NULL);
    return server;
}

static struct server *start_default_server(void)
{
    struct server_config cfg;
    server_config_init_default(&cfg);
    return start_server(&cfg);
}

/* 연결은 blocking 으로 맺고, 서버를 같은 스레드에서 돌려야 하므로 그 뒤로는 non-blocking 으로 쓴다
 */
static int connect_client(const struct server *server)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd >= 0);
    struct sockaddr_in addr = {.sin_family = AF_INET,
                               .sin_port = htons((uint16_t)server_port(server))};
    CHECK(inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) == 1);
    CHECK(connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    CHECK(fcntl(fd, F_SETFL, O_NONBLOCK) == 0);
    return fd;
}

static void run_ticks(struct server *server, int tick_count)
{
    for (int i = 0; i < tick_count; i++) {
        CHECK(server_run_once(server, TICK_MS) == SERVER_OK);
    }
}

/* 서버를 한 바퀴씩 돌리며 data 를 모두 보낸다. 큰 요청은 소켓 버퍼가 차므로 나눠 간다 */
static void send_all(struct server *server, int fd, const char *data, size_t len)
{
    size_t sent_total = 0;
    for (int i = 0; i < MAX_TICKS * 10 && sent_total < len; i++) {
        ssize_t sent = send(fd, data + sent_total, len - sent_total, 0);
        if (sent > 0) {
            sent_total += (size_t)sent;
        }
        CHECK(server_run_once(server, 0) == SERVER_OK);
    }
    CHECK(sent_total == len);
}

/* 서버를 돌리며 out 에 want_len 바이트가 모일 때까지 받는다. 상대가 닫으면 *out_is_closed */
static size_t receive_until(struct server *server, int fd, char *out, size_t want_len,
                            bool *out_is_closed)
{
    size_t received_total = 0;
    *out_is_closed = false;
    for (int i = 0; i < MAX_TICKS * 10 && received_total < want_len && !*out_is_closed; i++) {
        CHECK(server_run_once(server, TICK_MS) == SERVER_OK);
        ssize_t received = recv(fd, out + received_total, want_len - received_total, 0);
        if (received > 0) {
            received_total += (size_t)received;
        } else if (received == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
            *out_is_closed = true;
        }
    }
    return received_total;
}

/* request 를 보내고 정확히 expected 를 받으면 true */
static bool exchange(struct server *server, int fd, const char *request, const char *expected)
{
    send_all(server, fd, request, strlen(request));
    char reply[REPLY_BUF_BYTES];
    bool is_closed = false;
    size_t len = receive_until(server, fd, reply, strlen(expected), &is_closed);
    return len == strlen(expected) && memcmp(reply, expected, len) == 0;
}

/* 서버를 몇 바퀴 돌린 뒤 상대가 연결을 닫았는지 본다. 남은 데이터가 있으면 읽고 버린다 */
static bool is_closed_by_server(struct server *server, int fd)
{
    char sink[REPLY_BUF_BYTES];
    for (int i = 0; i < MAX_TICKS; i++) {
        CHECK(server_run_once(server, TICK_MS) == SERVER_OK);
        ssize_t received = recv(fd, sink, sizeof(sink), 0);
        if (received == 0 || (received < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
            return true;
        }
    }
    return false;
}

static void test_set_get_del_roundtrip(void)
{
    struct server *server = start_default_server();
    int fd = connect_client(server);
    CHECK(exchange(server, fd, SET_KV, "+OK\r\n"));
    CHECK(exchange(server, fd, GET_K, "$1\r\nv\r\n"));
    CHECK(exchange(server, fd, DEL_K, ":1\r\n"));
    CHECK(exchange(server, fd, "GET k\r\n", "$-1\r\n"));
    close(fd);
    server_destroy(server);
}

static void test_pipelined_commands_reply_in_order(void)
{
    struct server *server = start_default_server();
    int fd = connect_client(server);
    char request[REPLY_BUF_BYTES];
    snprintf(request, sizeof(request), "%s%s%s", SET_KV, GET_K, DEL_K);
    CHECK(exchange(server, fd, request, "+OK\r\n$1\r\nv\r\n:1\r\n"));
    close(fd);
    server_destroy(server);
}

static void test_half_command_waits_then_replies(void)
{
    struct server *server = start_default_server();
    int fd = connect_client(server);
    size_t half_len = strlen(SET_KV) / 2;
    send_all(server, fd, SET_KV, half_len);
    run_ticks(server, QUIET_TICKS);
    char reply[REPLY_BUF_BYTES];
    CHECK(recv(fd, reply, sizeof(reply), 0) < 0); /* 아직 응답 없음 (EAGAIN) */

    CHECK(exchange(server, fd, SET_KV + half_len, "+OK\r\n"));
    close(fd);
    server_destroy(server);
}

static void test_protocol_error_replies_then_closes(void)
{
    struct server *server = start_default_server();
    int fd = connect_client(server);
    CHECK(exchange(server, fd, "*x\r\n", "-ERR Protocol error: invalid request\r\n"));
    CHECK(is_closed_by_server(server, fd));
    CHECK(server_client_count(server) == 0);
    close(fd);
    server_destroy(server);
}

/* 1MB 값은 소켓 버퍼보다 커서 요청 · 응답 모두 여러 번에 나눠 오간다 */
static void test_large_value_roundtrip_is_complete(void)
{
    struct server *server = start_default_server();
    int fd = connect_client(server);
    char header[REPLY_BUF_BYTES];
    int header_len = snprintf(header, sizeof(header), "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$%d\r\n",
                              LARGE_VALUE_BYTES);
    char *value = mem_alloc(LARGE_VALUE_BYTES + 2);
    memset(value, 'x', LARGE_VALUE_BYTES);
    memcpy(value + LARGE_VALUE_BYTES, "\r\n", 2);
    send_all(server, fd, header, (size_t)header_len);
    send_all(server, fd, value, LARGE_VALUE_BYTES + 2);
    char reply[REPLY_BUF_BYTES];
    bool is_closed = false;
    CHECK(receive_until(server, fd, reply, strlen("+OK\r\n"), &is_closed) == strlen("+OK\r\n"));

    int reply_header_len = snprintf(header, sizeof(header), "$%d\r\n", LARGE_VALUE_BYTES);
    size_t want_len = (size_t)reply_header_len + LARGE_VALUE_BYTES + 2;
    char *big_reply = mem_alloc(want_len);
    send_all(server, fd, GET_K, strlen(GET_K));
    CHECK(receive_until(server, fd, big_reply, want_len, &is_closed) == want_len);
    CHECK(memcmp(big_reply, header, (size_t)reply_header_len) == 0);
    CHECK(memcmp(big_reply + reply_header_len, value, LARGE_VALUE_BYTES + 2) == 0);
    mem_free(big_reply);
    mem_free(value);
    close(fd);
    server_destroy(server);
}

static void test_multiple_clients_are_served(void)
{
    struct server *server = start_default_server();
    int fds[CLIENT_COUNT];
    for (int i = 0; i < CLIENT_COUNT; i++) {
        fds[i] = connect_client(server);
    }
    for (int i = 0; i < CLIENT_COUNT; i++) {
        CHECK(exchange(server, fds[i], SET_KV, "+OK\r\n"));
    }
    CHECK(server_client_count(server) == CLIENT_COUNT);
    for (int i = 0; i < CLIENT_COUNT; i++) {
        close(fds[i]);
    }
    server_destroy(server);
}

static void test_max_clients_rejects_extra_client(void)
{
    struct server_config cfg;
    server_config_init_default(&cfg);
    cfg.max_clients = 1;
    struct server *server = start_server(&cfg);
    int first_fd = connect_client(server);
    run_ticks(server, QUIET_TICKS);
    int extra_fd = connect_client(server);

    char reply[REPLY_BUF_BYTES];
    bool is_closed = false;
    const char *expected = "-ERR max number of clients reached\r\n";
    CHECK(receive_until(server, extra_fd, reply, strlen(expected), &is_closed) == strlen(expected));
    CHECK(memcmp(reply, expected, strlen(expected)) == 0);
    CHECK(is_closed_by_server(server, extra_fd));
    CHECK(exchange(server, first_fd, SET_KV, "+OK\r\n"));
    CHECK(server_client_count(server) == 1);
    close(extra_fd);
    close(first_fd);
    server_destroy(server);
}

static void test_idle_client_is_closed(void)
{
    struct server_config cfg;
    server_config_init_default(&cfg);
    cfg.idle_timeout_ms = IDLE_TIMEOUT_MS;
    struct server *server = start_server(&cfg);
    int fd = connect_client(server);
    CHECK(exchange(server, fd, SET_KV, "+OK\r\n"));

    int64_t start_ms = clock_now_ms();
    while (clock_now_ms() - start_ms < IDLE_WAIT_MS) {
        CHECK(server_run_once(server, TICK_MS) == SERVER_OK);
    }
    CHECK(server_client_count(server) == 0);
    CHECK(is_closed_by_server(server, fd));
    close(fd);
    server_destroy(server);
}

/* SO_LINGER 0 으로 닫으면 RST 가 가서 서버는 ECONNRESET 을 본다 */
static void test_client_reset_keeps_server_running(void)
{
    struct server *server = start_default_server();
    int fd = connect_client(server);
    send_all(server, fd, SET_KV, strlen(SET_KV) / 2);
    struct linger hard_close = {.l_onoff = 1, .l_linger = 0};
    CHECK(setsockopt(fd, SOL_SOCKET, SO_LINGER, &hard_close, sizeof(hard_close)) == 0);
    close(fd);
    run_ticks(server, QUIET_TICKS);
    CHECK(server_client_count(server) == 0);

    int next_fd = connect_client(server);
    CHECK(exchange(server, next_fd, SET_KV, "+OK\r\n"));
    close(next_fd);
    server_destroy(server);
}

/* 서버가 먼저 닫은 연결은 서버 쪽 포트에 TIME_WAIT 이 남는다. 그래도 같은 포트로 다시 열려야 한다
 */
static void test_restart_on_same_port_binds(void)
{
    struct server *server = start_default_server();
    int port = server_port(server);
    int fd = connect_client(server);
    CHECK(exchange(server, fd, "*x\r\n", "-ERR Protocol error: invalid request\r\n"));
    CHECK(is_closed_by_server(server, fd));
    close(fd);
    server_destroy(server);

    struct server_config cfg;
    server_config_init_default(&cfg);
    cfg.port = port;
    struct server *restarted = server_create(&cfg);
    CHECK(restarted != NULL);
    CHECK(server_port(restarted) == port);
    server_destroy(restarted);
}

static void test_destroy_with_open_clients_returns_used_bytes(void)
{
    size_t baseline_bytes = mem_used_bytes();
    struct server *server = start_default_server();
    int first_fd = connect_client(server);
    int second_fd = connect_client(server);
    CHECK(exchange(server, first_fd, SET_KV, "+OK\r\n"));
    run_ticks(server, QUIET_TICKS);
    CHECK(server_client_count(server) == 2);
    server_destroy(server);
    CHECK(mem_used_bytes() == baseline_bytes);
    close(second_fd);
    close(first_fd);
}

/* ---- 주기 작업 (server_cron) ---- */

/* expire_at_ms 가 0 이면 TTL 없이 넣는다 */
static void set_numbered_key(struct db *db, int number, int64_t expire_at_ms)
{
    char buf[KEY_BUF_BYTES];
    int len = snprintf(buf, sizeof(buf), "key:%d", number);
    struct db_set_request req = {
        .key = str_create(buf, (size_t)len),
        .value = str_create("v", 1),
        .now_ms = FAKE_NOW_MS,
    };
    CHECK(db_set(db, &req) == DB_OK);
    if (expire_at_ms > 0) {
        struct str *lookup = str_create(buf, (size_t)len);
        CHECK(db_set_expire(db, lookup, expire_at_ms) == DB_OK);
        str_destroy(lookup);
    }
}

static void test_cron_removes_expired_keys_without_access(void)
{
    struct server *server = start_default_server();
    struct db *db = server_db(server);
    for (int i = 0; i < TTL_KEY_COUNT; i++) {
        set_numbered_key(db, i, FAKE_NOW_MS + TTL_MS);
    }
    server_cron(server, FAKE_NOW_MS);
    CHECK(db_count(db) == TTL_KEY_COUNT); /* 아직 만료 전 */

    for (int i = 0; i < MAX_CRON_CALLS && db_count(db) > 0; i++) {
        server_cron(server, FAKE_NOW_MS + TTL_MS);
    }
    CHECK(db_count(db) == 0);
    CHECK(db_ttl_count(db) == 0);
    server_destroy(server);
}

static void test_cron_finishes_rehash_without_access(void)
{
    struct server *server = start_default_server();
    struct db *db = server_db(server);
    for (int i = 0; i < REHASH_KEY_LIMIT && !db_is_rehashing(db); i++) {
        set_numbered_key(db, i, 0);
    }
    CHECK(db_is_rehashing(db));
    for (int i = 0; i < MAX_CRON_CALLS && db_is_rehashing(db); i++) {
        server_cron(server, FAKE_NOW_MS);
    }
    CHECK(!db_is_rehashing(db));
    server_destroy(server);
}

/* 대량 FLUSHALL 뒤의 해제는 여러 번의 cron 에 나눠지고, 한 번은 짧게 끝나야 한다 */
static void test_cron_frees_flushed_keys_in_short_steps(void)
{
    size_t baseline_bytes = mem_used_bytes();
    struct server *server = start_default_server();
    struct db *db = server_db(server);
    for (int i = 0; i < CRON_FLUSH_KEY_COUNT; i++) {
        set_numbered_key(db, i, 0);
    }
    CHECK(db_flush(db) == DB_OK);

    int call_count = 0;
    int64_t slowest_ms = 0;
    while (call_count < MAX_CRON_CALLS && db_has_pending_free(db)) {
        int64_t start_ms = clock_now_ms();
        server_cron(server, FAKE_NOW_MS);
        int64_t spent_ms = clock_now_ms() - start_ms;
        if (spent_ms > slowest_ms) {
            slowest_ms = spent_ms;
        }
        call_count++;
    }
    printf("  cron %d 번, 가장 긴 한 번 %lld ms\n", call_count, (long long)slowest_ms);
    CHECK(!db_has_pending_free(db));
    CHECK(call_count > 1);
    CHECK(slowest_ms <= CRON_MAX_CALL_MS);
    server_destroy(server);
    CHECK(mem_used_bytes() == baseline_bytes);
}

static void test_flushall_then_get_replies_while_freeing(void)
{
    struct server_config cfg;
    server_config_init_default(&cfg);
    cfg.is_flush_enabled = true;
    struct server *server = start_server(&cfg);
    for (int i = 0; i < CRON_FLUSH_KEY_COUNT; i++) {
        set_numbered_key(server_db(server), i, 0);
    }
    int fd = connect_client(server);
    CHECK(exchange(server, fd, FLUSHALL, "+OK\r\n"));
    CHECK(exchange(server, fd, GET_K, "$-1\r\n"));
    CHECK(db_count(server_db(server)) == 0);
    close(fd);
    server_destroy(server);
}

int main(void)
{
    RUN(test_set_get_del_roundtrip);
    RUN(test_pipelined_commands_reply_in_order);
    RUN(test_half_command_waits_then_replies);
    RUN(test_protocol_error_replies_then_closes);
    RUN(test_large_value_roundtrip_is_complete);
    RUN(test_multiple_clients_are_served);
    RUN(test_max_clients_rejects_extra_client);
    RUN(test_idle_client_is_closed);
    RUN(test_client_reset_keeps_server_running);
    RUN(test_restart_on_same_port_binds);
    RUN(test_destroy_with_open_clients_returns_used_bytes);
    RUN(test_cron_removes_expired_keys_without_access);
    RUN(test_cron_finishes_rehash_without_access);
    RUN(test_cron_frees_flushed_keys_in_short_steps);
    RUN(test_flushall_then_get_replies_while_freeing);
    TEST_DONE();
    return 0;
}
