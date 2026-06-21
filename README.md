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
| 큐 라우팅 | 외부 Submit은 global queue, worker 내부 Submit은 local queue |
| Work Stealing | idle worker가 다른 worker의 local queue에서 작업을 가져옴 |
| 큐 통계 | `GetQueueStats()`로 global/local pop, steal, 처리량 확인 |
| 관측성 실험 | graph report, Chrome Trace export, trace ring buffer |
| 메모리 재사용 실험 | `JobStatePool` free list + generation 관찰 |

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

## Week 3 — 스케줄링, 그래프, 관측성

| Day | 주제 | 핵심 |
|-----|------|------|
| Day 15 | Helping Wait | 기다리는 동안 큐의 다른 작업을 직접 실행해 starvation 위험을 줄인다. |
| Day 16 | Work Stealing | 워커별 local queue를 두고, 빈 워커가 다른 워커의 작업을 훔쳐 부하 불균형을 줄인다. |
| Day 17 | Job Graph Builder | 여러 작업과 의존성을 선언 단계와 실행 단계로 분리한다. |
| Day 18 | Graph Validation | invalid edge와 cycle을 실행 전에 검증하고 topological order를 만든다. |
| Day 19 | Graph Execution Report | 노드별 queue wait와 실행 시간을 측정하고 critical path를 찾는다. |
| Day 20 | Chrome Trace Export | 실행, 대기, dependency 흐름을 Trace Event JSON으로 내보낸다. |
| Day 21 | JobState Pool | 작업 상태 객체를 재사용해 allocation pressure를 줄이는 구조를 실험한다. |
| Day 22 | ThreadPool Local Queue | 워커 내부 제출을 local queue로 보내고 idle worker가 steal한다. |
| Day 23 | Trace Ring Buffer | 고정 크기 trace buffer로 메모리 사용량을 고정하고 최근 event window만 보존한다. |
| Day 24 | Week 3 Recap | wait, queue, graph, trace, allocation 흐름과 현재 한계를 정리한다. |
| Day 25 | API Cleanup | 공개 API는 유지하면서 오래된 설명, 중복 주석, 헤더 구현 위치를 정리한다. |
| Day 26 | Regression Suite | 핵심 스케줄링 계약을 한 실행 파일에서 반복 검증한다. |
| Day 27 | Failure / Cancel Trace | 실행 실패와 dependency 기반 취소를 서로 다른 trace event로 기록한다. |
| Day 28 | Architecture Recap | 코어, 정책, 실험 계층과 현재 production 경계를 정리한다. |

### Day 15 — Helping Wait

`ThreadPool::WaitWithHelping(handle)`을 추가했습니다.
일반 `JobHandle::Wait()`는 호출 스레드를 잠재우지만, `WaitWithHelping()`은 기다리는 동안 큐에서 작업을 꺼내 현재 스레드가 직접 실행합니다.

Day 10의 문제는 워커가 `Wait()`로 막히면 큐에 남은 자식 작업을 실행할 스레드가 부족해진다는 점이었습니다.
Helping Wait은 기다리는 워커도 진행에 기여하게 만들어 이 starvation 위험을 줄입니다.

Day22 이후 일반 worker loop는 local -> global -> steal 순서로 작업을 찾습니다.
`WaitWithHelping()` 호출 스레드는 별도 helper로 취급해 global queue 다음으로 임의의 worker local queue를 확인합니다.
helper가 실행한 작업은 worker별 처리 통계에서 제외됩니다.
재진입 깊이 제한과 thread affinity 정책은 아직 남아 있습니다.

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

### Day 17 — Job Graph Builder

`experiments/Day17_JobGraphBuilder.cpp`에서 `JobGraphBuilder`를 실험했습니다.
기존 `ThreadPool` 코어를 직접 바꾸지 않고, `SubmitAfter()`와 `SubmitAfterAllSucceeded()` 위에 사용성 계층을 얹는 방식입니다.

핵심 구조:

| 구성 | 역할 |
|------|------|
| `JobId` | 그래프 내부에서 노드를 가리키는 식별자 |
| `AddJob()` | 작업 이름, 의존성 목록, 실행 함수, 실패 정책을 선언 |
| `Run(pool)` | 선언된 노드를 순서대로 `ThreadPool`에 제출하고 `JobHandle` 목록 반환 |
| `DependencyPolicy::Completion` | 선행 작업 성공/실패와 무관하게 완료 후 실행 |
| `DependencyPolicy::AllSucceeded` | 선행 작업이 모두 성공했을 때만 실행 |

실험은 세 가지를 확인합니다.

| 실험 | 확인 내용 |
|------|----------|
| 선형 파이프라인 | A → B → C 순서를 그래프 선언으로 표현 |
| Fan-out / Fan-in | 여러 독립 작업이 모두 끝난 뒤 합산 작업 실행 |
| 노드별 실패 정책 | 같은 실패 선행 작업 뒤에서도 cleanup은 실행, product는 취소 |

Day 17의 의미는 성능 최적화보다 API 사용성입니다.
작업 그래프가 커질수록 `SubmitAfter()` 호출이 코드 곳곳에 흩어지면 구조를 검토하기 어렵습니다.
Builder 계층은 그래프를 먼저 선언하고, 실행은 한 번에 넘기게 만들어 의존성 구조를 읽기 쉽게 합니다.

### Day 18 — Graph Validation / Topological Sort

`experiments/Day18_GraphValidation.cpp`에서 그래프 실행 전 검증 계층을 추가했습니다.
Day17은 dependency 노드가 먼저 선언되어야 했지만, Day18은 모든 노드를 만든 뒤 `AddDependency(job, dependency)`로 연결할 수 있습니다.

`Validate()`가 확인하는 오류:

| 오류 | 의미 |
|------|------|
| invalid `JobId` | 존재하지 않는 노드에 연결된 edge |
| self dependency | 노드가 자기 자신을 선행 작업으로 참조 |
| duplicate dependency | 같은 edge가 두 번 선언됨 |
| cycle | A → B → C → A처럼 시작점이 없는 순환 의존성 |

cycle 검사는 DFS 3-color 방식으로 구현했습니다.
`Unvisited`, `Visiting`, `Visited` 상태를 두고 현재 탐색 경로의 `Visiting` 노드를 다시 만나면 cycle path를 진단합니다.

유효한 그래프는 DFS 후위 순서로 dependency가 먼저 오는 topological order를 생성합니다.
`Run()`은 검증 실패 시 `GraphValidationException`을 던지고 아무 작업도 제출하지 않으며, 성공한 경우에만 해당 순서로 기존 `ThreadPool` API를 호출합니다.

### Day 19 — Graph Execution Report

`experiments/Day19_GraphExecutionReport.cpp`에서 각 그래프 노드의 실행 구간을 계측합니다.
작업 함수를 래핑해 `ready`, `start`, `end`, 상태, 실행 스레드를 기록하고 실행이 끝난 뒤 표 형태의 보고서를 만듭니다.

| 지표 | 의미 |
|------|------|
| queue wait | dependency가 끝나 실행 가능해진 시점부터 실제 시작까지의 시간 |
| execution time | 작업 함수가 실제로 실행된 시간 |
| graph elapsed | 그래프 실행 시작부터 모든 노드 완료까지의 wall-clock 시간 |
| critical path | dependency를 따라 누적 실행 시간이 가장 긴 경로 |

첫 번째 실험은 worker 2개에 독립 작업 6개를 제출해, 뒤쪽 작업의 queue wait가 증가하는 현상을 보여줍니다.
두 번째 실험은 fan-out/fan-in 그래프에서 `Load -> Parse -> Shaders -> Package -> Upload`가 critical path로 계산되는지 확인합니다.
세 번째 실험은 실패 노드를 보고서에 남기고, 완료 기반 후속 작업은 실행되지만 성공 기반 후속 작업은 `canceled`로 기록되는지 확인합니다.

실행 시간이 가장 긴 단일 노드와 critical path는 같은 개념이 아닙니다.
그래프 전체 완료 시간을 줄이려면 dependency를 따라 누적되는 경로를 먼저 찾아야 하며, queue wait가 크다면 작업 자체보다 worker 수나 스케줄링 정책을 점검해야 합니다.

