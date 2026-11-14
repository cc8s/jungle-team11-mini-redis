#include "server.h"

#include <errno.h>
#include <signal.h>
#include <string.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#ifdef __GLIBC__
#include <malloc.h>
#endif

#include "client_conn.h"
#include "clock.h"
#include "command.h"
#include "db.h"
#include "event_loop.h"
#include "expire.h"
#include "mem.h"

static const char DEFAULT_BIND_ADDRESS[] = "127.0.0.1";
static const char MAX_CLIENTS_MESSAGE[] = "-ERR max number of clients reached\r\n";

/* 불변식: conns 목록의 연결은 모두 이벤트 루프에 등록돼 있고 conn_count 와 개수가 같다 */
struct server {
    struct server_config cfg;
    struct event_loop *loop;
    struct db *db;
    int listen_fd;
    struct client_conn *conns;
    size_t conn_count;
};

static int open_listen_socket(const struct server_config *cfg);
static bool set_nonblocking(int fd);
static void handle_accept(struct event_loop *loop, const struct el_watch *watch, int fired_mask);
static void accept_client(struct server *server, int fd);
static void reject_client(int fd);
static void handle_client_event(struct event_loop *loop, const struct el_watch *watch,
                                int fired_mask);
static enum conn_result serve_client(struct server *server, struct client_conn *conn,
                                     int64_t now_ms);
static bool update_client_events(struct server *server, struct client_conn *conn);
static void close_client(struct server *server, struct client_conn *conn);
static void handle_cron(struct event_loop *loop, void *ctx);
static void close_idle_clients(struct server *server, int64_t now_ms);
static void fix_mmap_threshold(void);

void server_config_init_default(struct server_config *cfg)
{
    *cfg = (struct server_config){
        .bind_address = DEFAULT_BIND_ADDRESS,
        .port = SERVER_DEFAULT_PORT,
        .max_clients = SERVER_MAX_CLIENTS,
        .idle_timeout_ms = 0,
        .is_flush_enabled = false,
    };
}

/* 끊긴 소켓에 쓰면 SIGPIPE 로 프로세스가 죽으므로 무시하고 send 의 EPIPE 로 처리한다 */
struct server *server_create(const struct server_config *cfg)
{
    struct server *server = mem_calloc(1, sizeof(*server));
    if (server == NULL) {
        return NULL;
    }
    server->cfg = *cfg;
    signal(SIGPIPE, SIG_IGN);
    fix_mmap_threshold();
    server->loop = event_loop_create();
    server->db = db_create();
    server->listen_fd = open_listen_socket(cfg);
    if (server->loop == NULL || server->db == NULL || server->listen_fd < 0) {
        goto fail;
    }
    const struct el_watch accept_watch = {
        .fd = server->listen_fd, .mask = EL_READABLE, .handler = handle_accept, .ctx = server};
    if (event_loop_add(server->loop, &accept_watch) != EL_OK) {
        goto fail;
    }
    const struct el_timer cron = {
        .interval_ms = SERVER_CRON_INTERVAL_MS, .handler = handle_cron, .ctx = server};
    event_loop_set_timer(server->loop, &cron);
    return server;

fail:
    server_destroy(server);
    return NULL;
}

void server_destroy(struct server *server)
{
    if (server == NULL) {
        return;
    }
    while (server->conns != NULL) {
        close_client(server, server->conns);
    }
    if (server->listen_fd >= 0) {
        if (server->loop != NULL) {
            event_loop_remove(server->loop, server->listen_fd);
        }
        close(server->listen_fd);
    }
    event_loop_destroy(server->loop);
    db_destroy(server->db);
    mem_free(server);
}

enum server_result server_run(struct server *server)
{
    return event_loop_run(server->loop) == EL_OK ? SERVER_OK : SERVER_SYSTEM_ERROR;
}

enum server_result server_run_once(struct server *server, int max_wait_ms)
{
    return event_loop_run_once(server->loop, max_wait_ms) == EL_OK ? SERVER_OK
                                                                   : SERVER_SYSTEM_ERROR;
}

