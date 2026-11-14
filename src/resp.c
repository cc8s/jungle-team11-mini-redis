#include "resp.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "mem.h"

enum {
    DECIMAL_BASE = 10,
    HEADER_BUF_BYTES = 32,
};

static const char CRLF[] = "\r\n";

/* 버퍼를 앞에서부터 읽는 위치. buf 는 읽기만 한다 */
struct reader {
    const char *buf;
    size_t len;
    size_t pos;
};

/* 버퍼 안의 한 구간. 복사하지 않고 가리키기만 한다 */
struct byte_span {
    const char *data;
    size_t len;
};

static enum resp_parse_result skip_blank_bytes(struct reader *r);
static enum resp_parse_result walk_multibulk(struct reader *r, struct resp_command *cmd);
static enum resp_parse_result read_bulk(struct reader *r, struct byte_span *out_span);
static enum resp_parse_result read_number(struct reader *r, size_t max_value, size_t *out_value);
static enum resp_parse_result expect_byte(struct reader *r, char expected);
static enum resp_parse_result expect_crlf(struct reader *r);
static enum resp_parse_result parse_inline(struct reader *r, struct resp_command *cmd);
static enum resp_parse_result find_inline_line(struct reader *r, struct byte_span *out_line);
static size_t split_inline(const struct byte_span *line, struct str **argv);
static bool next_inline_token(const struct byte_span *line, size_t *pos,
                              struct byte_span *out_token);
static bool is_inline_space(char c);
static enum resp_parse_result add_arg(struct resp_command *cmd, const struct byte_span *span);
static enum buffer_result write_line(struct buffer *out_buf, char type, const char *text);
static enum buffer_result write_parts(struct buffer *out_buf, const struct byte_span *parts,
                                      size_t part_count);

/* 상태를 들고 다니지 않고 매번 처음부터 다시 읽는다. 첫 번째 훑기는 할당 없이 검사만 해서,
 * 덜 온 명령 때문에 할당했다 버리는 일이 없다. 명령이 다 왔을 때만 두 번째 훑기로 인자를 만든다 */
enum resp_parse_result resp_parse(const char *buf, size_t len, struct resp_command *out_cmd)
{
    *out_cmd = (struct resp_command){0};
    struct reader r = {.buf = buf, .len = len, .pos = 0};
    enum resp_parse_result result = skip_blank_bytes(&r);
    if (result != RESP_PARSE_DONE) {
        return result;
    }
    if (r.buf[r.pos] != '*') {
        return parse_inline(&r, out_cmd);
    }

    struct reader check = r;
    result = walk_multibulk(&check, NULL);
    if (result != RESP_PARSE_DONE) {
        return result;
    }
    result = walk_multibulk(&r, out_cmd);
    if (result != RESP_PARSE_DONE) {
        resp_command_deinit(out_cmd);
        return result;
    }
    out_cmd->consumed_bytes = r.pos;
    return RESP_PARSE_DONE;
}

void resp_command_deinit(struct resp_command *cmd)
{
    for (size_t i = 0; i < cmd->argc; i++) {
        str_destroy(cmd->argv[i]);
    }
    mem_free(cmd->argv);
    *cmd = (struct resp_command){0};
}

enum buffer_result resp_write_simple(struct buffer *out_buf, const char *text)
{
    return write_line(out_buf, '+', text);
}

enum buffer_result resp_write_error(struct buffer *out_buf, const char *text)
{
    return write_line(out_buf, '-', text);
}

enum buffer_result resp_write_integer(struct buffer *out_buf, int64_t value)
{
    char line[HEADER_BUF_BYTES];
    int len = snprintf(line, sizeof(line), ":%" PRId64 "\r\n", value);
    const struct byte_span part = {.data = line, .len = (size_t)len};
    return write_parts(out_buf, &part, 1);
}

enum buffer_result resp_write_bulk(struct buffer *out_buf, const struct str *value)
{
    char header[HEADER_BUF_BYTES];
    int header_len = snprintf(header, sizeof(header), "$%zu\r\n", value->len);
    const struct byte_span parts[] = {
        {.data = header, .len = (size_t)header_len},
        {.data = value->data, .len = value->len},
        {.data = CRLF, .len = sizeof(CRLF) - 1},
    };
    return write_parts(out_buf, parts, sizeof(parts) / sizeof(parts[0]));
}

enum buffer_result resp_write_null(struct buffer *out_buf)
{
    static const char null_bulk[] = "$-1\r\n";
    const struct byte_span part = {.data = null_bulk, .len = sizeof(null_bulk) - 1};
    return write_parts(out_buf, &part, 1);
}

/* 명령 사이의 빈 줄과 공백은 건너뛴다 (telnet 에서 Enter 만 친 경우, Redis 와 동일).
 * 남는 바이트가 없으면 NEED_MORE. 공백만 끝없이 보내 버퍼를 키우지 못하게 상한을 둔다 */
