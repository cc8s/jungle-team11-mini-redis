#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "mem.h"
#include "resp.h"
#include "test.h"

enum {
    NUMBER_BUF_BYTES = 64,
    MANY_ZERO_COUNT = 25,
};

/* 텍스트 요청을 파싱한다. 텍스트 안에 \0 이 없을 때만 쓴다 */
static enum resp_parse_result parse_text(const char *text, struct resp_command *out_cmd)
{
    return resp_parse(text, strlen(text), out_cmd);
}

static bool arg_equals(const struct str *arg, const char *text)
{
    return arg->len == strlen(text) && memcmp(arg->data, text, arg->len) == 0;
}

/* argv 가 NULL 로 끝나는 expected 와 순서 · 개수까지 같으면 true */
static bool argv_equals(const struct resp_command *cmd, const char *const *expected)
{
    size_t i = 0;
    for (; expected[i] != NULL; i++) {
        if (i >= cmd->argc || !arg_equals(cmd->argv[i], expected[i])) {
            return false;
        }
    }
    return i == cmd->argc;
}

/* text 전체가 명령 하나로 파싱되고 argv 가 expected 와 같은지 확인한다 */
static void check_parses_to(const char *text, const char *const *expected)
{
    struct resp_command cmd;
    CHECK(parse_text(text, &cmd) == RESP_PARSE_DONE);
    CHECK(argv_equals(&cmd, expected));
    CHECK(cmd.consumed_bytes == strlen(text));
    resp_command_deinit(&cmd);
}

static void check_parse_result(const char *text, enum resp_parse_result expected)
{
    struct resp_command cmd;
    CHECK(parse_text(text, &cmd) == expected);
    CHECK(cmd.argc == 0 && cmd.argv == NULL);
    resp_command_deinit(&cmd);
}

static void test_parse_set_returns_args(void)
{
    static const char *const expected[] = {"SET", "k", "v", NULL};
    check_parses_to("*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n", expected);
}

static void test_parse_set_with_ex_returns_args(void)
{
    static const char *const expected[] = {"SET", "k", "v", "EX", "10", NULL};
    check_parses_to("*5\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n$2\r\nEX\r\n$2\r\n10\r\n", expected);
}

static void test_parse_get_and_del_return_args(void)
{
    static const char *const get_expected[] = {"GET", "k", NULL};
    static const char *const del_expected[] = {"DEL", "k", NULL};
    check_parses_to("*2\r\n$3\r\nGET\r\n$1\r\nk\r\n", get_expected);
    check_parses_to("*2\r\n$3\r\nDEL\r\n$1\r\nk\r\n", del_expected);
}

static void test_parse_inline_returns_args(void)
{
    static const char *const expected[] = {"GET", "k", NULL};
    check_parses_to("GET k\r\n", expected);
    check_parses_to("GET k\n", expected);
}

static void test_parse_inline_splits_on_spaces_and_tabs(void)
{
    static const char *const expected[] = {"SET", "k", "v", NULL};
    check_parses_to("  SET   k\tv \r\n", expected);
}

static void test_parse_skips_blank_lines_before_command(void)
{
    static const char *const expected[] = {"GET", "k", NULL};
    check_parses_to("\r\n\n\r\n*2\r\n$3\r\nGET\r\n$1\r\nk\r\n", expected);
}

static void test_parse_only_blank_lines_needs_more(void)
{
    check_parse_result("\r\n\r\n", RESP_PARSE_NEED_MORE);
    check_parse_result("", RESP_PARSE_NEED_MORE);
}

/* 앞부분만 온 모든 길이에서 NEED_MORE 이고, 마지막 바이트가 와야 DONE 이다 */
static void check_done_only_at_last_byte(const char *text)
{
    size_t len = strlen(text);
    for (size_t n = 0; n < len; n++) {
        struct resp_command cmd;
        CHECK(resp_parse(text, n, &cmd) == RESP_PARSE_NEED_MORE);
        resp_command_deinit(&cmd);
    }
    struct resp_command cmd;
    CHECK(resp_parse(text, len, &cmd) == RESP_PARSE_DONE);
    resp_command_deinit(&cmd);
}

static void test_parse_byte_by_byte_done_only_at_last_byte(void)
{
    check_done_only_at_last_byte("*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$5\r\nvalue\r\n");
    check_done_only_at_last_byte("SET k value\r\n");
}

