#ifndef MINI_REDIS_COMMAND_H
#define MINI_REDIS_COMMAND_H

#include <stdbool.h>
#include <stdint.h>

#include "buffer.h"
#include "db.h"
#include "resp.h"

enum {
    COMMAND_MAX_NAME_IN_ERROR = 128,
    COMMAND_MS_PER_SEC = 1000,
};

enum command_result {
    COMMAND_OK = 0,
    COMMAND_OUT_OF_MEMORY, /* 응답을 out_buf 에 붙이지 못했다. 연결을 닫아야 한다 */
};

/* 명령 하나를 실행하는 데 필요한 것. 연결 계층이 명령마다 채운다. */
struct command_ctx {
    struct db *db;
    int64_t now_ms;
    bool is_flush_enabled; /* MINI_REDIS_ENABLE_FLUSH=1 일 때만 true */
};

/* 명령을 실행하고 응답 하나를 out_buf 뒤에 붙인다. 명령 오류(알 수 없는 명령, 인자 수 등)도
 * 에러 응답으로 붙이고 COMMAND_OK 를 돌려준다. cmd 는 읽기만 한다. */
enum command_result command_execute(const struct command_ctx *ctx, const struct resp_command *cmd,
                                    struct buffer *out_buf);

#endif
