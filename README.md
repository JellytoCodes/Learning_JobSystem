# JobSystem — C++ 멀티스레딩 학습 프로젝트

> C++17 표준 라이브러리만으로 Thread Pool을 직접 구현하고,
> 각 설계 결정의 이유를 실험으로 확인하는 학습 프로젝트입니다.

---

## 목표

UE5 TaskGraph, Unity Job System 같은 엔진 내부 시스템이 어떻게 동작하는지 직접 구현해서 이해합니다.
외부 라이브러리 없이 `std::thread`, `std::mutex`, `std::atomic`, `std::condition_variable`을 중심으로 실험합니다.

---

## 현재 구현 상태

| 기능 | 구현 |
|------|------|
| 기본 스케줄링 | `ThreadPool::Submit()` |
| 전체 완료 대기 | `ThreadPool::WaitAll()` |
| 반환값 있는 작업 | `ThreadPool::SubmitWithFuture()` |
| 특정 작업 추적 | `JobHandle::Wait()`, `JobHandle::IsDone()` |
| 대기 중 작업 돕기 | `ThreadPool::WaitWithHelping(handle)` |
| 의존성 기반 후속 작업 | `SubmitAfter(dependencies, job)` |
| 성공 기반 후속 작업 | `SubmitAfterAllSucceeded(dependencies, job)` |
| 작업 예외 전파 | `std::exception_ptr` 저장 후 `JobHandle::Wait()`에서 재전파 |
| 실패 정책 | 완료 기반 실행 vs 성공 기반 취소 |
| Work Stealing 실험 | worker local queue + steal 구조 |

---

## 빌드와 실행

Visual Studio CMake preset 기준:

```powershell
cmake --preset x64-debug
cmake --build out\build\x64-debug
```

특정 실험만 실행:

```powershell
out\build\x64-debug\Day13_FailurePolicy.exe
```

각 실험 파일 상단에도 `g++` 기준 단일 빌드 명령을 남겨두었습니다.

---

## Week 1 — ThreadPool 기초

| Day | 주제 | 핵심 |
|-----|------|------|
| Day 01 | Spurious Wakeup | `condition_variable::wait()`는 반드시 predicate와 함께 써야 한다. |
| Day 02 | `notify_one` vs `notify_all` | 불필요하게 많은 스레드를 깨우면 thundering herd가 생긴다. |
| Day 03 | 청크 수와 Speedup | 청크 수는 스레드 수보다 충분히 많아야 tail latency를 줄일 수 있다. |
| Day 04 | `SubmitWithFuture` | 반환값과 예외를 `std::future<T>`로 받을 수 있다. |
| Day 05 | 스레드별 작업 분포 | per-thread counter로 로드 밸런싱 상태를 관찰한다. |
| Day 06 | 데이터 레이스 | `counter++`는 원자적이지 않으며, 데이터 레이스는 UB다. |

### Week 1 리마인드

ThreadPool의 기본은 큐, mutex, condition_variable입니다.
하지만 성능과 안정성은 세부 정책에서 갈립니다.
스레드를 몇 개 만들지, 작업을 얼마나 잘게 나눌지, 어떤 스레드를 깨울지, 결과를 어디로 받을지 같은 선택이 실제 JobSystem의 동작을 결정합니다.

---

## Week 2 — JobHandle과 작업 그래프

| Day | 주제 | 핵심 |
|-----|------|------|
| Day 08 | `JobHandle` | 전체 풀이 아니라 특정 작업 하나의 완료를 추적한다. |
| Day 09 | `Submit` → `JobHandle` | 기존 Submit 패턴과 선택적 대기를 하나의 API로 합쳤다. |
| Day 10 | Worker Wait Starvation | 워커 안에서 다른 작업을 `Wait()`하면 워커 슬롯을 점유해 starvation이 생길 수 있다. |
| Day 11 | Dependency Counter / Continuation | `Wait()` 대신 선행 작업 완료 시 후속 작업을 큐에 자동 제출한다. |
| Day 12 | Exception Propagation | 작업 예외를 `JobState`에 저장하고 `Wait()`에서 재전파한다. |
| Day 13 | Failure Policy | 완료 기반 continuation과 성공 기반 continuation을 분리했다. |
| Day 14 | Week 2 Recap | 작업 그래프 관점에서 API와 정책을 정리한다. |

