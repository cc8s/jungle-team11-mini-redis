#include "command.h"

#include <stddef.h>
#include <stdio.h>

enum {
    ERROR_MESSAGE_BYTES = 256,
    MAX_EXPIRE_DIGITS = 18,
    DECIMAL_BASE = 10,
    SET_OPTION_START_IDX = 3,
    ASCII_DEL = 0x7f,
};

static const char OOM_MESSAGE[] = "OOM command not allowed when used memory > 'maxmemory'.";
static const char SYNTAX_ERROR_MESSAGE[] = "ERR syntax error";
static const char INVALID_EXPIRE_MESSAGE[] = "ERR invalid expire time in 'set' command";

typedef enum buffer_result command_handler_fn(const struct command_ctx *ctx,
                                              const struct resp_command *cmd,
                                              struct buffer *out_buf);

/* arity 는 명령 이름을 포함한 인자 수다. 음수면 그 절댓값이 최소 인자 수다 (Redis 관례).
 * name 은 소문자로 둔다. 인자 수 오류 메시지에 그대로 쓰인다 */
struct command_spec {
    const char *name;
    int arity;
    command_handler_fn *handler;
};

/* SET 의 옵션을 읽은 결과. 저장하기 전에 모두 검사해서, 옵션이 틀리면 아무것도 바꾸지 않는다 */
struct set_options {
    bool has_expire;
    int64_t expire_at_ms;
};

enum set_parse_result {
    SET_PARSE_OK = 0,
    SET_PARSE_SYNTAX_ERROR,
    SET_PARSE_INVALID_EXPIRE,
};

static enum buffer_result handle_set_command(const struct command_ctx *ctx,
                                             const struct resp_command *cmd,
                                             struct buffer *out_buf);
static enum buffer_result handle_get_command(const struct command_ctx *ctx,
                                             const struct resp_command *cmd,
                                             struct buffer *out_buf);
static enum buffer_result handle_del_command(const struct command_ctx *ctx,
                                             const struct resp_command *cmd,
                                             struct buffer *out_buf);
static enum buffer_result handle_flushall_command(const struct command_ctx *ctx,
                                                  const struct resp_command *cmd,
                                                  struct buffer *out_buf);

static const struct command_spec COMMAND_TABLE[] = {
    {"set", -3, handle_set_command},
    {"get", 2, handle_get_command},
    {"del", -2, handle_del_command},
    {"flushall", -1, handle_flushall_command},
};

enum {
    COMMAND_COUNT = sizeof(COMMAND_TABLE) / sizeof(COMMAND_TABLE[0]),
};

static const struct command_spec *find_spec(const struct str *name);
static bool is_arity_ok(const struct command_spec *spec, size_t argc);
static bool name_equals(const struct str *arg, const char *name);
static char to_lower_ascii(char c);
static char to_printable(char c);
static enum buffer_result reply_unknown_command(struct buffer *out_buf, const struct str *name);
static enum buffer_result reply_wrong_arity(struct buffer *out_buf,
                                            const struct command_spec *spec);
static enum set_parse_result parse_set_options(const struct command_ctx *ctx,
                                               const struct resp_command *cmd,
                                               struct set_options *out_options);
static bool parse_expire_at_ms(const struct str *seconds_arg, int64_t now_ms,
                               int64_t *out_expire_at_ms);
static enum db_result store_value(const struct command_ctx *ctx, const struct resp_command *cmd,
                                  const struct set_options *options);

enum command_result command_execute(const struct command_ctx *ctx, const struct resp_command *cmd,
                                    struct buffer *out_buf)
{
    enum buffer_result result = BUFFER_OK;
    const struct command_spec *spec = cmd->argc > 0 ? find_spec(cmd->argv[0]) : NULL;
    if (spec == NULL) {
        result = cmd->argc > 0 ? reply_unknown_command(out_buf, cmd->argv[0])
                               : resp_write_error(out_buf, "ERR empty command");
    } else if (!is_arity_ok(spec, cmd->argc)) {
        result = reply_wrong_arity(out_buf, spec);
    } else {
        result = spec->handler(ctx, cmd, out_buf);
    }
    return result == BUFFER_OK ? COMMAND_OK : COMMAND_OUT_OF_MEMORY;
}

