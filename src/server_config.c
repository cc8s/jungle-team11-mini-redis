#include "server_config.h"

#include <stdio.h>
#include <string.h>

#include <arpa/inet.h>

enum {
    MS_PER_SEC = 1000,
    MAX_PORT = 65535,
    MAX_NUMBER_DIGITS = 20,
    DECIMAL_BASE = 10,
};

static const char DEFAULT_BIND_ADDRESS[] = "127.0.0.1";

/* 환경변수 하나를 읽을 때 함께 넘기는 것들. 실패 메시지는 err 에 쓴다 */
struct env_reader {
    server_env_lookup_fn *lookup;
    struct server_config_error *err;
};

/* 숫자 환경변수의 이름과 허용 범위 */
struct number_spec {
    const char *name;
    uint64_t min_value;
    uint64_t max_value;
};

static bool read_number(const struct env_reader *reader, const struct number_spec *spec,
                        uint64_t *out_value);
static bool parse_unsigned(const char *text, uint64_t *out_value);
static bool read_flag(const struct env_reader *reader, const char *name, bool *out_value);
static bool read_address(const struct env_reader *reader, const char *name,
                         const char **out_address);
static bool read_numbers(const struct env_reader *reader, struct server_config *cfg);

void server_config_init_default(struct server_config *cfg)
{
    *cfg = (struct server_config){
        .bind_address = DEFAULT_BIND_ADDRESS,
        .port = SERVER_DEFAULT_PORT,
        .max_clients = SERVER_MAX_CLIENTS,
        .max_memory_bytes = 0,
        .idle_timeout_ms = 0,
        .is_flush_enabled = false,
    };
}

/* 잘못된 값을 기본값으로 조용히 바꾸면 잘못 띄운 걸 모르고 넘어가므로, 하나라도 틀리면 실패한다 */
bool server_config_load_env(struct server_config *cfg, server_env_lookup_fn *lookup,
                            struct server_config_error *out_err)
{
    server_config_init_default(cfg);
    out_err->message[0] = '\0';
    const struct env_reader reader = {.lookup = lookup, .err = out_err};
    return read_address(&reader, "MINI_REDIS_BIND", &cfg->bind_address) &&
           read_numbers(&reader, cfg) &&
           read_flag(&reader, "MINI_REDIS_ENABLE_FLUSH", &cfg->is_flush_enabled);
}

/* 각 값은 기본값으로 시작해 환경변수가 있을 때만 바뀐다 */
static bool read_numbers(const struct env_reader *reader, struct server_config *cfg)
{
    const struct number_spec port_spec = {"MINI_REDIS_PORT", 1, MAX_PORT};
    const struct number_spec memory_spec = {"MINI_REDIS_MAXMEMORY", 0, SIZE_MAX};
    const struct number_spec clients_spec = {"MINI_REDIS_MAX_CLIENTS", 1,
                                             SERVER_CONFIG_MAX_CLIENTS_LIMIT};
    const struct number_spec idle_spec = {"MINI_REDIS_IDLE_TIMEOUT_SEC", 0,
                                          SERVER_CONFIG_MAX_IDLE_TIMEOUT_SEC};
    uint64_t port = (uint64_t)cfg->port;
    uint64_t max_memory_bytes = cfg->max_memory_bytes;
    uint64_t max_clients = cfg->max_clients;
    uint64_t idle_timeout_sec = (uint64_t)(cfg->idle_timeout_ms / MS_PER_SEC);
    if (!read_number(reader, &port_spec, &port) ||
        !read_number(reader, &memory_spec, &max_memory_bytes) ||
        !read_number(reader, &clients_spec, &max_clients) ||
        !read_number(reader, &idle_spec, &idle_timeout_sec)) {
        return false;
    }
    cfg->port = (int)port;
    cfg->max_memory_bytes = (size_t)max_memory_bytes;
    cfg->max_clients = (size_t)max_clients;
    cfg->idle_timeout_ms = (int64_t)idle_timeout_sec * MS_PER_SEC;
    return true;
}

/* 변수가 없으면 out_value 를 그대로 두고 true. 있으면 범위 안의 10진수여야 한다 */
static bool read_number(const struct env_reader *reader, const struct number_spec *spec,
                        uint64_t *out_value)
{
    const char *text = reader->lookup(spec->name);
    if (text == NULL) {
        return true;
    }
    uint64_t value = 0;
    if (!parse_unsigned(text, &value) || value < spec->min_value || value > spec->max_value) {
        snprintf(reader->err->message, sizeof(reader->err->message),
                 "%s 값 '%s' 이(가) 올바르지 않습니다 (%llu ~ %llu 의 정수)", spec->name, text,
                 (unsigned long long)spec->min_value, (unsigned long long)spec->max_value);
        return false;
    }
    *out_value = value;
    return true;
}

/* strtoull 은 앞 공백 · 부호 · 뒤 쓰레기를 받아 주므로 쓰지 않고, 숫자만으로 된 문자열만 받는다 */
static bool parse_unsigned(const char *text, uint64_t *out_value)
{
    size_t len = strlen(text);
    if (len == 0 || len > MAX_NUMBER_DIGITS) {
        return false;
    }
    uint64_t value = 0;
    for (size_t i = 0; i < len; i++) {
        if (text[i] < '0' || text[i] > '9') {
            return false;
        }
        uint64_t digit = (uint64_t)(text[i] - '0');
        if (value > (UINT64_MAX - digit) / DECIMAL_BASE) {
            return false;
        }
        value = value * DECIMAL_BASE + digit;
    }
    *out_value = value;
    return true;
}

static bool read_flag(const struct env_reader *reader, const char *name, bool *out_value)
{
    const char *text = reader->lookup(name);
    if (text == NULL) {
        return true;
    }
    if (strcmp(text, "1") == 0 || strcmp(text, "0") == 0) {
        *out_value = text[0] == '1';
        return true;
    }
    snprintf(reader->err->message, sizeof(reader->err->message),
             "%s 값 '%s' 이(가) 올바르지 않습니다 (1 또는 0)", name, text);
    return false;
}

/* 값은 복사하지 않고 환경변수 문자열을 그대로 가리킨다. 환경은 프로세스가 끝날 때까지 남는다 */
static bool read_address(const struct env_reader *reader, const char *name,
                         const char **out_address)
{
    const char *text = reader->lookup(name);
    if (text == NULL) {
        return true;
    }
    struct in_addr parsed;
    if (inet_pton(AF_INET, text, &parsed) == 1) {
        *out_address = text;
        return true;
    }
    snprintf(reader->err->message, sizeof(reader->err->message),
             "%s 값 '%s' 이(가) 올바르지 않습니다 (IPv4 주소)", name, text);
    return false;
}