### Day 08 — JobHandle

`JobHandle`은 특정 작업 하나의 완료 상태를 추적합니다.
`WaitAll()`은 풀에 제출된 모든 작업을 기다리지만, `JobHandle::Wait()`은 연결된 작업 하나만 기다립니다.
핸들은 복사 가능해야 하므로 완료 상태는 `shared_ptr<JobState>`로 공유합니다.

### Day 09 — Submit → JobHandle 반환

`Submit()`이 `JobHandle`을 반환하도록 바꿔 기존 Submit 패턴과 선택적 대기를 하나의 API로 합쳤습니다.
핸들을 이용하면 A 완료 후 B 제출 같은 단순 의존성은 표현할 수 있습니다.
하지만 메인 스레드가 `hA.Wait()`로 막히는 구조라, 자동 의존성 실행으로 가기 전 한계가 있습니다.

### Day 10 — Worker Wait Starvation

워커 스레드 안에서 다른 작업의 `JobHandle::Wait()`를 호출하면 워커 슬롯을 점유합니다.
모든 워커가 대기 상태가 되면 큐에 자식 작업이 남아 있어도 실행할 워커가 없어져 starvation/deadlock이 발생할 수 있습니다.
따라서 엔진식 JobSystem은 워커를 막는 Wait 대신 dependency counter, continuation, work stealing/helping wait 같은 구조가 필요합니다.

### Day 11 — Dependency Counter / Continuation

`SubmitAfter(dependencies, job)`을 추가해 모든 선행 작업이 완료된 뒤 후속 작업이 자동 제출되도록 만들었습니다.
각 선행 작업 완료 시 continuation이 atomic counter를 감소시키고, 마지막 선행 작업이 끝나는 순간 후속 작업을 큐에 넣습니다.
이 방식은 워커 스레드가 `Wait()`로 막히지 않으므로 작업 그래프를 큐 기반으로 흘려보내는 구조에 가까워집니다.

### Day 12 — Exception Propagation / Worker Survival

`Submit()` 작업 내부에서 예외가 발생해도 워커 스레드가 종료되지 않도록 작업 래퍼에서 예외를 잡고 `JobState`에 저장합니다.
`JobHandle::Wait()`는 완료를 기다린 뒤 저장된 `std::exception_ptr`을 다시 던집니다.
따라서 호출자는 실패를 명시적으로 처리할 수 있고, 워커는 다음 작업을 계속 처리합니다.

### Day 13 — Failure Policy: Completion vs Success

`SubmitAfterAllSucceeded(dependencies, job)`을 추가해 모든 선행 작업이 성공했을 때만 후속 작업을 실행하는 정책을 만들었습니다.
기존 `SubmitAfter()`는 완료 기반 continuation이라 선행 작업이 실패해도 후속 작업을 실행합니다.
반면 `SubmitAfterAllSucceeded()`는 선행 작업 중 하나라도 예외를 저장했으면 후속 작업을 실행하지 않고 `JobCanceledException`으로 완료시킵니다.

### Day 14 — Week 2 Recap

Week 2의 핵심은 `Wait()`를 줄이고 작업 그래프를 큐로 흘려보내는 방향입니다.

| 개념 | 역할 |
|------|------|
| `JobHandle` | 작업 완료 상태를 외부에서 추적하는 핸들 |
| `JobState` | 완료 여부, 예외, continuation을 공유하는 내부 상태 |
| `Wait()` | 호출 스레드를 블록하는 동기화 지점 |
| continuation | 선행 작업 완료 시 실행할 후속 제출 로직 |
| dependency counter | 남은 선행 작업 수를 추적하는 atomic 카운터 |
| failure policy | 선행 작업 실패 후 후속 작업을 실행할지 취소할지 결정하는 정책 |

중요한 구분:

