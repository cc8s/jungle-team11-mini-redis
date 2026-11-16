/* 요청 하나씩의 왕복 지연을 재는 벤치마크 클라이언트.
 * SET bench:<i> v 를 응답을 받을 때마다 하나씩 보내고(파이프라인 없음) 지연 분포를 출력한다.
 * 사용법: latency <port> <key_count>
 * 출력 1줄: count, p50_us, p99_us, p999_us, max_us, over_1ms, over_10ms, max_at (key=value)
 * 출력 2줄: resize 2^k=µs ... — 키 테이블이 커지기 시작하는 요청(2^k 번째)의 지연.
 *          엔트리 수가 버킷 수를 넘는 순간 리사이즈하므로, 버킷 2^k 개에서는 2^k 번째 SET 이다 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

enum {
    REQUEST_BUF_BYTES = 128,
    OK_REPLY_BYTES = 5,
    US_PER_SEC = 1000000,
    NS_PER_US = 1000,
    US_PER_MS = 1000,
    SLOW_MS = 1,
    VERY_SLOW_MS = 10,
    PERMILLE_SCALE = 1000,
    P50_PERMILLE = 500,
    P99_PERMILLE = 990,
    P999_PERMILLE = 999,
    DECIMAL_BASE = 10,
    FIRST_RESIZE_SHIFT = 16,
    EXPECTED_ARGC = 3,
};

static const char OK_REPLY[] = "+OK\r\n";

/* 측정 결과. latencies_us 는 요청 순서대로 들어 있다 */
struct latency_run {
    uint32_t *latencies_us;
    size_t count;
};

static int connect_server(int port);
static bool send_set(int fd, size_t key_number);
static bool read_ok(int fd);
static int64_t now_us(void);
static int compare_u32(const void *a, const void *b);
static void print_summary(struct latency_run *run);
static void print_resize_points(const struct latency_run *run);
static uint32_t percentile_us(const uint32_t *sorted_us, size_t count, int permille);

int main(int argc, char **argv)
{
    if (argc != EXPECTED_ARGC) {
        fprintf(stderr, "사용법: %s <port> <key_count>\n", argv[0]);
        return 1;
    }
    int port = (int)strtol(argv[1], NULL, DECIMAL_BASE);
    size_t key_count = (size_t)strtoull(argv[2], NULL, DECIMAL_BASE);
    int fd = connect_server(port);
    if (fd < 0 || key_count == 0) {
        perror("latency: 접속 실패");
        return 1;
    }
    struct latency_run run = {.latencies_us = calloc(key_count, sizeof(uint32_t)),
                              .count = key_count};
    if (run.latencies_us == NULL) {
        close(fd);
        return 1;
    }
    for (size_t i = 0; i < key_count; i++) {
        int64_t start_us = now_us();
        if (!send_set(fd, i) || !read_ok(fd)) {
            fprintf(stderr, "latency: %zu 번째 요청 실패\n", i);
            free(run.latencies_us);
            close(fd);
            return 1;
        }
        run.latencies_us[i] = (uint32_t)(now_us() - start_us);
    }
    print_resize_points(&run);
    print_summary(&run);
    free(run.latencies_us);
    close(fd);
    return 0;
}

/* 작은 요청을 하나씩 주고받으므로 Nagle 지연이 끼지 않게 TCP_NODELAY 를 켠다 */
static int connect_server(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    int no_delay = 1;
    if (inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1 ||
        connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &no_delay, sizeof(no_delay)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static bool send_set(int fd, size_t key_number)
{
    char key[REQUEST_BUF_BYTES];
    int key_len = snprintf(key, sizeof(key), "bench:%zu", key_number);
    char request[REQUEST_BUF_BYTES];
    int request_len = snprintf(request, sizeof(request),
                               "*3\r\n$3\r\nSET\r\n$%d\r\n%s\r\n$1\r\nv\r\n", key_len, key);
    return send(fd, request, (size_t)request_len, 0) == request_len;
}

/* 응답은 항상 +OK\r\n 5바이트다. 나눠 올 수 있으므로 다 모일 때까지 읽는다 */
static bool read_ok(int fd)
{
    char reply[OK_REPLY_BYTES];
    size_t received_total = 0;
    while (received_total < OK_REPLY_BYTES) {
        ssize_t received = recv(fd, reply + received_total, OK_REPLY_BYTES - received_total, 0);
        if (received < 0 && errno == EINTR) {
            continue;
        }
        if (received <= 0) {
            return false;
        }
        received_total += (size_t)received;
    }
    return memcmp(reply, OK_REPLY, OK_REPLY_BYTES) == 0;
}

static int64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * US_PER_SEC + ts.tv_nsec / NS_PER_US;
}

static int compare_u32(const void *a, const void *b)
{
    uint32_t left = *(const uint32_t *)a;
    uint32_t right = *(const uint32_t *)b;
    return (left > right) - (left < right);
}

/* 가장 느렸던 요청이 몇 번째였는지(max_at)도 출력해 리사이즈 시점과 맞춰 볼 수 있게 한다.
 * 분위수를 구하려고 latencies_us 를 정렬하므로 순서가 바뀐다 */
static void print_summary(struct latency_run *run)
{
    size_t max_at = 0;
    size_t over_1ms = 0;
    size_t over_10ms = 0;
    for (size_t i = 0; i < run->count; i++) {
        uint32_t latency_us = run->latencies_us[i];
        if (latency_us > run->latencies_us[max_at]) {
            max_at = i;
        }
        over_1ms += latency_us >= SLOW_MS * US_PER_MS;
        over_10ms += latency_us >= VERY_SLOW_MS * US_PER_MS;
    }
    uint32_t max_us = run->latencies_us[max_at];
    qsort(run->latencies_us, run->count, sizeof(uint32_t), compare_u32);
    uint32_t p50_us = percentile_us(run->latencies_us, run->count, P50_PERMILLE);
    uint32_t p99_us = percentile_us(run->latencies_us, run->count, P99_PERMILLE);
    uint32_t p999_us = percentile_us(run->latencies_us, run->count, P999_PERMILLE);
    printf("count=%zu p50_us=%u p99_us=%u p999_us=%u max_us=%u over_1ms=%zu over_10ms=%zu "
           "max_at=%zu\n",
           run->count, p50_us, p99_us, p999_us, max_us, over_1ms, over_10ms, max_at);
}

/* 정렬하기 전에 불러야 한다. 2^FIRST_RESIZE_SHIFT 보다 작은 리사이즈는 옮길 키가 적어 뺀다 */
static void print_resize_points(const struct latency_run *run)
{
    printf("resize");
    for (size_t idx = (size_t)1 << FIRST_RESIZE_SHIFT; idx < run->count; idx *= 2) {
        printf(" %zu=%u", idx, run->latencies_us[idx]);
    }
    printf("\n");
}

static uint32_t percentile_us(const uint32_t *sorted_us, size_t count, int permille)
{
    size_t idx = count * (size_t)permille / PERMILLE_SCALE;
    if (idx >= count) {
        idx = count - 1;
    }
    return sorted_us[idx];
}