int server_port(const struct server *server)
{
    struct sockaddr_in addr = {0};
    socklen_t addr_len = sizeof(addr);
    if (getsockname(server->listen_fd, (struct sockaddr *)&addr, &addr_len) != 0) {
        return -1;
    }
    return ntohs(addr.sin_port);
}

/* 루프를 오래 막지 않도록 일마다 예산을 따로 둔다. 만료를 먼저 해서 지운 만큼 rehash 할 양이
 * 줄고, 유휴 정리는 연결 수에 비례하는 가벼운 일이라 예산 없이 마지막에 한다 */
void server_cron(struct server *server, int64_t now_ms)
{
    expire_cycle(server->db, now_ms, SERVER_CRON_TASK_BUDGET_MS);
    db_rehash_for_ms(server->db, SERVER_CRON_TASK_BUDGET_MS);
    db_free_pending_for_ms(server->db, SERVER_CRON_TASK_BUDGET_MS);
    close_idle_clients(server, now_ms);
}

struct db *server_db(struct server *server)
{
    return server->db;
}

size_t server_client_count(const struct server *server)
{
    return server->conn_count;
}

/* SO_REUSEADDR 로 TIME_WAIT 가 남아 있어도 같은 포트로 바로 다시 bind 한다 */
static int open_listen_socket(const struct server_config *cfg)
{
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons((uint16_t)cfg->port)};
    if (inet_pton(AF_INET, cfg->bind_address, &addr.sin_addr) != 1) {
        return -1;
    }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    int reuse = 1;
    bool is_ok = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) == 0 &&
                 set_nonblocking(fd) && bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0 &&
                 listen(fd, SERVER_LISTEN_BACKLOG) == 0;
    if (!is_ok) {
        close(fd);
        return -1;
    }
    return fd;
}

/* accept4 는 Linux 전용이라 macOS 와 같이 쓰도록 fcntl 로 따로 설정한다 */
static bool set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0 &&
           fcntl(fd, F_SETFD, FD_CLOEXEC) == 0;
}

/* 한 번 깨어났을 때 대기 중인 연결을 EAGAIN 이 날 때까지 모두 받는다.
 * EMFILE 같은 다른 오류도 이번 바퀴에서는 그만 받고, 레벨 트리거라 다음 바퀴에 다시 시도한다 */
static void handle_accept(struct event_loop *loop, const struct el_watch *watch, int fired_mask)
{
    (void)loop;
    (void)fired_mask;
    struct server *server = watch->ctx;
    for (;;) {
        int fd = accept(server->listen_fd, NULL, NULL);
        if (fd < 0) {
            return;
        }
        accept_client(server, fd);
    }
}

static void accept_client(struct server *server, int fd)
{
    int no_delay = 1;
    if (!set_nonblocking(fd) ||
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &no_delay, sizeof(no_delay)) != 0) {
        close(fd);
        return;
    }
    if (server->conn_count >= server->cfg.max_clients) {
        reject_client(fd);
        return;
    }
    struct client_conn *conn = conn_create(fd, clock_now_ms());
    const struct el_watch watch = {
        .fd = fd, .mask = EL_READABLE, .handler = handle_client_event, .ctx = conn};
    if (conn == NULL || event_loop_add(server->loop, &watch) != EL_OK) {
        conn_destroy(conn);
        close(fd);
        return;
    }
    conn->server = server;
    conn->event_mask = EL_READABLE;
    conn->next = server->conns;
    if (server->conns != NULL) {
        server->conns->prev = conn;
    }
    server->conns = conn;
    server->conn_count++;
}

/* 연결을 받은 직후라 소켓 버퍼가 비어 있어 짧은 메시지는 한 번에 나간다. 못 보내도 그냥 닫는다 */
static void reject_client(int fd)
{
    ssize_t sent = send(fd, MAX_CLIENTS_MESSAGE, sizeof(MAX_CLIENTS_MESSAGE) - 1, 0);
    (void)sent;
    close(fd);
}