static enum resp_parse_result skip_blank_bytes(struct reader *r)
{
    while (r->pos < r->len &&
           (r->buf[r->pos] == '\r' || r->buf[r->pos] == '\n' || is_inline_space(r->buf[r->pos]))) {
        r->pos++;
    }
    if (r->pos > RESP_MAX_INLINE_BYTES) {
        return RESP_PARSE_ERROR;
    }
    return r->pos < r->len ? RESP_PARSE_DONE : RESP_PARSE_NEED_MORE;
}

/* *<argc>\r\n 뒤에 bulk string argc 개. cmd 가 NULL 이면 검사만 하고 할당하지 않는다 */
static enum resp_parse_result walk_multibulk(struct reader *r, struct resp_command *cmd)
{
    size_t argc = 0;
    enum resp_parse_result result = expect_byte(r, '*');
    if (result == RESP_PARSE_DONE) {
        result = read_number(r, RESP_MAX_ARGS, &argc);
    }
    if (result != RESP_PARSE_DONE) {
        return result;
    }
    if (argc == 0) {
        return RESP_PARSE_ERROR;
    }
    if (cmd != NULL) {
        /* NOLINTNEXTLINE(bugprone-sizeof-expression) */
        cmd->argv = mem_calloc(argc, sizeof(struct str *));
        if (cmd->argv == NULL) {
            return RESP_PARSE_OUT_OF_MEMORY;
        }
    }

    for (size_t i = 0; i < argc; i++) {
        struct byte_span span = {0};
        result = read_bulk(r, &span);
        if (result == RESP_PARSE_DONE && cmd != NULL) {
            result = add_arg(cmd, &span);
        }
        if (result != RESP_PARSE_DONE) {
            return result;
        }
    }
    return RESP_PARSE_DONE;
}

/* $<len>\r\n<data>\r\n. data 는 길이로만 자르므로 안에 \0 이나 \r\n 이 있어도 값의 일부다 */
static enum resp_parse_result read_bulk(struct reader *r, struct byte_span *out_span)
{
    size_t data_len = 0;
    enum resp_parse_result result = expect_byte(r, '$');
    if (result == RESP_PARSE_DONE) {
        result = read_number(r, RESP_MAX_BULK_BYTES, &data_len);
    }
    if (result != RESP_PARSE_DONE) {
        return result;
    }
    if (r->len - r->pos < data_len) {
        return RESP_PARSE_NEED_MORE;
    }
    out_span->data = r->buf + r->pos;
    out_span->len = data_len;
    r->pos += data_len;
    return expect_crlf(r);
}

/* 0 이상 max_value 이하의 10진수 뒤에 \r\n. 부호와 숫자 외 문자는 받지 않는다.
 * 0 만 끝없이 이어 보내는 경우도 막도록 자릿수에 상한을 둔다 */
static enum resp_parse_result read_number(struct reader *r, size_t max_value, size_t *out_value)
{
    size_t value = 0;
    size_t digit_count = 0;
    while (r->pos < r->len && r->buf[r->pos] >= '0' && r->buf[r->pos] <= '9') {
        size_t digit = (size_t)(r->buf[r->pos] - '0');
        if (value > max_value / DECIMAL_BASE || ++digit_count > RESP_MAX_NUMBER_DIGITS) {
            return RESP_PARSE_ERROR;
        }
        value *= DECIMAL_BASE;
        if (digit > max_value - value) {
            return RESP_PARSE_ERROR;
        }
        value += digit;
        r->pos++;
    }
    if (r->pos < r->len && digit_count == 0) {
        return RESP_PARSE_ERROR;
    }
    enum resp_parse_result result = expect_crlf(r);
    if (result == RESP_PARSE_DONE) {
        *out_value = value;
    }
    return result;
}

static enum resp_parse_result expect_byte(struct reader *r, char expected)
{
    if (r->pos >= r->len) {
        return RESP_PARSE_NEED_MORE;
    }
    if (r->buf[r->pos] != expected) {
        return RESP_PARSE_ERROR;
    }
    r->pos++;
    return RESP_PARSE_DONE;
}

static enum resp_parse_result expect_crlf(struct reader *r)
{
    enum resp_parse_result result = expect_byte(r, '\r');
    if (result == RESP_PARSE_DONE) {
        result = expect_byte(r, '\n');
    }
    return result;
}

/* nc · telnet 용: 줄 끝(\n, 앞의 \r 은 버린다)까지를 공백으로 나눠 인자로 쓴다.
 * 따옴표 처리는 하지 않는다. 인자 수를 먼저 세고 배열을 한 번에 할당한다 */
