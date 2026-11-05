# Mini Redis

> **Krafton Jungle WEEK11-12 · Team 11** — [mini-react](https://github.com/cc8s/jungle-team11-mini-react) · [mini-redis](https://github.com/cc8s/jungle-team11-mini-redis) · [mini-dbms](https://github.com/cc8s/jungle-team11-mini-dbms) · [보드](https://github.com/users/cc8s/projects/9)

해시 테이블로 직접 만든 **RESP 호환 인메모리 키-값 저장소** (C). 실제 `redis-cli`, `redis-py` 로 접속할 수 있다.

## 실행 방법

> epoll 을 쓰므로 **Linux 전용**이다. Windows 에서는 VS Code → `Dev Containers: Reopen in Container` 로 연 뒤 빌드한다.

```bash
make            # 빌드
make test       # 단위 테스트
make run        # 서버 실행
redis-cli -p 6379 PING
```

## 아키텍처

<!-- 클라이언트 → RESP 파서 → 명령 처리 → 해시 테이블 / 만료 / AOF, epoll 이벤트 루프 -->

## 핵심 로직

### 해시 테이블

<!-- 해시 함수, 체이닝, 점진적 rehash -->

### 만료 (TTL)

<!-- lazy 삭제 + active 샘플링 삭제 -->

### 동시성

<!-- 단일 스레드 이벤트 루프라 락이 필요 없다. 대가: 느린 명령 하나가 전체를 막는다 → KEYS 는 O(N) 이라 위험, 실제 Redis 는 SCAN 권장 -->

### 캐시 무효화

<!-- cache-aside + 원본 수정 시 DEL (demo/ 기능 테스트) -->

### 영속화 (Optional)

<!-- AOF -->

## 테스트

| 구분 | 내용 |
|---|---|
| 단위 | |
| 기능 | |
| 엣지 케이스 | |

## 성능 비교 (캐시 사용 / 미사용)

## 실제 Redis 와의 차이

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
