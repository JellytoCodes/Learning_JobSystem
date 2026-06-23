# JobSystem 30-Day Final Review

## 목적

이 문서는 30일 동안 진행한 C++ JobSystem 학습 프로젝트의 최종 정리입니다.
목표는 완성형 엔진 JobSystem을 선언하는 것이 아니라, `ThreadPool`에서 시작해 작업 추적,
의존성, 실패 정책, 대기 정책, queue routing, 관측성, 회귀 검증까지 어떤 계약이 필요한지
직접 구현하고 확인한 범위를 명확히 남기는 것입니다.

## 최종 산출물

| 구분 | 산출물 | 역할 |
|------|--------|------|
| 실행 코어 | [`src/ThreadPool.h`](src/ThreadPool.h), [`src/ThreadPool.cpp`](src/ThreadPool.cpp) | 작업 제출, worker lifecycle, queue routing, continuation 실행 |
| 대표 실험 | [`experiments/`](experiments) | 각 날짜별 동시성 이슈와 정책 검증 |
| 회귀 검증 | [`experiments/Day26_RegressionSuite.cpp`](experiments/Day26_RegressionSuite.cpp) | 핵심 계약을 반복 실행으로 확인 |
| trace 검증 | [`experiments/Day27_FailureCancelTrace.cpp`](experiments/Day27_FailureCancelTrace.cpp) | 실패와 취소 흐름을 Chrome Trace JSON으로 분리 관측 |
| 포트폴리오 요약 | [`PORTFOLIO.md`](PORTFOLIO.md) | 대표 설계 결정과 검증 근거를 case study 형태로 정리 |

## 완료된 핵심 계약

- `Submit()`으로 작업을 제출하고 worker가 queue에서 소비합니다.
- `JobHandle`로 특정 작업의 완료와 실패를 추적합니다.
- 작업 본문 예외는 worker를 죽이지 않고 `JobHandle::Wait()`에서 재전파됩니다.
- `SubmitAfter()`는 dependency의 성공 여부와 관계없이 완료 후 실행됩니다.
- `SubmitAfterAllSucceeded()`는 하나라도 실패한 dependency가 있으면 continuation을 취소 상태로 완료합니다.
- `WaitWithHelping()`은 worker가 대기 중에도 가능한 작업을 처리해 starvation 위험을 줄입니다.
- 외부 제출은 global FIFO, worker 내부 제출은 owner local LIFO로 라우팅됩니다.
- idle worker는 다른 worker의 local queue에서 작업을 steal할 수 있습니다.
- queue 통계, graph report, trace export, ring buffer 실험으로 실행 흐름을 관측했습니다.
- Day26 회귀 테스트로 주요 동시성 계약을 한 실행 파일에서 반복 검증합니다.

## 검증 기준

기본 재현 경로는 다음과 같습니다.

```powershell
cmake --preset x64-debug
cmake --build out\build\x64-debug
out\build\x64-debug\Day26_RegressionSuite.exe 10
```

성공 기준은 `Day26_RegressionSuite`가 다음 계약을 모두 통과하고 exit code `0`으로 종료하는 것입니다.

- high-volume external submit 완료
- nested submit과 local queue / steal 경로 처리
- dependency fan-in 실행 순서
- completion 기반과 success-only 기반 failure policy
- worker 포화 상태에서 helping wait 진행
- 반복적인 pool 생성과 종료 lifecycle

실패와 취소 흐름은 다음 명령으로 trace JSON을 생성해 확인합니다.

```powershell
out\build\x64-debug\Day27_FailureCancelTrace.exe Day27_FailureCancelTrace.json
```

## 현재 경계

이 프로젝트가 아직 production-ready JobSystem이라고 말하지 않는 이유는 다음과 같습니다.

- 실행 중 작업에 대한 cooperative cancellation token이 없습니다.
- priority, affinity, NUMA-aware scheduling을 지원하지 않습니다.
- bounded queue와 overload/back-pressure 정책이 없습니다.
- work-stealing queue는 lock-free deque가 아니라 mutex 기반입니다.
- `JobStatePool`은 독립 실험이며 기본 `ThreadPool`에 통합되지 않았습니다.
- graph와 trace는 공용 라이브러리 API로 정리되지 않았습니다.
- shutdown 중 submit, cancel, continuation 등록의 경계 조건이 완전히 명세되지 않았습니다.
- sanitizer, 장시간 soak, randomized scheduler 기반 검증이 아직 없습니다.

## 다음 확장 우선순위

1. shutdown과 continuation 등록의 lifecycle 계약을 먼저 명세합니다.
2. cancellation token을 도입할지, 현재처럼 dependency 취소만 유지할지 결정합니다.
3. `JobState` 소유권과 allocator 전략을 정리한 뒤 `JobStatePool` 통합 여부를 판단합니다.
4. bounded queue와 back-pressure를 추가해 overload 상황의 동작을 명확히 합니다.
5. sanitizer, 장시간 반복 실행, randomized scheduling 검증을 자동화합니다.
6. graph와 trace를 실험 코드에서 공용 API 후보로 승격할지 검토합니다.

## 한 달의 결론

ThreadPool의 어려움은 worker 개수나 queue 하나의 구현보다, 완료와 성공의 의미,
대기 중 진행 보장, 실패 전파, 의존성 해제, queue ownership, 종료 시점 같은 계약들이 서로
겹치는 지점에 있었습니다.

이번 30일의 결과는 그 계약들을 작은 실험으로 분해하고, 검증 가능한 형태로 다시 묶은 것입니다.
다음 단계에서 기능을 더 붙이려면 성능 숫자보다 먼저 어떤 계약을 public surface로 보장할지부터
정하는 것이 맞습니다.