static enum resp_parse_result parse_inline(struct reader *r, struct resp_command *cmd)
{
    struct byte_span line = {0};
    enum resp_parse_result result = find_inline_line(r, &line);
    if (result != RESP_PARSE_DONE) {
        return result;
    }
    size_t argc = split_inline(&line, NULL);
    if (argc > RESP_MAX_ARGS) {
        return RESP_PARSE_ERROR;
    }

    /* NOLINTNEXTLINE(bugprone-sizeof-expression) */
    cmd->argv = mem_calloc(argc, sizeof(struct str *));
    if (cmd->argv == NULL) {
        return RESP_PARSE_OUT_OF_MEMORY;
    }
    cmd->argc = split_inline(&line, cmd->argv);
    if (cmd->argc != argc) {
        resp_command_deinit(cmd);
        return RESP_PARSE_OUT_OF_MEMORY;
    }
    cmd->consumed_bytes = r->pos;
    return RESP_PARSE_DONE;
}

/* r->pos 부터 \n 까지를 한 줄로 돌려주고 r->pos 를 \n 다음으로 옮긴다.
 * 줄 끝의 \r 은 빼고, \n 은 줄에 넣지 않는다 */
static enum resp_parse_result find_inline_line(struct reader *r, struct byte_span *out_line)
{
    size_t available = r->len - r->pos;
    size_t search_len = available < RESP_MAX_INLINE_BYTES ? available : RESP_MAX_INLINE_BYTES;
    const char *newline = memchr(r->buf + r->pos, '\n', search_len);
    if (newline == NULL) {
        return available >= RESP_MAX_INLINE_BYTES ? RESP_PARSE_ERROR : RESP_PARSE_NEED_MORE;
    }
    out_line->data = r->buf + r->pos;
    out_line->len = (size_t)(newline - out_line->data);
    r->pos += out_line->len + 1;
    if (out_line->len > 0 && out_line->data[out_line->len - 1] == '\r') {
        out_line->len--;
    }
    return RESP_PARSE_DONE;
}

/* 공백으로 나눈 토큰 수를 센다. argv 가 있으면 각 토큰을 str 로 만들어 담고, 할당에
 * 실패하면 거기서 멈춘다. 그래서 반환값이 센 수보다 작으면 할당 실패다 */
static size_t split_inline(const struct byte_span *line, struct str **argv)
{
    size_t token_count = 0;
    size_t pos = 0;
    struct byte_span token = {0};
    while (next_inline_token(line, &pos, &token)) {
        if (argv != NULL && (argv[token_count] = str_create(token.data, token.len)) == NULL) {
            return token_count;
        }
        token_count++;
    }
    return token_count;
}

/* *pos 부터 공백을 건너뛰고 다음 토큰을 out_token 에 담는다. 토큰이 없으면 false */
static bool next_inline_token(const struct byte_span *line, size_t *pos,
                              struct byte_span *out_token)
{
    size_t i = *pos;
    while (i < line->len && is_inline_space(line->data[i])) {
        i++;
    }
    size_t start = i;
    while (i < line->len && !is_inline_space(line->data[i])) {
        i++;
    }
    *pos = i;
    out_token->data = line->data + start;
    out_token->len = i - start;
    return out_token->len > 0;
}

static bool is_inline_space(char c)
{
    return c == ' ' || c == '\t';
}

/* deinit 이 지금까지 만든 인자만 해제하도록 성공할 때마다 argc 를 늘린다 */
static enum resp_parse_result add_arg(struct resp_command *cmd, const struct byte_span *span)
{
    struct str *arg = str_create(span->data, span->len);
    if (arg == NULL) {
        return RESP_PARSE_OUT_OF_MEMORY;
    }
    cmd->argv[cmd->argc++] = arg;
    return RESP_PARSE_DONE;
}

static enum buffer_result write_line(struct buffer *out_buf, char type, const char *text)
{
    const struct byte_span parts[] = {
        {.data = &type, .len = 1},
        {.data = text, .len = strlen(text)},
        {.data = CRLF, .len = sizeof(CRLF) - 1},
    };
    return write_parts(out_buf, parts, sizeof(parts) / sizeof(parts[0]));
}

/* 조각 중 하나라도 붙이지 못하면 이번에 붙인 만큼 len 을 되돌려 응답이 반쯤 남지 않게 한다 */
static enum buffer_result write_parts(struct buffer *out_buf, const struct byte_span *parts,
                                      size_t part_count)
{
    size_t start_len = out_buf->len;
    for (size_t i = 0; i < part_count; i++) {
        if (buffer_append(out_buf, parts[i].data, parts[i].len) != BUFFER_OK) {
            out_buf->len = start_len;
            return BUFFER_OUT_OF_MEMORY;
        }
    }
    return BUFFER_OK;
}