static void test_parse_pipelined_commands_in_order(void)
{
    static const char text[] = "*2\r\n$3\r\nGET\r\n$1\r\na\r\n*2\r\n$3\r\nDEL\r\n$1\r\nb\r\n";
    static const char *const first_expected[] = {"GET", "a", NULL};
    static const char *const second_expected[] = {"DEL", "b", NULL};
    size_t first_len = strlen("*2\r\n$3\r\nGET\r\n$1\r\na\r\n");

    struct resp_command cmd;
    CHECK(parse_text(text, &cmd) == RESP_PARSE_DONE);
    CHECK(argv_equals(&cmd, first_expected));
    CHECK(cmd.consumed_bytes == first_len);
    resp_command_deinit(&cmd);

    CHECK(resp_parse(text + first_len, strlen(text) - first_len, &cmd) == RESP_PARSE_DONE);
    CHECK(argv_equals(&cmd, second_expected));
    CHECK(cmd.consumed_bytes == strlen(text) - first_len);
    resp_command_deinit(&cmd);
}

static void test_parse_pipelined_inline_commands_in_order(void)
{
    static const char text[] = "GET a\r\nDEL b\n";
    struct resp_command cmd;
    CHECK(parse_text(text, &cmd) == RESP_PARSE_DONE);
    CHECK(cmd.consumed_bytes == strlen("GET a\r\n"));
    resp_command_deinit(&cmd);
    CHECK(parse_text(text + strlen("GET a\r\n"), &cmd) == RESP_PARSE_DONE);
    CHECK(cmd.consumed_bytes == strlen("DEL b\n"));
    resp_command_deinit(&cmd);
}

static void test_parse_value_with_nul_and_crlf_keeps_bytes(void)
{
    static const char text[] = "*2\r\n$3\r\nGET\r\n$5\r\na\0\r\nb\r\n";
    static const char value[] = {'a', '\0', '\r', '\n', 'b'};
    struct resp_command cmd;
    CHECK(resp_parse(text, sizeof(text) - 1, &cmd) == RESP_PARSE_DONE);
    CHECK(cmd.argc == 2);
    CHECK(cmd.argv[1]->len == sizeof(value));
    CHECK(memcmp(cmd.argv[1]->data, value, sizeof(value)) == 0);
    resp_command_deinit(&cmd);
}

static void test_parse_empty_bulk_returns_zero_length_arg(void)
{
    struct resp_command cmd;
    CHECK(parse_text("*2\r\n$3\r\nGET\r\n$0\r\n\r\n", &cmd) == RESP_PARSE_DONE);
    CHECK(cmd.argc == 2);
    CHECK(cmd.argv[1]->len == 0);
    resp_command_deinit(&cmd);
}

static void test_parse_bulk_without_trailing_crlf_returns_error(void)
{
    check_parse_result("*1\r\n$1\r\nab\r\n", RESP_PARSE_ERROR);
    check_parse_result("*1\r\n$1\r\na\rX", RESP_PARSE_ERROR);
}

static void test_parse_bad_numbers_return_error(void)
{
    check_parse_result("*x\r\n", RESP_PARSE_ERROR);
    check_parse_result("*-1\r\n", RESP_PARSE_ERROR);
    check_parse_result("*0\r\n", RESP_PARSE_ERROR);
    check_parse_result("*1x\r\n", RESP_PARSE_ERROR);
    check_parse_result("*1\rX", RESP_PARSE_ERROR);
    check_parse_result("*1\r\n$-1\r\n", RESP_PARSE_ERROR);
    check_parse_result("*1\r\n$abc\r\n", RESP_PARSE_ERROR);
    check_parse_result("*1\r\n$\r\n", RESP_PARSE_ERROR);
    check_parse_result("*99999999999999999999999\r\n", RESP_PARSE_ERROR);
}

static void test_parse_many_leading_zeros_returns_error(void)
{
    char text[NUMBER_BUF_BYTES] = "*";
    memset(text + 1, '0', MANY_ZERO_COUNT);
    check_parse_result(text, RESP_PARSE_ERROR);
}

static void test_parse_array_element_not_bulk_returns_error(void)
{
    check_parse_result("*1\r\n+OK\r\n", RESP_PARSE_ERROR);
}

