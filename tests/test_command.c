#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "buffer.h"
#include "command.h"
#include "db.h"
#include "mem.h"
#include "resp.h"
#include "test.h"

enum {
    NOW_MS = 1000000,
    TEN_SEC_MS = 10 * COMMAND_MS_PER_SEC,
    SMALL_LIMIT_BYTES = 4096,
    BIG_VALUE_BYTES = 8192,
};

/* 테스트 하나가 쓰는 db 와 실행 문맥, 응답 버퍼 */
struct fixture {
    struct db *db;
    struct command_ctx ctx;
    struct buffer out;
};

static void fixture_init(struct fixture *fx)
{
    fx->db = db_create();
    fx->ctx = (struct command_ctx){.db = fx->db, .now_ms = NOW_MS, .is_flush_enabled = true};
    buffer_init(&fx->out);
}

static void fixture_deinit(struct fixture *fx)
{
    buffer_deinit(&fx->out);
    db_destroy(fx->db);
}

/* NULL 로 끝나는 텍스트 인자로 명령을 만든다. resp_command_deinit() 으로 해제한다 */
static struct resp_command make_command(const char *const *args)
{
    struct resp_command cmd = {0};
    size_t argc = 0;
    while (args[argc] != NULL) {
        argc++;
    }
    /* NOLINTNEXTLINE(bugprone-sizeof-expression) */
    cmd.argv = mem_calloc(argc, sizeof(struct str *));
    for (size_t i = 0; i < argc; i++) {
        cmd.argv[i] = str_create(args[i], strlen(args[i]));
    }
    cmd.argc = argc;
    return cmd;
}

/* 응답 버퍼를 비우고 명령을 실행한 뒤 응답이 expected 와 같은지 돌려준다 */
static bool run_expect(struct fixture *fx, const char *const *args, const char *expected)
{
    buffer_clear(&fx->out);
    struct resp_command cmd = make_command(args);
    enum command_result result = command_execute(&fx->ctx, &cmd, &fx->out);
    resp_command_deinit(&cmd);
    return result == COMMAND_OK && fx->out.len == strlen(expected) &&
           memcmp(fx->out.data, expected, fx->out.len) == 0;
}

static bool out_equals(const struct buffer *out, const char *data, size_t len)
{
    return out->len == len && memcmp(out->data, data, len) == 0;
}

static struct str *create_from_text(const char *text)
{
    return str_create(text, strlen(text));
}

/* ---- 9.1 응답 직렬화 ---- */

static void test_write_simple_error_and_null(void)
{
    struct buffer out;
    buffer_init(&out);
    CHECK(resp_write_simple(&out, "OK") == BUFFER_OK);
    CHECK(resp_write_error(&out, "ERR boom") == BUFFER_OK);
    CHECK(resp_write_null(&out) == BUFFER_OK);
    static const char expected[] = "+OK\r\n-ERR boom\r\n$-1\r\n";
    CHECK(out_equals(&out, expected, sizeof(expected) - 1));
    buffer_deinit(&out);
}

static void test_write_integer_values(void)
{
    struct buffer out;
    buffer_init(&out);
    CHECK(resp_write_integer(&out, 0) == BUFFER_OK);
    CHECK(resp_write_integer(&out, -5) == BUFFER_OK);
    CHECK(resp_write_integer(&out, INT64_MAX) == BUFFER_OK);
    static const char expected[] = ":0\r\n:-5\r\n:9223372036854775807\r\n";
    CHECK(out_equals(&out, expected, sizeof(expected) - 1));
    buffer_deinit(&out);
}

static void test_write_bulk_and_empty_bulk(void)
{
    struct buffer out;
    buffer_init(&out);
    struct str *value = create_from_text("value");
    struct str *empty = create_from_text("");
    CHECK(resp_write_bulk(&out, value) == BUFFER_OK);
    CHECK(resp_write_bulk(&out, empty) == BUFFER_OK);
    static const char expected[] = "$5\r\nvalue\r\n$0\r\n\r\n";
    CHECK(out_equals(&out, expected, sizeof(expected) - 1));
    str_destroy(empty);
    str_destroy(value);
    buffer_deinit(&out);
}

