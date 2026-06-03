# JobSystem — C++ 멀티스레딩 학습 프로젝트

> C++17 표준 라이브러리만으로 Thread Pool을 직접 구현하고,  
> 각 설계 결정의 이유를 실험으로 확인하는 학습 프로젝트입니다.

---

## 목표

UE5 TaskGraph, Unity Job System 같은 엔진 내부 시스템이  
어떻게 동작하는지 직접 구현해서 이해합니다.  
외부 라이브러리 없이 `std::thread`, `std::mutex`, `std::atomic`, `std::condition_variable`만 사용합니다.

---

## Week 1 — 

### Day 01 — Spurious Wakeup

`condition_variable::wait(lock)`만 쓰면 OS가 이유 없이 스레드를 깨울 수 있다.  
이를 **spurious wakeup**이라 하며, POSIX 표준에서 허용된 동작이다.

### Day 02 — notify_one vs notify_all

`notify_all`로 스레드 N개를 깨우면 1개만 작업을 가져가고  
나머지 N-1개는 다시 잠든다. 이 과정이 **Thundering Herd** 문제다.

### Day 03 — 청크 수와 Speedup

| 청크 수 | 현상 |
|--------|------|
| 청크 < 스레드 수 | 일부 스레드가 놀음 → Speedup 낮음 |
| 청크 = 스레드 수 | 작업량 불균일 시 Tail Latency 발생 |
| 청크 = 스레드 × 4~8 | 동적 로드 밸런싱, 실전 스윗 스팟 |
| 청크 >> 스레드 수 | Submit/Queue 오버헤드 역전 |

### Day 04 — SubmitWithFuture

반환값 있는 작업을 제출하고 `std::future<T>`로 결과를 받는다.

**예외 전파:** 작업 내 예외가 future에 저장되어 `get()` 호출 시 재발생한다.

### Day 05 — 스레드별 작업 분포

각 워커 스레드가 처리한 작업 수를 추적해 로드 밸런싱 상태를 시각화한다.

**주요 구현 포인트:**  
`std::atomic`은 복사/이동 불가이므로 `vector<atomic>::resize()` 사용 불가.  
→ `unique_ptr<atomic<uint64_t>[]>` + `make_unique<T[]>(n)` 패턴으로 해결.

### Day 06 — 데이터 레이스

`counter++`는 LOAD → ADD → STORE 3단계로, 원자적이지 않다.

데이터 레이스는 **비결정적**이라 가끔 맞아서 버그를 숨긴다.  
`-fsanitize=thread` (ThreadSanitizer)로 탐지 가능.

| | volatile int | std::atomic |
|--|--|--|
| 최적화 방지 | ✅ | ✅ |
| 멀티스레딩 동기화 | ❌ | ✅ |
| 성능 (상대적) | 빠름 (하지만 UB) | 느림 (캐시 동기화 비용) |

---

## Week 2 —

### Day 08 — JobHandle

`JobHandle`은 특정 작업 하나의 완료 상태를 추적한다.

`WaitAll()`은 풀에 제출된 모든 작업을 기다리지만, `JobHandle::Wait()`은 연결된 작업 하나만 기다린다.  
핸들은 복사 가능해야 하므로 완료 상태는 `shared_ptr<JobState>`로 공유한다.

### Day 09 — Submit → JobHandle 반환

`Submit()`이 `JobHandle`을 반환하도록 바꿔 기존 Submit 패턴과 선택적 대기를 하나의 API로 합쳤다.

핸들을 이용하면 A 완료 후 B 제출 같은 단순 의존성은 표현할 수 있다.  
하지만 메인 스레드가 `hA.Wait()`로 막히는 구조라, 자동 의존성 실행으로 가기 전 한계가 있다.

### Day 10 — Worker Wait Starvation

워커 스레드 안에서 다른 작업의 `JobHandle::Wait()`를 호출하면 워커 슬롯을 점유한다.

모든 워커가 대기 상태가 되면 큐에 자식 작업이 남아 있어도 실행할 워커가 없어져 starvation/deadlock이 발생할 수 있다.  
따라서 엔진식 JobSystem은 워커를 막는 Wait 대신 dependency counter, continuation, work stealing/helping wait 같은 구조가 필요하다.