static void test_parse_partial_numbers_need_more(void)
{
    check_parse_result("*1", RESP_PARSE_NEED_MORE);
    check_parse_result("*12\r", RESP_PARSE_NEED_MORE);
    check_parse_result("*1\r\n$", RESP_PARSE_NEED_MORE);
    check_parse_result("*1\r\n$10\r\nabc", RESP_PARSE_NEED_MORE);
}

static void test_parse_arg_count_limit(void)
{
    char text[NUMBER_BUF_BYTES];
    snprintf(text, sizeof(text), "*%d\r\n", RESP_MAX_ARGS);
    check_parse_result(text, RESP_PARSE_NEED_MORE);
    snprintf(text, sizeof(text), "*%d\r\n", RESP_MAX_ARGS + 1);
    check_parse_result(text, RESP_PARSE_ERROR);
}

static void test_parse_bulk_size_limit(void)
{
    char text[NUMBER_BUF_BYTES];
    snprintf(text, sizeof(text), "*1\r\n$%d\r\n", RESP_MAX_BULK_BYTES);
    check_parse_result(text, RESP_PARSE_NEED_MORE);
    snprintf(text, sizeof(text), "*1\r\n$%d\r\n", RESP_MAX_BULK_BYTES + 1);
    check_parse_result(text, RESP_PARSE_ERROR);
}

static void test_parse_inline_line_limit(void)
{
    char *text = mem_alloc(RESP_MAX_INLINE_BYTES + 1);
    memset(text, 'a', RESP_MAX_INLINE_BYTES);
    struct resp_command cmd;
    CHECK(resp_parse(text, RESP_MAX_INLINE_BYTES - 1, &cmd) == RESP_PARSE_NEED_MORE);
    CHECK(resp_parse(text, RESP_MAX_INLINE_BYTES, &cmd) == RESP_PARSE_ERROR);
    resp_command_deinit(&cmd);
    mem_free(text);
}

static void test_deinit_after_each_result_keeps_used_bytes(void)
{
    size_t baseline_bytes = mem_used_bytes();
    struct resp_command cmd;
    parse_text("*2\r\n$3\r\nGET\r\n$1\r\nk\r\n", &cmd);
    resp_command_deinit(&cmd);
    parse_text("*2\r\n$3\r\nGET\r\n$1\r\nkX", &cmd);
    resp_command_deinit(&cmd);
    parse_text("*2\r\n$3\r\nGET", &cmd);
    resp_command_deinit(&cmd);
    resp_command_deinit(&cmd); /* 두 번 불러도 안전하다 */
    CHECK(mem_used_bytes() == baseline_bytes);
}

static void run_multibulk_tests(void)
{
    RUN(test_parse_set_returns_args);
    RUN(test_parse_set_with_ex_returns_args);
    RUN(test_parse_get_and_del_return_args);
    RUN(test_parse_skips_blank_lines_before_command);
    RUN(test_parse_only_blank_lines_needs_more);
    RUN(test_parse_value_with_nul_and_crlf_keeps_bytes);
    RUN(test_parse_empty_bulk_returns_zero_length_arg);
}

static void run_inline_tests(void)
{
    RUN(test_parse_inline_returns_args);
    RUN(test_parse_inline_splits_on_spaces_and_tabs);
    RUN(test_parse_inline_line_limit);
}

static void run_stream_tests(void)
{
    RUN(test_parse_byte_by_byte_done_only_at_last_byte);
    RUN(test_parse_pipelined_commands_in_order);
    RUN(test_parse_pipelined_inline_commands_in_order);
}

static void run_error_tests(void)
{
    RUN(test_parse_bulk_without_trailing_crlf_returns_error);
    RUN(test_parse_bad_numbers_return_error);
    RUN(test_parse_many_leading_zeros_returns_error);
    RUN(test_parse_array_element_not_bulk_returns_error);
    RUN(test_parse_partial_numbers_need_more);
    RUN(test_parse_arg_count_limit);
    RUN(test_parse_bulk_size_limit);
    RUN(test_deinit_after_each_result_keeps_used_bytes);
}

int main(void)
{
    run_multibulk_tests();
    run_inline_tests();
    run_stream_tests();
    run_error_tests();
    TEST_DONE();
    return 0;
}
