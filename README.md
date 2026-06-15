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
| Day 17 | Job Graph Builder | 여러 작업과 의존성을 선언 단계와 실행 단계로 분리한다. |
| Day 18 | Graph Validation | invalid edge와 cycle을 실행 전에 검증하고 topological order를 만든다. |
| Day 19 | Graph Execution Report | 노드별 queue wait와 실행 시간을 측정하고 critical path를 찾는다. |
| Day 20 | Chrome Trace Export | 실행, 대기, dependency 흐름을 Trace Event JSON으로 내보낸다. |
| Day 21 | JobState Pool | 작업 상태 객체를 재사용해 allocation pressure를 줄이는 구조를 실험한다. |
| Day 22 | ThreadPool Local Queue | 워커 내부 제출을 local queue로 보내고 idle worker가 steal한다. |

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

---

## 다음 방향

Week 3에서 다루기 좋은 후보:

| 후보 | 이유 |
|------|------|
| Trace ring buffer | 장시간 실행에서도 메모리를 제한하도록 고정 크기 event buffer를 실험한다. |
| Week 3 recap | wait, queue, graph, trace, pool 실험을 한 번 정리한다. |
| 마무리 로드맵 | 30일 완주를 위해 남은 실험과 정리 범위를 고정한다. |

Day 23 후보로는 **Trace ring buffer** 또는 **Week 3 recap**이 자연스럽습니다.
30일 완주를 목표로 한다면 Day23~24에서 관찰 도구를 마무리하고, Day25~30은 정리와 안정화 중심으로 닫는 편이 좋습니다.