static void test_write_bulk_with_nul_keeps_length(void)
{
    static const char data[] = {'a', '\0', '\r', '\n'};
    static const char expected[] = {'$', '4', '\r', '\n', 'a', '\0', '\r', '\n', '\r', '\n'};
    struct buffer out;
    buffer_init(&out);
    struct str *value = str_create(data, sizeof(data));
    CHECK(resp_write_bulk(&out, value) == BUFFER_OK);
    CHECK(out_equals(&out, expected, sizeof(expected)));
    str_destroy(value);
    buffer_deinit(&out);
}

/* ---- 9.2 명령 테이블 ---- */

static void test_command_name_is_case_insensitive(void)
{
    static const char *const lower[] = {"set", "k", "v", NULL};
    static const char *const mixed[] = {"Set", "k", "v", NULL};
    static const char *const upper[] = {"SET", "k", "v", NULL};
    struct fixture fx;
    fixture_init(&fx);
    CHECK(run_expect(&fx, lower, "+OK\r\n"));
    CHECK(run_expect(&fx, mixed, "+OK\r\n"));
    CHECK(run_expect(&fx, upper, "+OK\r\n"));
    fixture_deinit(&fx);
}

static void test_unknown_command_returns_error(void)
{
    static const char *const args[] = {"foo", "bar", NULL};
    struct fixture fx;
    fixture_init(&fx);
    CHECK(run_expect(&fx, args, "-ERR unknown command 'foo'\r\n"));
    fixture_deinit(&fx);
}

/* 이름에 \r\n 이 있어도 응답은 한 줄이어야 한다 */
static void test_unknown_command_name_with_crlf_stays_one_line(void)
{
    static const char *const args[] = {"a\r\nb", NULL};
    struct fixture fx;
    fixture_init(&fx);
    CHECK(run_expect(&fx, args, "-ERR unknown command 'a  b'\r\n"));
    fixture_deinit(&fx);
}

static void test_wrong_arity_returns_error(void)
{
    static const char *const too_few[] = {"GET", NULL};
    static const char *const too_many[] = {"GET", "a", "b", NULL};
    static const char *const set_too_few[] = {"SET", "k", NULL};
    struct fixture fx;
    fixture_init(&fx);
    CHECK(run_expect(&fx, too_few, "-ERR wrong number of arguments for 'get' command\r\n"));
    CHECK(run_expect(&fx, too_many, "-ERR wrong number of arguments for 'get' command\r\n"));
    CHECK(run_expect(&fx, set_too_few, "-ERR wrong number of arguments for 'set' command\r\n"));
    fixture_deinit(&fx);
}

/* ---- 9.3 SET / GET / DEL ---- */

static void test_set_then_get_returns_value(void)
{
    static const char *const set_args[] = {"SET", "k", "v", NULL};
    static const char *const get_args[] = {"GET", "k", NULL};
    struct fixture fx;
    fixture_init(&fx);
    CHECK(run_expect(&fx, set_args, "+OK\r\n"));
    CHECK(run_expect(&fx, get_args, "$1\r\nv\r\n"));
    fixture_deinit(&fx);
}

static void test_get_missing_key_returns_null(void)
{
    static const char *const args[] = {"GET", "missing", NULL};
    struct fixture fx;
    fixture_init(&fx);
    CHECK(run_expect(&fx, args, "$-1\r\n"));
    fixture_deinit(&fx);
}

static void test_del_existing_and_missing_keys(void)
{
    static const char *const set_a[] = {"SET", "a", "1", NULL};
    static const char *const set_b[] = {"SET", "b", "2", NULL};
    static const char *const del_a[] = {"DEL", "a", NULL};
    static const char *const del_again[] = {"DEL", "a", NULL};
    static const char *const del_many[] = {"DEL", "a", "b", "c", NULL};
    struct fixture fx;
    fixture_init(&fx);
    CHECK(run_expect(&fx, set_a, "+OK\r\n"));
    CHECK(run_expect(&fx, del_a, ":1\r\n"));
    CHECK(run_expect(&fx, del_again, ":0\r\n"));
    CHECK(run_expect(&fx, set_a, "+OK\r\n"));
    CHECK(run_expect(&fx, set_b, "+OK\r\n"));
    CHECK(run_expect(&fx, del_many, ":2\r\n"));
    fixture_deinit(&fx);
}