### Day 20 — Chrome Trace JSON Export

`experiments/Day20_ChromeTraceExport.cpp`는 그래프 실행 기록을 Chrome Trace Event JSON 형식으로 저장합니다.
생성된 파일은 Perfetto UI 또는 `chrome://tracing`에서 열 수 있습니다.

| Trace event | 표현 내용 |
|-------------|-----------|
| complete event (`X`) | worker thread에서 실제 작업이 실행된 구간 |
| queue wait (`X`) | dependency 완료 후 worker를 얻기까지 기다린 구간 |
| flow (`s` / `f`) | 선행 작업 종료에서 후속 작업 시작으로 이어지는 dependency |
| metadata (`M`) | process와 worker/queue lane 이름 |

실행 예시:

```powershell
out\build\x64-debug\Day20_ChromeTraceExport.exe Day20_JobTrace.json
```

텍스트 표는 정확한 수치를 비교하기 좋지만, 병렬 실행의 겹침과 빈 구간을 한눈에 읽기는 어렵습니다.
Trace viewer에서는 worker lane의 활용률, queue wait, fan-out/fan-in 흐름을 시간축 위에서 함께 확인할 수 있습니다.

### Day 21 — JobState Pool

`experiments/Day21_JobStatePool.cpp`에서 `JobState` 객체 재사용 구조를 독립 실험했습니다.
기존 `ThreadPool`은 작업 제출마다 `shared_ptr<JobState>`를 새로 만들기 때문에 작업 수가 많아질수록 heap allocation pressure가 커집니다.

실험 구조:

| 구성 | 역할 |
|------|------|
| `JobStatePool::Acquire()` | free list에서 상태 객체를 꺼내거나 새로 생성 |
| custom deleter | 마지막 `shared_ptr` 참조가 사라질 때 pool로 반환 |
| `ResetForReuse()` | `done`, `exception`, `continuations`를 초기 상태로 복원 |
| generation | 같은 객체가 재사용됐는지 확인하는 관찰용 카운터 |

핵심 수명 규칙은 간단합니다.
핸들이 하나라도 살아 있으면 해당 `JobState`는 pool로 돌아가지 않습니다.
마지막 핸들이 파괴된 뒤에만 custom deleter가 호출되고, 그 시점에 상태를 reset한 뒤 free list로 반환합니다.

이번 실험은 `JobState` 객체 allocation 수를 줄이는 데 초점을 둡니다.
`shared_ptr` control block allocation은 여전히 남아 있으므로, 더 강하게 최적화하려면 intrusive ref count나 custom allocator까지 별도로 검토해야 합니다.

### Day 22 — ThreadPool Local Queue Integration

Day16의 독립 `WorkStealingPool` 구조를 실제 `ThreadPool` 실행 경로에 작게 통합했습니다.
외부 스레드에서 호출한 `Submit()`은 기존처럼 global queue에 들어가고, worker 스레드가 작업 실행 중 다시 `Submit()`한 child 작업은 해당 worker의 local queue에 들어갑니다.

실행 우선순위:

| 순서 | 대상 | 이유 |
|------|------|------|
| 1 | 자기 local queue | worker가 방금 만든 child 작업을 빠르게 이어서 처리 |
| 2 | global queue | 외부 제출 작업의 공정한 소비 |
| 3 | 다른 worker local queue steal | idle worker가 과부하 worker의 일을 가져와 부하 불균형 완화 |

통계도 추가했습니다.
`GetQueueStats()`는 worker별 `globalPops`, `localPops`, `steals`, `jobsProcessed`를 반환합니다.
Day22 실험은 외부 제출이 global queue를 타는지, worker 내부 제출이 local queue를 타는지, 과부하 local queue에서 steal이 발생하는지를 확인합니다.

아직 lock-free deque는 아닙니다.
이번 단계의 목적은 성능 극대화가 아니라 기존 `ThreadPool` API를 유지하면서 local queue / stealing 정책을 안전하게 연결하는 것입니다.

### Day 23 — Trace Ring Buffer

