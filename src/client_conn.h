#ifndef MINI_REDIS_CLIENT_CONN_H
#define MINI_REDIS_CLIENT_CONN_H

#include <stdbool.h>
#include <stdint.h>

#include "buffer.h"
#include "command.h"

enum {
    CONN_READ_CHUNK_BYTES = 16 * 1024,
    CONN_MAX_QUERY_BYTES = 1024 * 1024 * 1024,
    CONN_OUTPUT_PAUSE_BYTES = 1024 * 1024,
    CONN_KEEP_BUFFER_BYTES = 64 * 1024,
};

enum conn_result {
    CONN_OK = 0,
    CONN_CLOSED,        /* 상대가 끊었거나 소켓 오류. 연결을 닫는다 */
    CONN_OUT_OF_MEMORY, /* 응답을 만들 메모리가 없다. 연결을 닫는다 */
};

struct server;

/* 클라이언트 연결 하나. fd 는 서버가 열고 닫는다. prev · next 는 서버의 연결 목록이다.
 * read_buf 에는 아직 처리하지 않은 요청이, write_buf 에는 아직 보내지 않은 응답이 있다. */
struct client_conn {
    int fd;
    struct buffer read_buf;
    struct buffer write_buf;
    int64_t last_active_ms;
    int event_mask; /* 이벤트 루프에 지금 등록된 mask */
    bool should_close_after_write;
    struct server *server;
    struct client_conn *prev;
    struct client_conn *next;
};

/* 빈 버퍼를 가진 연결을 만든다. 실패하면 NULL, conn_destroy() 로 해제한다. */
struct client_conn *conn_create(int fd, int64_t now_ms);

/* 버퍼와 연결 구조체를 해제한다. fd 는 닫지 않는다. NULL 이면 아무것도 하지 않는다. */
void conn_destroy(struct client_conn *conn);

/* 소켓에서 한 번 읽어 read_buf 뒤에 붙인다. 읽을 것이 없으면(EAGAIN) CONN_OK,
 * 상대가 끊었거나 오류면 CONN_CLOSED. 한 번에 CONN_READ_CHUNK_BYTES 까지만 읽는다. */
enum conn_result conn_read(struct client_conn *conn);

/* read_buf 의 완성된 명령을 순서대로 실행하고 응답을 write_buf 에 붙인다.
 * 덜 온 명령은 남겨 두고, 깨진 요청이면 에러 응답 뒤 닫기로 표시한다.
 * write_buf 가 CONN_OUTPUT_PAUSE_BYTES 를 넘으면 나머지 요청은 다음으로 미룬다. */
enum conn_result conn_process_input(struct client_conn *conn, const struct command_ctx *ctx);

/* write_buf 를 소켓이 받는 만큼 보낸다. 다 못 보내면 남겨 두고 CONN_OK, 오류면 CONN_CLOSED. */
enum conn_result conn_write(struct client_conn *conn);

/* 보낼 응답이 남아 있으면 true */
bool conn_has_pending_output(const struct client_conn *conn);

/* 요청을 더 읽거나 처리하지 않아야 하면 true (닫기 표시 또는 응답이 너무 쌓임) */
bool conn_is_input_paused(const struct client_conn *conn);

/* 닫기로 표시됐고 남은 응답도 다 보냈으면 true */
bool conn_should_close(const struct client_conn *conn);

#endif