static void test_set_with_ex_expires_after_ttl(void)
{
    static const char *const set_args[] = {"SET", "k", "v", "EX", "10", NULL};
    static const char *const get_args[] = {"GET", "k", NULL};
    struct fixture fx;
    fixture_init(&fx);
    CHECK(run_expect(&fx, set_args, "+OK\r\n"));
    fx.ctx.now_ms = NOW_MS + TEN_SEC_MS - COMMAND_MS_PER_SEC;
    CHECK(run_expect(&fx, get_args, "$1\r\nv\r\n"));
    fx.ctx.now_ms = NOW_MS + TEN_SEC_MS;
    CHECK(run_expect(&fx, get_args, "$-1\r\n"));
    fixture_deinit(&fx);
}

static void test_set_with_lowercase_ex_works(void)
{
    static const char *const set_args[] = {"set", "k", "v", "ex", "10", NULL};
    static const char *const get_args[] = {"GET", "k", NULL};
    struct fixture fx;
    fixture_init(&fx);
    CHECK(run_expect(&fx, set_args, "+OK\r\n"));
    fx.ctx.now_ms = NOW_MS + TEN_SEC_MS;
    CHECK(run_expect(&fx, get_args, "$-1\r\n"));
    fixture_deinit(&fx);
}

/* 잘못된 만료 시간이면 에러를 돌려주고 키를 저장하지 않는다 */
static void check_invalid_expire(struct fixture *fx, const char *seconds)
{
    const char *const set_args[] = {"SET", "k", "v", "EX", seconds, NULL};
    CHECK(run_expect(fx, set_args, "-ERR invalid expire time in 'set' command\r\n"));
    CHECK(db_count(fx->db) == 0);
}

static void test_set_with_invalid_expire_returns_error(void)
{
    struct fixture fx;
    fixture_init(&fx);
    check_invalid_expire(&fx, "0");
    check_invalid_expire(&fx, "-1");
    check_invalid_expire(&fx, "abc");
    check_invalid_expire(&fx, "");
    check_invalid_expire(&fx, "10s");
    check_invalid_expire(&fx, "99999999999999999999");
    check_invalid_expire(&fx, "9223372036854775");
    fixture_deinit(&fx);
}

static void test_set_with_bad_options_returns_syntax_error(void)
{
    static const char *const unknown_option[] = {"SET", "k", "v", "PX", "10", NULL};
    static const char *const missing_seconds[] = {"SET", "k", "v", "EX", NULL};
    static const char *const ex_twice[] = {"SET", "k", "v", "EX", "1", "EX", "2", NULL};
    struct fixture fx;
    fixture_init(&fx);
    CHECK(run_expect(&fx, unknown_option, "-ERR syntax error\r\n"));
    CHECK(run_expect(&fx, missing_seconds, "-ERR syntax error\r\n"));
    CHECK(run_expect(&fx, ex_twice, "-ERR syntax error\r\n"));
    CHECK(db_count(fx.db) == 0);
    fixture_deinit(&fx);
}

static void test_set_over_memory_limit_returns_oom(void)
{
    static const char *const small_args[] = {"SET", "small", "v", NULL};
    struct fixture fx;
    fixture_init(&fx);
    db_set_max_bytes(fx.db, mem_used_bytes() + SMALL_LIMIT_BYTES);
    CHECK(run_expect(&fx, small_args, "+OK\r\n"));

    char *big_value = mem_alloc(BIG_VALUE_BYTES + 1);
    memset(big_value, 'x', BIG_VALUE_BYTES);
    big_value[BIG_VALUE_BYTES] = '\0';
    const char *const big_args[] = {"SET", "big", big_value, NULL};
    CHECK(
        run_expect(&fx, big_args, "-OOM command not allowed when used memory > 'maxmemory'.\r\n"));
    mem_free(big_value);
    CHECK(db_count(fx.db) == 1);
    fixture_deinit(&fx);
}

/* ---- 9.3 FLUSHALL ---- */