Day20에서는 전체 trace를 실행 후 파일로 내보냈습니다.
Day23은 런타임 수집에 더 가까운 구조로, 고정 크기 `TraceRingBuffer`를 실험했습니다.

구조는 단순합니다.

| 구성 | 역할 |
|------|------|
| capacity | buffer가 보존할 수 있는 event 한도 |
| sequence | event가 기록된 전역 순서 |
| snapshot | 현재 buffer에 남아 있는 최근 event를 순서대로 복사 |
| overwritten count | capacity를 초과해서 밀려난 old event 수 |

실험은 세 가지를 확인합니다.

| 실험 | 확인 내용 |
|------|----------|
| buffer large enough | 기록 event 수가 capacity 이하면 모든 event가 남는다. |
| fixed recent window | burst로 capacity를 초과하면 old event가 overwrite된다. |
| trace export | 남아 있는 최근 window만 Chrome Trace JSON으로 내보낸다. |

핵심은 trade-off입니다.
Ring buffer는 메모리 사용량을 고정하기 좋지만 전체 history를 보장하지 않습니다.
따라서 trace를 분석할 때는 `total written`, `retained`, `overwritten`를 같이 봐야 합니다.

### Day 24 — Week 3 Recap

Week 3의 핵심은 ThreadPool을 단순한 작업 큐에서 **정책을 관찰하고 교체할 수 있는 JobSystem**으로 확장한 것입니다.

전체 흐름:

```text
Graph declaration
    -> validation / topological order
    -> dependency가 준비된 job 제출
    -> global 또는 worker local queue로 routing
    -> local pop / global pop / steal
    -> job 실행과 JobState 완료
    -> continuation 제출
    -> report / trace event 수집
```

#### 1. Wait 정책과 진행 보장

| 방식 | 호출 스레드 | 장점 | 비용/위험 |
|------|-------------|------|-----------|
| `JobHandle::Wait()` | block | 구현과 의미가 단순함 | worker에서 사용하면 starvation 가능 |
| `WaitWithHelping()` | 대기 중 작업 실행 | worker slot 낭비를 줄임 | 재진입과 실행 순서가 복잡해짐 |
| continuation | block하지 않음 | dependency graph를 자연스럽게 진행 | 상태와 failure policy 관리 필요 |

Helping wait은 deadlock을 자동으로 없애는 만능 해법이 아닙니다.
대기 중 임의의 작업을 실행하므로 호출 스택이 깊어질 수 있고, 작업이 암묵적으로 thread affinity를 가정하면 문제가 생길 수 있습니다.

#### 2. Queue 정책과 부하 분산

| 제출 위치 | queue | 소비 방식 |
|-----------|-------|-----------|
| 외부 스레드 | global queue | worker가 FIFO pop |
| worker 내부 | 해당 worker local queue | owner가 LIFO pop |
| idle worker | 다른 worker local queue | 앞쪽에서 FIFO steal |

local queue는 parent가 만든 child의 locality를 높이고 global mutex 경쟁을 줄입니다.
반면 queue가 여러 개로 분산되므로 종료 조건, wake-up predicate, pending count가 모든 queue를 함께 고려해야 합니다.
`GetQueueStats()`의 local/global pop과 steal 수는 이 정책이 실제 workload에서 작동했는지 확인하기 위한 최소 계측입니다.

#### 3. Graph 계층과 실행 정책

| 단계 | 책임 |
|------|------|
| builder | node와 dependency를 선언 |
| validation | invalid edge, duplicate, self dependency, cycle 차단 |
| topological order | dependency가 먼저 제출되도록 순서 생성 |
| failure policy | 완료 후 실행 또는 전체 성공 후 실행 선택 |
| execution report | queue wait, 실행 시간, critical path 계산 |

그래프 구조와 스케줄러 구현은 분리해야 합니다.
그래프는 **무엇이 무엇에 의존하는지**를 표현하고, ThreadPool은 **준비된 작업을 어느 worker가 실행할지**를 결정합니다.

#### 4. 관측성과 메모리 비용