/* SET key value [EX seconds]. 옵션을 먼저 모두 검사하고, 문제가 없을 때만 저장한다 */
static enum buffer_result handle_set_command(const struct command_ctx *ctx,
                                             const struct resp_command *cmd, struct buffer *out_buf)
{
    struct set_options options = {0};
    switch (parse_set_options(ctx, cmd, &options)) {
    case SET_PARSE_OK:
        break;
    case SET_PARSE_SYNTAX_ERROR:
        return resp_write_error(out_buf, SYNTAX_ERROR_MESSAGE);
    case SET_PARSE_INVALID_EXPIRE:
        return resp_write_error(out_buf, INVALID_EXPIRE_MESSAGE);
    }
    if (store_value(ctx, cmd, &options) != DB_OK) {
        return resp_write_error(out_buf, OOM_MESSAGE);
    }
    return resp_write_simple(out_buf, "OK");
}

static enum buffer_result handle_get_command(const struct command_ctx *ctx,
                                             const struct resp_command *cmd, struct buffer *out_buf)
{
    const struct str *value = db_get(ctx->db, cmd->argv[1], ctx->now_ms);
    if (value == NULL) {
        return resp_write_null(out_buf);
    }
    return resp_write_bulk(out_buf, value);
}

/* Redis 처럼 키를 여러 개 받아 실제로 지운 수를 돌려준다. 키 하나면 :1 / :0 이다 */
static enum buffer_result handle_del_command(const struct command_ctx *ctx,
                                             const struct resp_command *cmd, struct buffer *out_buf)
{
    int64_t deleted_count = 0;
    for (size_t i = 1; i < cmd->argc; i++) {
        if (db_delete(ctx->db, cmd->argv[i], ctx->now_ms) == DB_OK) {
            deleted_count++;
        }
    }
    return resp_write_integer(out_buf, deleted_count);
}

/* 테스트 전용. Redis 의 ASYNC / SYNC 옵션은 받기만 하고, 해제는 항상 지연 해제로 한다 */
static enum buffer_result handle_flushall_command(const struct command_ctx *ctx,
                                                  const struct resp_command *cmd,
                                                  struct buffer *out_buf)
{
    if (!ctx->is_flush_enabled) {
        return resp_write_error(out_buf, "ERR FLUSHALL is disabled");
    }
    bool is_option_ok = cmd->argc == 1 || (cmd->argc == 2 && (name_equals(cmd->argv[1], "async") ||
                                                              name_equals(cmd->argv[1], "sync")));
    if (!is_option_ok) {
        return resp_write_error(out_buf, SYNTAX_ERROR_MESSAGE);
    }
    if (db_flush(ctx->db) != DB_OK) {
        return resp_write_error(out_buf, OOM_MESSAGE);
    }
    return resp_write_simple(out_buf, "OK");
}

static const struct command_spec *find_spec(const struct str *name)
{
    for (size_t i = 0; i < COMMAND_COUNT; i++) {
        if (name_equals(name, COMMAND_TABLE[i].name)) {
            return &COMMAND_TABLE[i];
        }
    }
    return NULL;
}

static bool is_arity_ok(const struct command_spec *spec, size_t argc)
{
    if (spec->arity >= 0) {
        return argc == (size_t)spec->arity;
    }
    return argc >= (size_t)-spec->arity;
}

/* 명령 이름과 옵션은 대소문자를 구분하지 않는다. 로케일에 따라 바뀌지 않게 ASCII 로만 비교한다 */
static bool name_equals(const struct str *arg, const char *name)
{
    size_t i = 0;
    for (; i < arg->len; i++) {
        if (name[i] == '\0' || to_lower_ascii(arg->data[i]) != name[i]) {
            return false;
        }
    }
    return name[i] == '\0';
}

static char to_lower_ascii(char c)
{
    if (c >= 'A' && c <= 'Z') {
        return (char)(c + ('a' - 'A'));
    }
    return c;
}

/* 응답 한 줄을 깨뜨릴 수 있는 제어 문자(\r, \n, \0 등)와 DEL 은 공백으로 바꾼다 */
static char to_printable(char c)
{
    unsigned char byte = (unsigned char)c;
    if (byte < ' ' || byte == ASCII_DEL) {
        return ' ';
    }
    return c;
}

