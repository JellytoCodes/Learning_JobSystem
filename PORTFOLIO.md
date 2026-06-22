# C++ JobSystem Case Study

## 한 줄 요약

C++17 표준 라이브러리만으로 작업 제출, 의존성, 실패 전파, helping wait,
worker-local queue와 work stealing을 단계적으로 구현하고 회귀 테스트와 trace로 계약을 검증한 프로젝트입니다.

## 구현 목표

완성된 스레드 풀을 가져다 쓰는 대신, 게임 엔진의 JobSystem에서 문제가 되는 지점을 작은 실험으로 분리했습니다.

- worker가 작업을 기다리고 깨는 방식
- 작업 하나의 완료와 실패를 표현하는 방식
- fan-in dependency와 continuation을 연결하는 방식
- worker가 다른 작업을 기다릴 때 발생하는 starvation
- global/local queue와 work stealing의 역할
- 동시성 계약을 반복 검증하고 실행 흐름을 관측하는 방식

## 현재 구조

```text
external Submit -> global FIFO ----+
                                    |
worker Submit   -> owner local LIFO +-> worker: local -> global -> steal
                                    |             |
                                    |             +-> execute wrapped job
                                    |                         |
                                    +-------------------------+
                                                              v
JobHandle <- JobState <- success / exception <- continuation release
```

공용 실행 코어는 [`src/ThreadPool.h`](src/ThreadPool.h)와
[`src/ThreadPool.cpp`](src/ThreadPool.cpp)에 있습니다. 각 날짜의 코드는 새로운 정책을
독립적으로 검증한 뒤 코어에 통합할지 판단하는 실험 기록입니다.

## 핵심 설계 결정

### 완료와 성공을 분리

`SubmitAfter()`는 dependency가 성공하거나 실패해도 모두 완료되면 실행합니다.
`SubmitAfterAllSucceeded()`는 하나라도 실패하면 본문을 실행하지 않고
`JobCanceledException`으로 완료합니다. dependency counter는 완료 개수만 추적하고,
실행 여부는 별도 failure policy가 결정합니다.

### blocking wait의 진행 보장 보완

worker가 자식 작업을 기다리며 모두 멈추는 상황을 줄이기 위해
`WaitWithHelping()`은 대기 중인 스레드가 global queue와 worker queue의 작업을 직접
처리하도록 합니다. 이는 무조건적인 deadlock 방지 보장이 아니라, 현재 큐 정책에서
진행 가능성을 높이는 명시적 wait policy입니다.

### 제출 위치에 따른 큐 선택

외부 스레드의 제출은 global FIFO로, worker 내부 제출은 해당 worker의 local LIFO로
보냅니다. owner는 최근 작업을 먼저 처리하고, idle worker는 다른 local queue의 오래된
작업을 steal합니다. 큐별 mutex를 사용하며 lock-free deque를 주장하지 않습니다.

### 예외를 worker 밖으로 전파

작업 본문의 예외는 worker를 종료시키지 않고 `JobState`에 `std::exception_ptr`로
저장합니다. 호출자는 `JobHandle::Wait()`에서 같은 실패를 관찰합니다.

## 대표 구현과 검증 근거

| 관점 | 대표 파일 | 확인할 내용 |
|------|-----------|-------------|
| 실행 코어 | [`src/ThreadPool.cpp`](src/ThreadPool.cpp) | queue routing, wrapped job 완료, continuation 제출 |
| local queue 통합 | [`experiments/Day22_ThreadPoolLocalQueue.cpp`](experiments/Day22_ThreadPoolLocalQueue.cpp) | nested submit과 steal 경로 |
| 고정 메모리 trace | [`experiments/Day23_TraceRingBuffer.cpp`](experiments/Day23_TraceRingBuffer.cpp) | bounded recent-event window |
| 계약 회귀 테스트 | [`experiments/Day26_RegressionSuite.cpp`](experiments/Day26_RegressionSuite.cpp) | 6개 핵심 계약과 non-zero failure exit |
| 실패/취소 관측 | [`experiments/Day27_FailureCancelTrace.cpp`](experiments/Day27_FailureCancelTrace.cpp) | failed duration과 canceled scheduler event 구분 |

Day26 회귀 테스트는 다음 계약을 한 실행에서 검사합니다.

1. high-volume external submit의 유실 없는 완료
2. nested submit의 local pop 또는 steal 처리
3. dependency fan-in의 실행 순서
4. completion 기반과 success-only 기반 failure policy
5. worker 포화 상태의 helping wait 진행
6. 반복적인 pool 생성과 종료

## 재현 방법

Visual Studio Developer PowerShell 또는 CMake를 사용할 수 있는 PowerShell에서 실행합니다.

```powershell
cmake --preset x64-debug
cmake --build out\build\x64-debug
out\build\x64-debug\Day26_RegressionSuite.exe 10
```

성공 기준은 `6/6 tests passed`와 프로세스 exit code `0`입니다. 숫자 `10`은 workload
multiplier이며 성능 수치가 아니라 동시성 계약을 반복해서 흔들어 보기 위한 입력입니다.

실패와 취소 trace는 다음과 같이 생성할 수 있습니다.

```powershell
out\build\x64-debug\Day27_FailureCancelTrace.exe Day27_FailureCancelTrace.json
```

생성된 JSON은 `chrome://tracing` 또는 Perfetto UI에서 열어 worker 실행 구간과 dependency
flow를 확인할 수 있습니다.

## 현재 경계

이 프로젝트는 학습용 구현이며 다음 항목은 아직 production 보장 범위가 아닙니다.

- 실행 중 작업의 cooperative cancellation
- priority, affinity, NUMA-aware scheduling
- bounded queue와 back-pressure
- lock-free work-stealing deque
- `JobStatePool`의 기본 코어 통합
- graph와 trace 타입의 공용 라이브러리 API
- shutdown과 continuation 등록이 동시에 발생할 때의 완전한 lifecycle 명세
- sanitizer, 장시간 soak, randomized scheduler 기반 검증

이 경계를 명시한 이유는 구현한 기능과 검증하지 않은 보장을 구분하기 위해서입니다.

## 배운 점

JobSystem의 난점은 스레드 개수보다 계약의 조합에 있었습니다. queue policy, wait policy,
dependency completion, failure propagation, memory ownership, observability를 분리해야 각 문제를
독립적으로 검증할 수 있습니다. 기능을 코어에 바로 합치기 전에 작은 실행 파일로 가설을
검증하고, 통합 후에는 Day26 회귀 테스트로 기존 계약을 다시 확인하는 흐름을 유지했습니다.