| 도구/구조 | 얻는 것 | 잃는 것 |
|-----------|---------|---------|
| text report | 정확한 수치와 critical path | 병렬 구간을 직관적으로 보기 어려움 |
| Chrome Trace | worker overlap과 dependency 흐름 | event 저장/직렬화 비용 |
| trace ring buffer | 고정 메모리, 최근 구간 보존 | 오래된 history overwrite |
| `JobStatePool` | 상태 객체 재사용 | pool lifetime과 reset 규칙 복잡도 |

성능 최적화는 측정과 함께 들어가야 합니다.
local queue, stealing, pooling은 구조를 복잡하게 만들기 때문에 queue 통계와 trace 없이 적용하면 실제 개선 여부를 판단하기 어렵습니다.

#### 현재 경계

다음 항목은 아직 production 수준 구현이 아닙니다.

| 항목 | 현재 상태 |
|------|-----------|
| local queue | mutex 기반 `deque`; lock-free work-stealing deque 아님 |
| `JobStatePool` | 독립 실험이며 기본 `ThreadPool`에는 미통합 |
| graph builder/report | 실험 파일별 구현이며 공용 라이브러리 API로 추출되지 않음 |
| trace ring buffer | mutex 기반 producer serialization |
| cancellation | 실행 전 후속 작업 취소만 표현; 실행 중 cooperative cancellation 없음 |
| affinity/priority | worker affinity와 job priority 정책 없음 |

Week 3의 결론은 더 많은 기능 자체가 아닙니다.
**진행 보장, 부하 분산, 의존성 검증, 관측 가능성, 메모리 비용을 서로 다른 정책으로 보고 각각 검증해야 한다**는 점이 핵심입니다.

### Day 25 — API and Comment Cleanup

새 기능을 추가하지 않고 `ThreadPool`의 공개 표면과 설명을 현재 구현에 맞췄습니다.
기존 실험이 그대로 빌드되도록 함수 이름과 반환 타입은 유지했습니다.

정리한 항목:

| 항목 | 변경 |
|------|------|
| `JobHandle` 문서 | 존재하지 않는 `SubmitWithHandle()` 예시를 현재 `Submit()` API로 수정 |
| queue 설명 | global queue 하나만 표현하던 구조도를 global/local/steal 정책으로 갱신 |
| `WaitWithHelping()` | Day22 이후 실제 탐색 순서를 문서에 반영 |
| query API | `IsDone()`, `HasException()`, 통계 getter 등에 `[[nodiscard]]` 추가 |
| statistics 구현 | 긴 inline 본문을 `ThreadPool.cpp`로 이동해 헤더에는 계약만 유지 |
| condition variable 설명 | 현재 구현의 `notify_all()` 정책과 일치하도록 수정 |

이 단계의 핵심은 cleanup에서도 호환성을 지키는 것입니다.
API 이름을 바꾸거나 통계 구조를 합치면 코드는 더 짧아질 수 있지만, 이전 Day 실험 전체를 수정해야 합니다.
Day25는 기존 학습 기록을 보존하면서 잘못된 설명과 헤더 노이즈만 줄였습니다.

### Day 26 — ThreadPool Stress / Regression Suite

Day01~25에서 개별적으로 확인한 핵심 계약을 `Day26_RegressionSuite` 하나로 묶었습니다.
각 검사는 조건 불일치 시 예외를 던지고, suite는 `[PASS]` / `[FAIL]`과 실행 시간을 출력한 뒤 실패가 하나라도 있으면 non-zero exit code를 반환합니다.

검사 항목:

| 검사 | 보장하는 계약 |
|------|---------------|
| high-volume external submit | 모든 외부 작업이 완료되고 pending count가 0으로 돌아옴 |
| nested submit and local/steal | worker 내부 child가 local pop 또는 steal 경로에서 유실 없이 처리됨 |
| dependency fan-in | 모든 dependency 완료 후에만 fan-in continuation 실행 |
| failure policies | 완료 기반 continuation은 실행되고 성공 기반 continuation은 취소됨 |
| helping wait under saturation | 모든 worker가 parent를 실행 중이어도 child 작업이 진행됨 |
| repeated pool lifecycle | 반복 생성, 제출, 대기, 소멸 과정에서 작업 유실 없음 |

