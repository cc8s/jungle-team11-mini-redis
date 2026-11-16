# Mini Redis

> **Krafton Jungle WEEK11-12 · Team 11** — [mini-react](https://github.com/cc8s/jungle-team11-mini-react) · [mini-redis](https://github.com/cc8s/jungle-team11-mini-redis) · [mini-dbms](https://github.com/cc8s/jungle-team11-mini-dbms) · [보드](https://github.com/users/cc8s/projects/9)

해시 테이블로 직접 만든 **RESP 호환 인메모리 키-값 저장소** (C). 실제 `redis-cli`, `redis-py` 로 접속할 수 있다.

- 명령: `SET key value [EX seconds]`, `GET key`, `DEL key [key ...]`, `FLUSHALL` (테스트용)
- 단일 스레드 이벤트 루프(epoll / kqueue), 점진적 rehash, TTL 만료(Lazy + 샘플링), maxmemory 근사 LRU 퇴출
- 외부 라이브러리 없이 C11 + POSIX 만 사용

## 실행 방법

Linux 개발 컨테이너(`.devcontainer`)에서 빌드 · 실행한다. Windows 에서는 VS Code → `Dev Containers: Reopen in Container` 로 연 뒤 빌드한다. 서버 자체는 macOS(kqueue)에서도 빌드된다.

```bash
make            # ./mini-redis 빌드
make run        # 127.0.0.1:6379 에서 실행 (Ctrl+C 로 종료)
```

```bash
redis-cli SET greeting hello EX 10
redis-cli GET greeting
```

```python
import redis

# redis-py 8 부터 기본이 RESP3 라서 protocol=2 를 넘겨야 한다 (5.x 는 기본값으로 동작)
r = redis.Redis(host="127.0.0.1", port=6379, protocol=2)
r.set("k", "v", ex=10)
r.get("k")
```

설정은 환경변수로 한다. 값이 잘못되면 이유를 출력하고 종료 코드 1 로 끝난다.

| 환경변수 | 범위 | 기본값 |
|---|---|---|
| `MINI_REDIS_BIND` | IPv4 주소 (컨테이너 밖에서 접속하려면 `0.0.0.0`) | `127.0.0.1` |
| `MINI_REDIS_PORT` | 1 ~ 65535 | 6379 |
| `MINI_REDIS_MAXMEMORY` | 바이트, 0 = 제한 없음 | 0 |
| `MINI_REDIS_MAX_CLIENTS` | 1 ~ 1000000 | 10000 |
| `MINI_REDIS_IDLE_TIMEOUT_SEC` | 초, 0 = 끔 | 0 |
| `MINI_REDIS_ENABLE_FLUSH` | `1` 이면 `FLUSHALL` 허용 (테스트용) | 0 |

```bash
MINI_REDIS_PORT=7000 MINI_REDIS_MAXMEMORY=104857600 MINI_REDIS_ENABLE_FLUSH=1 ./mini-redis
```

## 아키텍처

```
redis-cli · redis-py ──TCP(RESP2)──▶ server ─┬─ event_loop (epoll / kqueue, 단일 스레드)
                                             │    ├─ 리스닝 소켓 → accept → client_conn
                                             │    └─ 100ms 타이머 → server_cron
                                             └─ client_conn (연결마다 read_buf / write_buf)
                                                  └─ resp_parse → command_execute → resp_write_*
                                                                       │
                                                                       ▼
                                                    db ─┬─ keys    : hash_table (키 → 값 + 접근 시각)
                                                        ├─ expires : hash_table (키 → 만료 시각)
                                                        └─ pending : FLUSHALL 로 넘긴 옛 테이블
```

| 계층 | 파일 (`src/`) | 하는 일 |
|---|---|---|
| 기반 | `mem` · `str` · `clock` · `buffer` | 사용량을 세는 할당, 바이너리 안전 문자열, 단조 시계, 늘어나는 바이트 버퍼 |
| 저장 | `hash_table` · `db` · `expire` | 체이닝 + 점진적 rehash, 키 · 만료 두 테이블, 만료 샘플링, maxmemory 퇴출 |
| 프로토콜 | `resp` · `command` | RESP2 파서(배열 · 인라인), 응답 직렬화, 명령 처리 |
| 네트워크 | `event_loop` · `client_conn` · `server` · `server_config` · `main` | 이벤트 루프, 연결별 버퍼 · 파이프라이닝, accept · 주기 작업, 환경변수 설정 |

저장 · 프로토콜 계층과 연결 처리 로직은 소켓 없이 테스트할 수 있게 나눴다. 시간은 `now_ms` 인자로 받아 테스트에서 가짜 시각을 쓴다.

## 핵심 로직

### 해시 테이블

- 체이닝. 키 해시는 SipHash-2-4 이고 시작할 때 `/dev/urandom` 에서 seed 를 읽는다 (해시 충돌 공격 방지).
- 엔트리 수가 버킷 수를 넘으면 2배로 키우고, 10% 미만이면 절반으로 줄인다.
- 리사이즈는 **점진적 rehash**: 새 버킷 배열만 만들고, 옮기기는 set · delete 마다 버킷 1개, 주기 작업마다 1ms 씩 나눠서 한다. rehash 중 새 키는 새 테이블에만 넣고, 조회는 두 테이블을 모두 본다.
- 한 번에 옮기는 방식과 비교하면 키 100만 개 리사이즈에서 멈춤이 0.75초 → 0.2ms 안팎으로 줄었다 (아래 성능 절).

### 만료 (TTL)

- 만료 시각은 `expires` 테이블에 따로 둔다. 불변식: `expires` 의 키는 모두 `keys` 에 있다.
- **Lazy**: `GET` 이 만료된 키를 만나면 그 자리에서 지우고 nil 을 돌려준다.
- **Active (샘플링)**: 100ms 마다 TTL 키 20개를 무작위로 뽑아 만료된 것을 지우고, 25% 이상이 만료였으면 다시 뽑는다. 1ms 예산을 넘으면 멈춘다.
- `SET` 은 기존 TTL 을 지운다 (Redis 와 같음). `EX` 는 양의 정수만 받는다.

### 동시성

- 스레드 하나와 epoll(Linux) / kqueue(macOS) 이벤트 루프. 락이 없다.
- 대신 루프를 오래 막는 일은 모두 나눈다:
  - 읽기는 이벤트당 16KB. 큰 요청을 보내는 연결 하나가 루프를 독차지하지 않는다.
  - 파이프라인 응답이 1MB 를 넘으면 그 연결의 입력을 잠시 멈추고, 다 보낸 뒤 이어서 처리한다.
  - 주기 작업(100ms): 만료 1ms → rehash 1ms → 지연 해제 1ms → 유휴 연결 정리.
- 소켓은 논블로킹, 부분 읽기 · 쓰기를 버퍼로 이어 붙인다. 끊긴 소켓에 써도 죽지 않게 SIGPIPE 를 무시하고 `EPIPE` 로 처리한다.

### 캐시 무효화

| 방식 | 명령 · 설정 | 동작 |
|---|---|---|
| 명시적 삭제 | `DEL key [key ...]` | 지운 키 수를 돌려준다 |
| TTL 만료 | `SET key value EX seconds` | 위의 Lazy + Active 만료 |
| 전체 삭제 | `FLUSHALL` (`MINI_REDIS_ENABLE_FLUSH=1` 일 때만) | 두 테이블을 새것으로 바꾸고 옛것은 주기 작업이 나눠 해제한다. 키 100만 개에서도 0.23ms 에 응답 |
| 메모리 한도 | `MINI_REDIS_MAXMEMORY` | 넘으면 근사 LRU(5개 샘플, 만료된 키 우선)로 퇴출, 값 하나가 한도보다 크면 `-OOM` |

캐시로 쓸 때는 cache-aside 를 가정한다: 읽을 때 `GET` 이 nil 이면 원본(DB)에서 읽어 `SET ... EX` 로 넣고, 원본을 고치면 그 키를 `DEL` 한다. TTL 은 `DEL` 을 놓쳤을 때 오래된 값이 남는 시간의 상한이 된다. (이 흐름을 보여 주는 `demo/` 는 아직 없다.)

### 영속화 (Optional)

구현하지 않았다. 재시작하면 데이터가 사라진다.

## 테스트

| 종류 | 명령 | 내용 | 결과 |
|---|---|---|---|
| 단위 | `make test` | 14개 실행 파일 · 테스트 함수 259개 (해시 테이블, db, 만료, RESP, 명령, 이벤트 루프, 연결, 서버, 설정 …) | Linux · macOS 통과 |
| 인수 | `make e2e` | 실제 서버를 띄워 `redis-cli` · 원시 소켓 · redis-py 로 31개 항목 확인 | 통과 |
| 메모리 | `make valgrind` | 단위 테스트 전체 + 서버를 valgrind 로 띄운 e2e | 오류 · 누수 0 |
| 안정성 | `make stability` | 비정상 클라이언트 351개(반만 보내기 · RST · 응답 안 읽기 …), `redis-benchmark -c 500`, 8MB maxmemory 에 키 20만 개 | 15개 항목 통과 |
| 성능 | `make bench` | 아래 성능 절 | — |

`make e2e` · `make stability` · `make bench` 는 Linux 개발 컨테이너용이다. redis-py 가 없으면 `build/e2e-venv` 에 설치한다.

## 성능 비교 (캐시 사용 / 미사용)

캐시 사용 / 미사용 비교(DB 앞단에 캐시를 둔 경우)는 아직 측정하지 않았다. 아래는 서버 자체 성능이다.

측정 환경: macOS 위 Docker Desktop (Ubuntu 22.04, CPU 2개). 서버와 측정 도구가 같은 VM 에서 CPU 를 나눠 쓰므로 절대값보다 **같은 조건의 Redis 6.0.16 대비 비율**로 본다. `make bench` 로 다시 잴 수 있다.

**처리량** — `redis-benchmark`, 요청 20만 개, 키 공간 10만, 연결 50개, 3회 중앙값

| 시나리오 | mini-redis (req/s) | Redis 6.0.16 (req/s) | 비율 |
|---|---|---|---|
| GET | 53,163 | 63,251 | 84% |
| GET, 파이프라인 16 | 344,262 | 453,805 | 76% |
| SET | 56,338 | 62,228 | 91% |
| SET, 파이프라인 16 | 317,414 | 420,639 | 75% |
| SET EX 1 + GET 동시 | 31,114 / 31,536 | 27,233 / 27,038 | 114% / 117% |

**점진적 rehash 효과** — 키 110만 개를 하나씩 SET 하며 요청별 왕복 지연을 쟀다. 리사이즈가 시작되는 요청(2^k 번째)의 지연:

| 서버 | 2^16 | 2^17 | 2^18 | 2^19 | 2^20 번째 |
|---|---|---|---|---|---|
| 점진적 rehash (기본) | 36 µs | 160 µs | 162 µs | 166 µs | 164 µs |
| 한 번에 rehash (비교용 빌드) | 19.7 ms | 43.6 ms | 118 ms | 196 ms | 746 ms |

한 번에 옮기면 멈추는 시간이 키 수에 비례해 늘어나 100만 개에서 모든 클라이언트가 0.75초 기다린다. 점진적 rehash 는 리사이즈 순간도 평소 요청(p50 142µs)과 같다.

## 실제 Redis 와의 차이

| 항목 | mini-redis | Redis |
|---|---|---|
| 명령 | `SET` (`EX` 만), `GET`, `DEL`, `FLUSHALL` | 수백 개 (`PING`, `CONFIG`, `INCR`, 자료구조 …) |
| 자료형 | 문자열만 | 문자열 · 리스트 · 해시 · 셋 · 정렬 셋 … |
| 프로토콜 | RESP2 만 (`HELLO` 없음) | RESP2 · RESP3 |
| `FLUSHALL` | 환경변수로 켜야 동작 | 항상 동작 |
| 퇴출 정책 | 근사 LRU 하나 | `allkeys-lru` · `volatile-ttl` · LFU 등 여러 개 |
| 영속화 · 복제 · 클러스터 | 없음 | RDB · AOF · replica · cluster |
| 스레드 | 단일 스레드 | 명령 실행은 단일, I/O 스레드 · 백그라운드 스레드 사용 가능 |

`redis-benchmark` 는 시작할 때 `CONFIG GET` 을 보내 에러를 찍지만 기본값으로 계속 동작한다.

## 팀

| 이름 | GitHub | 역할 |
|---|---|---|
| | [@cc8s](https://github.com/cc8s) | |
| | [@bloro-sh](https://github.com/bloro-sh) | |

## Contributing

- `main` 에 직접 push 하지 않는다. 브랜치 → PR → 리뷰 1명 승인 → merge
- 브랜치: `feat/<주제>`, `fix/<주제>`, `docs/<주제>`
- 커밋: `[Feat]` `[Fix]` `[Test]` `[Docs]` `[Refactor]` `[Chore]` + 제목, 빈 줄, 한 줄 설명
- 이슈 1개 ↔ PR 1개. PR 본문에 `Closes #이슈번호`