static void test_flushall_enabled_clears_keys(void)
{
    static const char *const set_args[] = {"SET", "k", "v", NULL};
    static const char *const flush_args[] = {"FLUSHALL", NULL};
    static const char *const flush_async[] = {"flushall", "async", NULL};
    struct fixture fx;
    fixture_init(&fx);
    CHECK(run_expect(&fx, set_args, "+OK\r\n"));
    CHECK(run_expect(&fx, flush_args, "+OK\r\n"));
    CHECK(db_count(fx.db) == 0);
    CHECK(run_expect(&fx, flush_async, "+OK\r\n"));
    fixture_deinit(&fx);
}

static void test_flushall_disabled_returns_error_and_keeps_keys(void)
{
    static const char *const set_args[] = {"SET", "k", "v", NULL};
    static const char *const flush_args[] = {"FLUSHALL", NULL};
    struct fixture fx;
    fixture_init(&fx);
    fx.ctx.is_flush_enabled = false;
    CHECK(run_expect(&fx, set_args, "+OK\r\n"));
    CHECK(run_expect(&fx, flush_args, "-ERR FLUSHALL is disabled\r\n"));
    CHECK(db_count(fx.db) == 1);
    fixture_deinit(&fx);
}

static void test_flushall_unknown_option_returns_syntax_error(void)
{
    static const char *const args[] = {"FLUSHALL", "NOW", NULL};
    struct fixture fx;
    fixture_init(&fx);
    CHECK(run_expect(&fx, args, "-ERR syntax error\r\n"));
    fixture_deinit(&fx);
}

/* ---- 파서와 함께 ---- */

static void test_parsed_command_executes(void)
{
    static const char request[] = "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n";
    struct fixture fx;
    fixture_init(&fx);
    struct resp_command cmd;
    CHECK(resp_parse(request, sizeof(request) - 1, &cmd) == RESP_PARSE_DONE);
    CHECK(command_execute(&fx.ctx, &cmd, &fx.out) == COMMAND_OK);
    CHECK(out_equals(&fx.out, "+OK\r\n", strlen("+OK\r\n")));
    resp_command_deinit(&cmd);
    fixture_deinit(&fx);
}

static void test_commands_return_used_bytes_to_baseline(void)
{
    static const char *const set_args[] = {"SET", "k", "v", "EX", "10", NULL};
    static const char *const get_args[] = {"GET", "k", NULL};
    static const char *const unknown_args[] = {"NOPE", NULL};
    size_t baseline_bytes = mem_used_bytes();
    struct fixture fx;
    fixture_init(&fx);
    CHECK(run_expect(&fx, set_args, "+OK\r\n"));
    CHECK(run_expect(&fx, get_args, "$1\r\nv\r\n"));
    CHECK(run_expect(&fx, unknown_args, "-ERR unknown command 'NOPE'\r\n"));
    fixture_deinit(&fx);
    CHECK(mem_used_bytes() == baseline_bytes);
}

static void run_serialize_tests(void)
{
    RUN(test_write_simple_error_and_null);
    RUN(test_write_integer_values);
    RUN(test_write_bulk_and_empty_bulk);
    RUN(test_write_bulk_with_nul_keeps_length);
}

static void run_table_tests(void)
{
    RUN(test_command_name_is_case_insensitive);
    RUN(test_unknown_command_returns_error);
    RUN(test_unknown_command_name_with_crlf_stays_one_line);
    RUN(test_wrong_arity_returns_error);
}

static void run_command_tests(void)
{
    RUN(test_set_then_get_returns_value);
    RUN(test_get_missing_key_returns_null);
    RUN(test_del_existing_and_missing_keys);
    RUN(test_set_with_ex_expires_after_ttl);
    RUN(test_set_with_lowercase_ex_works);
    RUN(test_set_with_invalid_expire_returns_error);
    RUN(test_set_with_bad_options_returns_syntax_error);
    RUN(test_set_over_memory_limit_returns_oom);
    RUN(test_flushall_enabled_clears_keys);
    RUN(test_flushall_disabled_returns_error_and_keeps_keys);
    RUN(test_flushall_unknown_option_returns_syntax_error);
    RUN(test_parsed_command_executes);
    RUN(test_commands_return_used_bytes_to_baseline);
}

int main(void)
{
    run_serialize_tests();
    run_table_tests();
    run_command_tests();
    TEST_DONE();
    return 0;
}