기본 실행과 stress 실행:

```powershell
out\build\x64-debug\Day26_RegressionSuite.exe
out\build\x64-debug\Day26_RegressionSuite.exe 10
```

두 번째 인자는 workload multiplier이며 `1~100` 범위입니다.
기본 패스와 10배 패스 모두 6개 검사가 통과하는 것을 확인했습니다.
실행 시간은 환경에 따라 달라지므로 절대 성능 기준이 아니라 회귀 위치를 찾기 위한 참고값입니다.

### Day 27 — Failure / Cancel Trace

`experiments/Day27_FailureCancelTrace.cpp`는 성공, 실패, 취소 상태를 하나의 Chrome Trace JSON에 기록합니다.

실행된 성공/실패 작업은 duration event(`X`)로 기록합니다. Dependency 실패로 본문이 실행되지 않은 작업은 가짜 실행 구간을 만들지 않고 scheduler lane의 instant event(`i`)로 기록합니다. 실패 또는 상위 취소에서 하위 취소로 이어지는 관계는 flow event(`s`/`f`)로 연결합니다.

```powershell
out\build\x64-debug\Day27_FailureCancelTrace.exe
out\build\x64-debug\Day27_FailureCancelTrace.exe Day27_FailureCancelTrace.json
```

기본 시나리오는 `LoadInput -> Decode(failed)` 이후 completion 정책의 `Cleanup`은 실행하고, all-succeeded 정책의 `BuildOutput`과 그 후속 `Publish`는 취소합니다. 취소 작업은 실제 worker 본문을 실행하지 않으므로 worker 실행 구간을 갖지 않습니다.

현재 `ThreadPool`은 cancellation callback이나 정확한 cancel timestamp를 공개하지 않습니다. 따라서 trace의 취소 시각은 마지막 dependency 완료 시각으로 추론하며 JSON의 `timestamp_source`에 이 사실을 남깁니다.

### Day 28 — Architecture Recap

Day01~27의 결과를 현재 코드 기준으로 다시 나누면, 이 저장소는 하나의 거대한 JobSystem이 아니라 **작게 유지한 실행 코어와 교체 가능한 정책 실험 모음**입니다.

#### 현재 전체 흐름

```text
Submit / SubmitAfter / SubmitAfterAllSucceeded
    -> JobState 생성과 JobHandle 반환
    -> 외부 제출은 global FIFO
       worker 내부 제출은 해당 worker local LIFO
    -> worker: local -> global -> steal
       helper: global -> steal any
    -> wrapped job 실행
    -> 성공 또는 exception_ptr 저장
    -> JobState 완료 + continuation 호출
    -> pending count 감소
    -> Wait / WaitAll / WaitWithHelping 해제
```

#### 코어에 실제 통합된 책임

| 영역 | 현재 구현 | 소유 위치 |
|------|-----------|-----------|
| 실행 | worker 생성, queue 소비, shutdown 시 이미 queue에 들어온 작업 drain | `ThreadPool` |
| 작업 상태 | 완료 flag, 예외, continuation 목록 | `JobState` |
| 외부 관찰 | 단일 작업 대기, 완료/예외 query | `JobHandle` |
| 의존성 | completion 또는 all-succeeded continuation | `ThreadPool::SubmitAfter*` |
| 큐 정책 | global FIFO, owner local LIFO, thief FIFO | `ThreadPool` queue helpers |
| 진행 보조 | 대기 스레드가 global/steal 작업 실행 | `WaitWithHelping()` |
| 완료 추적 | queued + running 작업을 하나의 atomic count로 추적 | `_pendingJobs` |
| 최소 계측 | worker별 처리량, local/global pop, steal count | `GetQueueStats()` |

핵심 불변식은 다음과 같습니다.

1. 제출되어 실행 가능한 작업은 global 또는 local queue 중 한 곳에만 존재합니다.
2. `_pendingJobs`는 queue 대기와 실행 중 작업을 모두 포함하며, 실행 종료 시 정확히 한 번 감소합니다.
3. 정상 실행 경로의 `JobState`는 성공과 실패 모두에서 완료되며, 상태 lock 밖에서 continuation을 호출합니다.
4. dependency counter는 성공 여부가 아니라 완료 수를 추적합니다.
5. success-only 정책만 dependency exception을 읽어 후속 작업을 `JobCanceledException`으로 완료합니다.
6. worker와 helper가 어느 경로에서 작업을 가져가도 동일한 wrapped job 완료 경로를 사용합니다.