| 질문 | 답 |
|------|----|
| 작업이 끝났는가? | completion |
| 작업이 성공했는가? | exception/failure state |
| 후속 작업을 실행할 것인가? | scheduling policy |
| 호출 스레드를 막을 것인가? | wait policy |

이 네 가지를 섞으면 구현은 단순해 보이지만, 워커 starvation이나 예외 누락 같은 문제가 생깁니다.
따라서 `JobHandle`, `JobState`, dependency counter, failure policy를 분리해서 생각하는 것이 Week 2의 핵심입니다.

---

## Week 3 — Wait 정책과 큐 구조

| Day | 주제 | 핵심 |
|-----|------|------|
| Day 15 | Helping Wait | 기다리는 동안 큐의 다른 작업을 직접 실행해 starvation 위험을 줄인다. |
| Day 16 | Work Stealing | 워커별 local queue를 두고, 빈 워커가 다른 워커의 작업을 훔쳐 부하 불균형을 줄인다. |

### Day 15 — Helping Wait

`ThreadPool::WaitWithHelping(handle)`을 추가했습니다.
일반 `JobHandle::Wait()`는 호출 스레드를 잠재우지만, `WaitWithHelping()`은 기다리는 동안 큐에서 작업을 꺼내 현재 스레드가 직접 실행합니다.

Day 10의 문제는 워커가 `Wait()`로 막히면 큐에 남은 자식 작업을 실행할 스레드가 부족해진다는 점이었습니다.
Helping Wait은 기다리는 워커도 진행에 기여하게 만들어 이 starvation 위험을 줄입니다.

다만 현재 구현은 전역 큐에서만 작업을 꺼냅니다.
실제 엔진에 가까워지려면 worker local queue, work stealing, 재진입 깊이 제한 같은 정책이 추가로 필요합니다.

### Day 16 — Work Stealing

`experiments/Day16_WorkStealing.cpp`에서 독립적인 `WorkStealingPool`을 구현했습니다.
이번 실험은 기존 `ThreadPool` API를 바로 바꾸기보다, 큐 구조 자체를 분리해서 관찰하는 데 초점을 둡니다.

핵심 구조:

| 구성 | 역할 |
|------|------|
| worker local queue | 각 워커가 자기 작업을 우선 처리한다. |
| owner pop back | 소유 워커는 뒤쪽에서 꺼내 최근에 들어온 작업을 빠르게 처리한다. |
| thief steal front | 빈 워커는 다른 워커 큐의 앞쪽에서 오래된 작업을 훔친다. |
| pending job counter | 모든 local queue에 흩어진 작업의 전체 완료를 추적한다. |

실험은 세 가지를 확인합니다.

| 실험 | 확인 내용 |
|------|----------|
| 균등 분배 | 각 워커에 작업이 고르게 있으면 steal 필요가 작다. |
| 한 워커 과부하 | worker 0에 몰린 작업을 다른 워커들이 훔쳐 처리한다. |
| LIFO/FIFO 방향 | 소유자는 뒤에서 pop하고, 훔치는 쪽은 앞에서 가져간다. |

전역 큐 하나만 두면 모든 워커가 같은 mutex를 경쟁합니다.
Work Stealing은 평소에는 자기 local queue만 건드리고, 놀고 있는 워커만 다른 큐를 확인하므로 전역 큐 병목과 부하 불균형을 동시에 줄이는 방향입니다.

---

## 다음 방향

Week 3에서 다루기 좋은 후보:

| 후보 | 이유 |
|------|------|
| Job Graph Builder | 여러 job과 dependency를 한 번에 선언하는 API를 만든다. |
| JobState Pool | `shared_ptr<JobState>` 할당 비용을 줄이는 재사용 구조를 실험한다. |
| ThreadPool local queue 통합 | Day16 실험 구조를 기존 `ThreadPool`에 점진적으로 반영한다. |

Day 17 후보로는 **Job Graph Builder**가 자연스럽습니다.
Day11~13에서 dependency와 failure policy를 만들었고, Day15~16에서 wait/queue 정책을 확장했으므로, 이제 여러 작업과 의존성을 한 번에 선언하는 사용성 계층을 얹기 좋습니다.