/* 이벤트가 오면 읽기 → 처리 → 쓰기를 하고, 상태에 맞게 기다릴 이벤트를 다시 정한다 */
static void handle_client_event(struct event_loop *loop, const struct el_watch *watch,
                                int fired_mask)
{
    (void)loop;
    struct client_conn *conn = watch->ctx;
    struct server *server = conn->server;
    int64_t now_ms = clock_now_ms();
    conn->last_active_ms = now_ms;

    enum conn_result result = CONN_OK;
    if ((fired_mask & EL_READABLE) && !conn_is_input_paused(conn)) {
        result = conn_read(conn);
    }
    if (result == CONN_OK) {
        result = serve_client(server, conn, now_ms);
    }
    if (result != CONN_OK || conn_should_close(conn) || !update_client_events(server, conn)) {
        close_client(server, conn);
    }
}

/* 응답을 다 보냈는데 미뤄 둔 요청이 남아 있으면 이어서 처리한다. 소켓에서 새로 읽지 않으면
 * 읽기 이벤트가 오지 않으므로, 여기서 처리하지 않으면 남은 요청이 영영 멈춘다 */
static enum conn_result serve_client(struct server *server, struct client_conn *conn,
                                     int64_t now_ms)
{
    const struct command_ctx ctx = {
        .db = server->db, .now_ms = now_ms, .is_flush_enabled = server->cfg.is_flush_enabled};
    for (;;) {
        size_t pending_input = conn->read_buf.len;
        enum conn_result result = conn_process_input(conn, &ctx);
        if (result == CONN_OK && conn_has_pending_output(conn)) {
            result = conn_write(conn);
        }
        if (result != CONN_OK) {
            return result;
        }
        bool made_progress = conn->read_buf.len < pending_input;
        if (conn_has_pending_output(conn) || !made_progress || conn->read_buf.len == 0) {
            return CONN_OK;
        }
    }
}

/* 응답이 쌓였거나 닫을 예정이면 읽기를 끄고, 보낼 응답이 남았을 때만 쓰기를 켠다 */
static bool update_client_events(struct server *server, struct client_conn *conn)
{
    int mask = conn_is_input_paused(conn) ? 0 : EL_READABLE;
    if (conn_has_pending_output(conn)) {
        mask |= EL_WRITABLE;
    }
    if (mask == conn->event_mask) {
        return true;
    }
    if (event_loop_modify(server->loop, conn->fd, mask) != EL_OK) {
        return false;
    }
    conn->event_mask = mask;
    return true;
}

/* 순서 고정: 루프 등록 해제 → fd close → 버퍼 해제. close 를 먼저 하면 같은 fd 번호가
 * 재사용됐을 때 루프의 이전 등록과 섞인다 */
static void close_client(struct server *server, struct client_conn *conn)
{
    event_loop_remove(server->loop, conn->fd);
    close(conn->fd);
    if (conn->prev != NULL) {
        conn->prev->next = conn->next;
    } else {
        server->conns = conn->next;
    }
    if (conn->next != NULL) {
        conn->next->prev = conn->prev;
    }
    conn_destroy(conn);
    server->conn_count--;
}

static void handle_cron(struct event_loop *loop, void *ctx)
{
    (void)loop;
    server_cron(ctx, clock_now_ms());
}

static void close_idle_clients(struct server *server, int64_t now_ms)
{
    if (server->cfg.idle_timeout_ms <= 0) {
        return;
    }
    struct client_conn *conn = server->conns;
    while (conn != NULL) {
        struct client_conn *next = conn->next;
        if (now_ms - conn->last_active_ms >= server->cfg.idle_timeout_ms) {
            close_client(server, conn);
        }
        conn = next;
    }
}

/* glibc 는 큰 블록을 해제할 때마다 mmap 임계값을 그 크기로 올린다. 그러면 큰 버킷 배열도 힙에
 * 잡히고, 대량 FLUSHALL 의 마지막 해제에서 힙 꼭대기 수십 MB 를 한 번에 OS 에 돌려주느라
 * cron 한 번이 수십 ms 멈춘다. 임계값을 고정해 큰 블록은 항상 mmap 으로 따로 받고 돌려준다 */
static void fix_mmap_threshold(void)
{
#ifdef __GLIBC__
    mallopt(M_MMAP_THRESHOLD, SERVER_MMAP_THRESHOLD_BYTES);
#endif
}