#### 동기화 경계

| 공유 자원 | 보호 방식 | 이유 |
|-----------|-----------|------|
| global queue / stop 전환 | `_queueMutex` | 외부 제출과 worker wake-up 조건 직렬화 |
| worker local queue | queue별 mutex | owner pop과 다른 worker steal 충돌 방지 |
| JobState exception/continuation | `JobState::mutex` | 완료와 continuation 등록 경쟁 방지 |
| JobState 완료 여부 | atomic + condition variable | 빠른 query와 blocking wait 모두 지원 |
| 전체 pending count | atomic + completion CV | queue 위치와 무관한 전체 완료 조건 제공 |
| 통계 | worker별 atomic counter | 실행 경로를 막지 않고 관찰 |

#### 코어와 분리된 실험 계층

| 실험 | 검증한 설계 | 현재 상태 |
|------|-------------|-----------|
| graph builder / validation | 선언형 dependency와 cycle 검증 | 공용 코어 API로 미통합 |
| execution report | queue wait, 실행 시간, critical path | 실험별 record 구조 사용 |
| Chrome Trace | worker overlap과 dependency flow | 실행 후 파일 export |
| trace ring buffer | 고정 메모리 recent window | mutex 기반 독립 recorder |
| JobState pool | allocation 재사용과 generation | 기본 `shared_ptr<JobState>`를 대체하지 않음 |
| failure/cancel trace | failed duration과 canceled instant 구분 | cancel 시각은 dependency 완료로 추론 |

이 분리는 의도적입니다. 실험 결과가 유효하더라도 코어 API에 바로 합치면 이전 Day의 계약과 사용 예제를 함께 바꿔야 합니다. 먼저 독립 실험과 Day26 regression suite로 정책을 검증한 뒤, 실제 사용 요구가 생길 때 통합하는 편이 변경 비용을 통제하기 쉽습니다.

#### 현재 보장과 현재 한계

| 구분 | 내용 |
|------|------|
| 보장 | 외부/중첩 제출 완료, fan-in 순서, failure policy, helping wait 진행, 반복 lifecycle |
| 보장하지 않음 | 작업 실행 순서, 특정 worker 실행, 절대 성능, lock-free 진행성 |
| 취소 한계 | 실행 전 success-only continuation 취소만 지원; 실행 중 cooperative cancel 없음 |
| scheduling 한계 | priority, affinity, NUMA 인식, starvation fairness 정책 없음 |
| memory 한계 | 기본 경로는 작업마다 `shared_ptr<JobState>` 할당 |
| observability 한계 | 코어 내장 trace가 아니며 실험 wrapper가 계측 |
| graph 한계 | graph 타입과 validation이 공용 라이브러리 표면에 없음 |
| lifecycle 한계 | pool 소멸과 dependency continuation 제출이 겹치는 동시성 계약은 미정의 |

#### Production 방향으로 확장한다면

현재 학습 코어를 실제 엔진 시스템으로 확장할 때의 우선순위는 기능 수가 아니라 계약 강화입니다.

1. cancellation token과 정확한 상태 전이 정의
2. graph/trace 타입의 공용 API 여부 결정
3. JobState allocator 또는 intrusive handle 소유권 설계
4. bounded queue와 overload/back-pressure 정책
5. shutdown 중 submit, cancel, dependency 등록의 경쟁 조건 명세
6. sanitizer, 장시간 soak, randomized scheduling 검증

Day28의 결론은 **queue, wait, dependency, failure, memory, observability를 각각 독립 정책으로 유지해야 확장과 검증이 가능하다**는 것입니다.

---

## 다음 방향

30일 완주를 위한 남은 단계:

| Day | 방향 |
|------|------|
| Day 29 | README / portfolio packaging |
| Day 30 | final review, remaining work list, one-month close |