/* 이름은 클라이언트가 보낸 바이트라 그대로 넣으면 응답 줄이 깨질 수 있다.
 * 128 바이트까지만 쓰고 제어 문자는 공백으로 바꾼다 */
static enum buffer_result reply_unknown_command(struct buffer *out_buf, const struct str *name)
{
    char safe_name[COMMAND_MAX_NAME_IN_ERROR + 1];
    size_t name_len = name->len < COMMAND_MAX_NAME_IN_ERROR ? name->len : COMMAND_MAX_NAME_IN_ERROR;
    for (size_t i = 0; i < name_len; i++) {
        safe_name[i] = to_printable(name->data[i]);
    }
    safe_name[name_len] = '\0';

    char message[ERROR_MESSAGE_BYTES];
    snprintf(message, sizeof(message), "ERR unknown command '%s'", safe_name);
    return resp_write_error(out_buf, message);
}

static enum buffer_result reply_wrong_arity(struct buffer *out_buf, const struct command_spec *spec)
{
    char message[ERROR_MESSAGE_BYTES];
    snprintf(message, sizeof(message), "ERR wrong number of arguments for '%s' command",
             spec->name);
    return resp_write_error(out_buf, message);
}

/* argv[3] 부터 옵션을 읽는다. 지금은 EX seconds 하나뿐이고, 두 번 오면 문법 오류다 */
static enum set_parse_result parse_set_options(const struct command_ctx *ctx,
                                               const struct resp_command *cmd,
                                               struct set_options *out_options)
{
    for (size_t i = SET_OPTION_START_IDX; i < cmd->argc; i += 2) {
        bool is_ex = name_equals(cmd->argv[i], "ex");
        if (!is_ex || out_options->has_expire || i + 1 >= cmd->argc) {
            return SET_PARSE_SYNTAX_ERROR;
        }
        if (!parse_expire_at_ms(cmd->argv[i + 1], ctx->now_ms, &out_options->expire_at_ms)) {
            return SET_PARSE_INVALID_EXPIRE;
        }
        out_options->has_expire = true;
    }
    return SET_PARSE_OK;
}

/* 초는 1 이상의 10진수만 받는다. 0, 음수, 숫자 외 문자, 그리고 now_ms 에 더했을 때
 * int64 를 넘는 값은 모두 잘못된 만료 시간이다 */
static bool parse_expire_at_ms(const struct str *seconds_arg, int64_t now_ms,
                               int64_t *out_expire_at_ms)
{
    if (seconds_arg->len == 0 || seconds_arg->len > MAX_EXPIRE_DIGITS) {
        return false;
    }
    int64_t seconds = 0;
    for (size_t i = 0; i < seconds_arg->len; i++) {
        char c = seconds_arg->data[i];
        if (c < '0' || c > '9') {
            return false;
        }
        seconds = seconds * DECIMAL_BASE + (c - '0');
    }
    if (seconds <= 0 || seconds > (INT64_MAX - now_ms) / COMMAND_MS_PER_SEC) {
        return false;
    }
    *out_expire_at_ms = now_ms + seconds * COMMAND_MS_PER_SEC;
    return true;
}

/* argv 는 명령이 소유하므로 db 에 넘길 복사본을 만든다. TTL 을 걸지 못하면 TTL 없는 키가
 * 남지 않도록 방금 저장한 키를 지운다 */
static enum db_result store_value(const struct command_ctx *ctx, const struct resp_command *cmd,
                                  const struct set_options *options)
{
    struct db_set_request req = {
        .key = str_dup(cmd->argv[1]),
        .value = str_dup(cmd->argv[2]),
        .now_ms = ctx->now_ms,
    };
    enum db_result result = DB_OUT_OF_MEMORY;
    if (req.key != NULL && req.value != NULL) {
        result = db_set(ctx->db, &req);
    }
    if (result != DB_OK) {
        str_destroy(req.value);
        str_destroy(req.key);
        return result;
    }
    if (options->has_expire &&
        db_set_expire(ctx->db, cmd->argv[1], options->expire_at_ms) != DB_OK) {
        db_delete(ctx->db, cmd->argv[1], ctx->now_ms);
        return DB_OUT_OF_MEMORY;
    }
    return DB_OK;
}
