#include "server.h"

#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "buf.h"
#include "commands.h"
#include "db.h"
#include "expire.h"
#include "resp.h"

typedef struct {
    int fd;
    buf in, out;
    int closing;   /* 응답을 다 보낸 뒤 닫는다 */
    int want_out;  /* EPOLLOUT 을 켜 두었나 */
} client;

static volatile sig_atomic_t stop_flag = 0;
static void on_signal(int sig)
{
    (void)sig;
    stop_flag = 1;
}

static int64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int epfd;
static client *clients[SERVER_MAX_CLIENTS];
static int nclients;

static void client_close(client *c)
{
    epoll_ctl(epfd, EPOLL_CTL_DEL, c->fd, NULL);
    close(c->fd);
    clients[c->fd] = NULL;
    buf_free(&c->in);
    buf_free(&c->out);
    free(c);
    nclients--;
}

static void set_events(client *c, int want_out)
{
    if (c->want_out == want_out)
        return;
    struct epoll_event ev = {.events = EPOLLIN | (want_out ? EPOLLOUT : 0), .data.fd = c->fd};
    epoll_ctl(epfd, EPOLL_CTL_MOD, c->fd, &ev);
    c->want_out = want_out;
}

/* out 을 보낼 수 있는 만큼 보낸다. 연결을 닫았으면 -1 */
static int client_flush(client *c)
{
    while (c->out.len > 0) {
        ssize_t n = send(c->fd, c->out.data, c->out.len, MSG_NOSIGNAL);
        if (n > 0) {
            buf_consume(&c->out, (size_t)n);
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            set_events(c, 1);
            return 0;
        } else {
            client_close(c);
            return -1;
        }
    }
    set_events(c, 0);
    if (c->closing) {
        client_close(c);
        return -1;
    }
    return 0;
}

/* 입력 버퍼에서 완성된 명령을 모두 실행한다 */
static void client_process(client *c, db *d)
{
    size_t off = 0;
    while (!c->closing && off < c->in.len) {
        resp_cmd cmd;
        size_t used;
        const char *err;
        resp_status st = resp_parse(c->in.data + off, c->in.len - off, &cmd, &used, &err);
        if (st == RESP_INCOMPLETE)
            break;
        if (st == RESP_ERR) {
            resp_error(&c->out, err);
            c->closing = 1;
            break;
        }
        cmd_execute(d, &cmd, &c->out);
        resp_cmd_free(&cmd);
        off += used;
    }
    buf_consume(&c->in, off);
}

static void client_readable(client *c, db *d)
{
    char tmp[16 * 1024];
    for (;;) {
        ssize_t n = read(c->fd, tmp, sizeof tmp);
        if (n > 0) {
            if (buf_append(&c->in, tmp, (size_t)n) != 0 || c->in.len > (size_t)SERVER_MAX_QUERYBUF) {
                client_close(c);
                return;
            }
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            break;
        client_close(c); /* 0 = 상대가 닫음, 그 외 오류 */
        return;
    }
    client_process(c, d);
    client_flush(c);
}

static void accept_all(int lfd)
{
    for (;;) {
        int fd = accept4(lfd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EINTR)
                continue;
            return; /* EAGAIN 또는 일시적 오류 */
        }
        if (fd >= SERVER_MAX_CLIENTS) {
            static const char msg[] = "-ERR max number of clients reached\r\n";
            send(fd, msg, sizeof msg - 1, MSG_NOSIGNAL);
            close(fd);
            continue;
        }
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        client *c = calloc(1, sizeof *c);
        if (c == NULL) {
            close(fd);
            continue;
        }
        c->fd = fd;
        buf_init(&c->in);
        buf_init(&c->out);
        struct epoll_event ev = {.events = EPOLLIN, .data.fd = fd};
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) != 0) {
            close(fd);
            free(c);
            continue;
        }
        clients[fd] = c;
        nclients++;
    }
}

static void cron(db *d)
{
    expire_active_cycle(d);
    int64_t until = mono_ms() + 1;
    while ((dict_is_rehashing(d->main) || dict_is_rehashing(d->expires)) && mono_ms() < until) {
        dict_rehash_step(d->main, 100);
        dict_rehash_step(d->expires, 100);
    }
}

static int listen_on(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons((unsigned short)port),
                               .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) != 0 || listen(fd, 511) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int server_run(const server_config *cfg)
{
    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    db *d = db_create(cfg->maxmemory, cfg->policy);
    int lfd = listen_on(cfg->port);
    epfd = epoll_create1(EPOLL_CLOEXEC);
    if (d == NULL || lfd < 0 || epfd < 0) {
        perror("mini-redis: 시작 실패");
        return 1;
    }
    struct epoll_event ev = {.events = EPOLLIN, .data.fd = lfd};
    epoll_ctl(epfd, EPOLL_CTL_ADD, lfd, &ev);
    printf("mini-redis: %d 번 포트에서 대기 중\n", cfg->port);
    fflush(stdout);

    struct epoll_event events[256];
    int64_t next_cron = mono_ms() + SERVER_CRON_MS;
    while (!stop_flag) {
        int64_t wait = next_cron - mono_ms();
        int n = epoll_wait(epfd, events, 256, wait > 0 ? (int)wait : 0);
        if (n < 0 && errno != EINTR)
            break;
        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;
            if (fd == lfd) {
                accept_all(lfd);
                continue;
            }
            client *c = clients[fd];
            if (c == NULL)
                continue;
            if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                client_close(c);
                continue;
            }
            if (events[i].events & EPOLLOUT && client_flush(c) != 0)
                continue;
            if (events[i].events & EPOLLIN)
                client_readable(c, d);
        }
        if (mono_ms() >= next_cron) {
            cron(d);
            next_cron = mono_ms() + SERVER_CRON_MS;
        }
    }

    for (int fd = 0; fd < SERVER_MAX_CLIENTS; fd++)
        if (clients[fd] != NULL)
            client_close(clients[fd]);
    close(lfd);
    close(epfd);
    db_free(d);
    printf("mini-redis: 종료\n");
    return 0;
}
