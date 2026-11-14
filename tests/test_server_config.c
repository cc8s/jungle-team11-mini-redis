#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "server_config.h"
#include "test.h"

enum {
    MAX_FAKE_VARS = 8,
};

/* 테스트마다 채우는 가짜 환경. 표에 없는 이름은 NULL 이다 */
struct fake_var {
    const char *name;
    const char *value;
};

static struct fake_var g_fake_env[MAX_FAKE_VARS];
static size_t g_fake_count;

static void env_clear(void)
{
    g_fake_count = 0;
}

static void env_set(const char *name, const char *value)
{
    CHECK(g_fake_count < MAX_FAKE_VARS);
    g_fake_env[g_fake_count++] = (struct fake_var){.name = name, .value = value};
}

static const char *fake_lookup(const char *name)
{
    for (size_t i = 0; i < g_fake_count; i++) {
        if (strcmp(g_fake_env[i].name, name) == 0) {
            return g_fake_env[i].value;
        }
    }
    return NULL;
}

/* name=value 하나만 넣고 읽기가 실패하며, 메시지에 변수 이름이 들어가는지 본다 */
static bool rejects(const char *name, const char *value)
{
    env_clear();
    env_set(name, value);
    struct server_config cfg;
    struct server_config_error err;
    bool is_loaded = server_config_load_env(&cfg, fake_lookup, &err);
    return !is_loaded && strstr(err.message, name) != NULL;
}

static void test_no_env_gives_defaults(void)
{
    env_clear();
    struct server_config cfg;
    struct server_config_error err;
    CHECK(server_config_load_env(&cfg, fake_lookup, &err));
    CHECK_STR_EQ(cfg.bind_address, "127.0.0.1");
    CHECK(cfg.port == SERVER_DEFAULT_PORT);
    CHECK(cfg.max_clients == SERVER_MAX_CLIENTS);
    CHECK(cfg.max_memory_bytes == 0);
    CHECK(cfg.idle_timeout_ms == 0);
    CHECK(!cfg.is_flush_enabled);
}

static void test_all_env_values_are_applied(void)
{
    env_clear();
    env_set("MINI_REDIS_BIND", "0.0.0.0");
    env_set("MINI_REDIS_PORT", "7000");
    env_set("MINI_REDIS_MAXMEMORY", "1048576");
    env_set("MINI_REDIS_MAX_CLIENTS", "50");
    env_set("MINI_REDIS_IDLE_TIMEOUT_SEC", "30");
    env_set("MINI_REDIS_ENABLE_FLUSH", "1");
    struct server_config cfg;
    struct server_config_error err;
    CHECK(server_config_load_env(&cfg, fake_lookup, &err));
    CHECK_STR_EQ(cfg.bind_address, "0.0.0.0");
    CHECK(cfg.port == 7000);
    CHECK(cfg.max_memory_bytes == 1048576);
    CHECK(cfg.max_clients == 50);
    CHECK(cfg.idle_timeout_ms == 30000);
    CHECK(cfg.is_flush_enabled);
}

static void test_flush_zero_keeps_disabled(void)
{
    env_clear();
    env_set("MINI_REDIS_ENABLE_FLUSH", "0");
    struct server_config cfg;
    struct server_config_error err;
    CHECK(server_config_load_env(&cfg, fake_lookup, &err));
    CHECK(!cfg.is_flush_enabled);
}

static void test_port_boundaries(void)
{
    CHECK(rejects("MINI_REDIS_PORT", "0"));
    CHECK(rejects("MINI_REDIS_PORT", "65536"));
    CHECK(rejects("MINI_REDIS_PORT", "70000"));
    env_clear();
    env_set("MINI_REDIS_PORT", "65535");
    struct server_config cfg;
    struct server_config_error err;
    CHECK(server_config_load_env(&cfg, fake_lookup, &err));
    CHECK(cfg.port == 65535);
}

/* strtol 이 받아 주는 형태(부호, 공백, 뒤 문자, 빈 값)도 모두 거절한다 */
static void test_malformed_numbers_are_rejected(void)
{
    CHECK(rejects("MINI_REDIS_PORT", "abc"));
    CHECK(rejects("MINI_REDIS_PORT", ""));
    CHECK(rejects("MINI_REDIS_PORT", "+6379"));
    CHECK(rejects("MINI_REDIS_PORT", " 6379"));
    CHECK(rejects("MINI_REDIS_PORT", "6379abc"));
    CHECK(rejects("MINI_REDIS_PORT", "0x10"));
    CHECK(rejects("MINI_REDIS_MAXMEMORY", "-1"));
    CHECK(rejects("MINI_REDIS_MAXMEMORY", "100mb"));
    CHECK(rejects("MINI_REDIS_MAXMEMORY", "99999999999999999999999"));
    CHECK(rejects("MINI_REDIS_MAXMEMORY", "18446744073709551616"));
}

static void test_other_ranges_are_checked(void)
{
    CHECK(rejects("MINI_REDIS_MAX_CLIENTS", "0"));
    CHECK(rejects("MINI_REDIS_MAX_CLIENTS", "1000001"));
    CHECK(rejects("MINI_REDIS_IDLE_TIMEOUT_SEC", "-5"));
    CHECK(rejects("MINI_REDIS_IDLE_TIMEOUT_SEC", "31536001"));
}

static void test_bad_flag_and_address_are_rejected(void)
{
    CHECK(rejects("MINI_REDIS_ENABLE_FLUSH", "yes"));
    CHECK(rejects("MINI_REDIS_ENABLE_FLUSH", "true"));
    CHECK(rejects("MINI_REDIS_ENABLE_FLUSH", ""));
    CHECK(rejects("MINI_REDIS_BIND", "localhost"));
    CHECK(rejects("MINI_REDIS_BIND", "256.0.0.1"));
}

static void test_error_message_shows_value(void)
{
    env_clear();
    env_set("MINI_REDIS_PORT", "abc");
    struct server_config cfg;
    struct server_config_error err;
    CHECK(!server_config_load_env(&cfg, fake_lookup, &err));
    CHECK(strstr(err.message, "'abc'") != NULL);
    CHECK(strstr(err.message, "65535") != NULL);
}

static void test_long_bad_value_message_is_truncated(void)
{
    char long_value[SERVER_CONFIG_ERROR_BYTES * 2];
    memset(long_value, 'x', sizeof(long_value) - 1);
    long_value[sizeof(long_value) - 1] = '\0';
    env_clear();
    env_set("MINI_REDIS_BIND", long_value);
    struct server_config cfg;
    struct server_config_error err;
    CHECK(!server_config_load_env(&cfg, fake_lookup, &err));
    CHECK(strlen(err.message) == SERVER_CONFIG_ERROR_BYTES - 1);
}

int main(void)
{
    RUN(test_no_env_gives_defaults);
    RUN(test_all_env_values_are_applied);
    RUN(test_flush_zero_keeps_disabled);
    RUN(test_port_boundaries);
    RUN(test_malformed_numbers_are_rejected);
    RUN(test_other_ranges_are_checked);
    RUN(test_bad_flag_and_address_are_rejected);
    RUN(test_error_message_shows_value);
    RUN(test_long_bad_value_message_is_truncated);
    TEST_DONE();
    return 0;
}
