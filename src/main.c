#include <stdio.h>

#include "event_loop.h"
#include "hash_table.h"
#include "server.h"

/* 13단계에서 환경변수 설정을 읽도록 바뀐다. 지금은 기본값으로 서버를 띄운다 */
int main(void)
{
    if (hash_table_load_random_seed() != HT_OK) {
        fprintf(stderr, "mini-redis: /dev/urandom 에서 해시 seed 를 읽지 못했습니다\n");
        return 1;
    }
    struct server_config cfg;
    server_config_init_default(&cfg);
    struct server *server = server_create(&cfg);
    if (server == NULL) {
        perror("mini-redis: 서버를 시작하지 못했습니다");
        return 1;
    }
    if (event_loop_stop_on_signals() != EL_OK) {
        perror("mini-redis: 시그널 처리를 설정하지 못했습니다");
        server_destroy(server);
        return 1;
    }
    printf("mini-redis: %s:%d 에서 대기 중\n", cfg.bind_address, server_port(server));
    fflush(stdout);
    enum server_result result = server_run(server);
    server_destroy(server);
    return result == SERVER_OK ? 0 : 1;
}
