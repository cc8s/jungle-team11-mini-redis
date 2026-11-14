#include "client_conn.h"

#include <errno.h>
#include <stddef.h>

#include <sys/socket.h>
#include <unistd.h>

#include "mem.h"
#include "resp.h"

static const char PROTOCOL_ERROR_MESSAGE[] = "ERR Protocol error: invalid request";

static enum conn_result reply_protocol_error(struct client_conn *conn);
static enum conn_result run_one_command(struct client_conn *conn, const struct command_ctx *ctx,
                                        size_t *offset);
static void release_if_large(struct buffer *buf);

struct client_conn *conn_create(int fd, int64_t now_ms)
{
    struct client_conn *conn = mem_calloc(1, sizeof(*conn));
    if (conn == NULL) {
        return NULL;
    }
    conn->fd = fd;
    buffer_init(&conn->read_buf);
    buffer_init(&conn->write_buf);
    conn->last_active_ms = now_ms;
    return conn;
}

void conn_destroy(struct client_conn *conn)
{
    if (conn == NULL) {
        return;
    }
    buffer_deinit(&conn->read_buf);
    buffer_deinit(&conn->write_buf);
    mem_free(conn);
}

/* 이벤트 하나에 한 번만 읽는다. 레벨 트리거라 남은 데이터는 다음 바퀴에 다시 알려 주므로,
 * 큰 요청을 보내는 연결 하나가 루프를 독차지하지 않는다 */
enum conn_result conn_read(struct client_conn *conn)
{
    char chunk[CONN_READ_CHUNK_BYTES];
    ssize_t read_bytes = read(conn->fd, chunk, sizeof(chunk));
    if (read_bytes > 0) {
        bool is_ok = buffer_append(&conn->read_buf, chunk, (size_t)read_bytes) == BUFFER_OK;
        return is_ok ? CONN_OK : CONN_OUT_OF_MEMORY;
    }
    if (read_bytes < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
        return CONN_OK;
    }
    return CONN_CLOSED; /* 0 은 상대가 닫음, 나머지는 ECONNRESET 같은 오류 */
}

/* 파이프라인으로 붙어 온 명령을 앞에서부터 실행하고, 처리한 앞부분은 마지막에 한 번만 당긴다 */
enum conn_result conn_process_input(struct client_conn *conn, const struct command_ctx *ctx)
{
    if (conn->should_close_after_write) {
        return CONN_OK;
    }
    if (conn->read_buf.len > CONN_MAX_QUERY_BYTES) {
        return reply_protocol_error(conn);
    }
    size_t offset = 0;
    enum conn_result result = CONN_OK;
    while (result == CONN_OK && offset < conn->read_buf.len && !conn_is_input_paused(conn)) {
        size_t before = offset;
        result = run_one_command(conn, ctx, &offset);
        if (offset == before) {
            break; /* 명령이 덜 왔다 */
        }
    }
    buffer_consume(&conn->read_buf, offset);
    release_if_large(&conn->read_buf);
    return result;
}

enum conn_result conn_write(struct client_conn *conn)
{
    size_t sent_total = 0;
    while (sent_total < conn->write_buf.len) {
        ssize_t sent =
            send(conn->fd, conn->write_buf.data + sent_total, conn->write_buf.len - sent_total, 0);
        if (sent > 0) {
            sent_total += (size_t)sent;
            continue;
        }
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        }
        return CONN_CLOSED;
    }
    buffer_consume(&conn->write_buf, sent_total);
    release_if_large(&conn->write_buf);
    return CONN_OK;
}

bool conn_has_pending_output(const struct client_conn *conn)
{
    return conn->write_buf.len > 0;
}

bool conn_is_input_paused(const struct client_conn *conn)
{
    return conn->should_close_after_write || conn->write_buf.len >= CONN_OUTPUT_PAUSE_BYTES;
}

bool conn_should_close(const struct client_conn *conn)
{
    return conn->should_close_after_write && !conn_has_pending_output(conn);
}

/* 깨진 요청 뒤의 바이트는 믿을 수 없으므로 버리고, 에러를 보낸 뒤 닫는다 (Redis 와 동일) */
static enum conn_result reply_protocol_error(struct client_conn *conn)
{
    buffer_clear(&conn->read_buf);
    release_if_large(&conn->read_buf);
    conn->should_close_after_write = true;
    if (resp_write_error(&conn->write_buf, PROTOCOL_ERROR_MESSAGE) != BUFFER_OK) {
        return CONN_OUT_OF_MEMORY;
    }
    return CONN_OK;
}

/* read_buf 의 offset 부터 명령 하나를 파싱해 실행한다. 실행했으면 offset 을 그 명령 뒤로 옮기고,
 * 덜 왔으면 offset 을 그대로 둔다 */
static enum conn_result run_one_command(struct client_conn *conn, const struct command_ctx *ctx,
                                        size_t *offset)
{
    struct resp_command cmd;
    enum resp_parse_result parsed =
        resp_parse(conn->read_buf.data + *offset, conn->read_buf.len - *offset, &cmd);
    if (parsed == RESP_PARSE_NEED_MORE) {
        return CONN_OK;
    }
    if (parsed == RESP_PARSE_ERROR) {
        *offset = conn->read_buf.len;
        return reply_protocol_error(conn);
    }
    if (parsed == RESP_PARSE_OUT_OF_MEMORY) {
        return CONN_OUT_OF_MEMORY;
    }
    enum command_result executed = command_execute(ctx, &cmd, &conn->write_buf);
    *offset += cmd.consumed_bytes;
    resp_command_deinit(&cmd);
    return executed == COMMAND_OK ? CONN_OK : CONN_OUT_OF_MEMORY;
}

/* 큰 값을 한 번 주고받은 연결이 그 크기의 버퍼를 계속 들고 있지 않게, 비었을 때 돌려준다.
 * 버퍼도 mem 사용량에 들어가 maxmemory 퇴출에 영향을 주기 때문이다 */
static void release_if_large(struct buffer *buf)
{
    if (buf->len == 0 && buf->cap > CONN_KEEP_BUFFER_BYTES) {
        buffer_deinit(buf);
    }
}
