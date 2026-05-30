// =============================================================================
// Day 06 — 데이터 레이스 재현: int vs atomic<int>
//
// 목표:
//   _pendingJobs 같은 공유 카운터를 일반 int로 썼을 때
//   실제로 어떤 일이 벌어지는지 수치로 확인한다.
//
// 핵심 개념: 데이터 레이스 (Data Race)
//   둘 이상의 스레드가 동기화 없이 같은 메모리를 읽고 쓸 때 발생.
//   C++ 표준에서는 데이터 레이스를 Undefined Behavior로 규정한다.
//   즉, 컴파일러가 어떤 결과를 내도 "맞는" 동작이 된다.
//
// ++ 연산이 왜 위험한가?
//   counter++ 는 한 줄처럼 보이지만 실제로는 3단계다.
//     1. 메모리에서 값을 레지스터로 LOAD
//     2. 레지스터에서 +1
//     3. 레지스터 값을 메모리에 STORE
//   두 스레드가 1단계를 동시에 실행하면 같은 값을 읽고,
//   각자 +1 한 뒤 덮어쓰므로 → 증가가 1번만 일어나는 것처럼 된다.
//   이게 "소실된 업데이트(Lost Update)" 문제다.
//
// 실행 방법 (experiments 폴더에서):
//   g++ -std=c++17 -O1 -pthread Day06_DataRace.cpp -o Day06 && ./Day06
//
//   ThreadSanitizer로 레이스 탐지 (권장):
//   g++ -std=c++17 -O1 -pthread -fsanitize=thread -g Day06_DataRace.cpp -o Day06_tsan && ./Day06_tsan
// =============================================================================

#include <iostream>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <string>

using Clock    = std::chrono::high_resolution_clock;
using Duration = std::chrono::duration<double, std::milli>;

// =============================================================================
// 공통 파라미터
// =============================================================================
constexpr uint32_t kThreadCount   = 8;
constexpr uint64_t kOpsPerThread  = 1'000'000;   // 스레드당 증가 횟수
constexpr uint64_t kExpected      = kThreadCount * kOpsPerThread;

// =============================================================================
// 실험 A: volatile int — 데이터 레이스 발생
//
// volatile이란?
//   컴파일러 최적화(값 캐싱, 연산 제거)를 막는 키워드.
//   멀티스레딩 동기화와는 무관하다.
//   volatile을 써도 데이터 레이스는 그대로 발생한다.
//   "volatile이 스레드 안전하다"는 흔한 오해다.
// =============================================================================
static void ExperimentA_VolatileInt()
{
    std::cout << "\n[A] volatile int (데이터 레이스 발생)\n";
    std::cout << "---------------------------------------------\n";

    volatile int counter = 0;   // volatile이어도 레이스는 발생한다

    std::vector<std::thread> threads;
    threads.reserve(kThreadCount);

    auto start = Clock::now();

    for (uint32_t t = 0; t < kThreadCount; ++t)
    {
        threads.emplace_back([&counter]
        {
            for (uint64_t i = 0; i < kOpsPerThread; ++i)
                ++counter;  // LOAD → ADD → STORE (3단계, 원자적이지 않음)
        });
    }

    for (auto& th : threads) th.join();

    Duration elapsed = Clock::now() - start;

    const int64_t result    = counter;
    const int64_t lost      = static_cast<int64_t>(kExpected) - result;
    const double  lostRatio = static_cast<double>(lost) / kExpected * 100.0;

    std::cout << "  기댓값  : " << kExpected << "\n";
    std::cout << "  실제값  : " << result    << "\n";
    std::cout << "  소실    : " << lost      << " (" << std::fixed << std::setprecision(1)
              << lostRatio << "%)\n";
    std::cout << "  시간    : " << elapsed.count() << " ms\n";

    if (result != static_cast<int64_t>(kExpected))
        std::cout << "  ❌ 데이터 레이스 확인 — 값이 소실됨\n";
    else
        std::cout << "  ⚠ 우연히 맞음 (레이스가 없다는 보장은 없음, 반복 실행 권장)\n";

    // ─────────────────────────────────────────────────────────────────────────
    // [관찰 포인트]
    //   결과가 kExpected보다 작으면 Lost Update가 발생한 것.
    //   실행마다 결과가 달라지는 것도 데이터 레이스의 특징이다.
    //   이런 버그는 디버거로 재현이 어렵다 — 관찰 자체가 타이밍을 바꾸기 때문.
    // ─────────────────────────────────────────────────────────────────────────
}

// =============================================================================
// 실험 B: std::atomic<int> — 안전
//
// atomic::operator++는 LOCK XADD (x86) 같은 CPU 명령어 하나로 처리된다.
// LOAD + ADD + STORE가 분리되지 않아 중간에 다른 스레드가 끼어들 수 없다.
// =============================================================================
static void ExperimentB_AtomicInt()
{
    std::cout << "\n[B] std::atomic<int> (올바른 방식)\n";
    std::cout << "---------------------------------------------\n";

    std::atomic<int> counter{ 0 };

    std::vector<std::thread> threads;
    threads.reserve(kThreadCount);

    auto start = Clock::now();

    for (uint32_t t = 0; t < kThreadCount; ++t)
    {
        threads.emplace_back([&counter]
        {
            for (uint64_t i = 0; i < kOpsPerThread; ++i)
                ++counter;   // 원자적 연산 — CPU 명령어 하나로 처리
        });
    }

    for (auto& th : threads) th.join();

    Duration elapsed = Clock::now() - start;

    const int64_t result = counter.load();

    std::cout << "  기댓값  : " << kExpected << "\n";
    std::cout << "  실제값  : " << result    << "\n";
    std::cout << "  시간    : " << elapsed.count() << " ms\n";

    if (result == static_cast<int64_t>(kExpected))
        std::cout << "  ✅ 항상 정확\n";
}

// =============================================================================
// 실험 C: 성능 비교 — atomic의 비용
//
// atomic 연산은 CPU 캐시 일관성 프로토콜(MESI)을 통해 모든 코어 동기화.
// 이 비용이 얼마나 되는지 volatile int와 비교한다.
// (단, volatile int는 UB이므로 결과를 신뢰할 수 없음 — 속도 참고용)
// =============================================================================
static void ExperimentC_PerformanceCompare()
{
    std::cout << "\n[C] 성능 비교: volatile int vs atomic (참고용)\n";
    std::cout << "---------------------------------------------\n";

    // volatile int 속도 (UB이지만 속도 참고용)
    {
        volatile int counter = 0;
        auto start = Clock::now();
        std::vector<std::thread> threads;
        for (uint32_t t = 0; t < kThreadCount; ++t)
            threads.emplace_back([&counter]
            { for (uint64_t i = 0; i < kOpsPerThread; ++i) ++counter; });
        for (auto& th : threads) th.join();
        Duration elapsed = Clock::now() - start;
        std::cout << "  volatile int  : " << std::fixed << std::setprecision(1)
                  << elapsed.count() << " ms  (결과 신뢰 불가, 속도 참고만)\n";
    }

    // atomic<int> 속도
    {
        std::atomic<int> counter{ 0 };
        auto start = Clock::now();
        std::vector<std::thread> threads;
        for (uint32_t t = 0; t < kThreadCount; ++t)
            threads.emplace_back([&counter]
            { for (uint64_t i = 0; i < kOpsPerThread; ++i) ++counter; });
        for (auto& th : threads) th.join();
        Duration elapsed = Clock::now() - start;
        std::cout << "  atomic<int>   : " << elapsed.count() << " ms  (정확, 항상)\n";
    }

    // ─────────────────────────────────────────────────────────────────────────
    // [관찰 포인트]
    //   atomic이 volatile보다 느린 이유:
    //     모든 코어의 캐시에서 해당 캐시 라인을 무효화하고
    //     자신이 독점 소유해야 쓸 수 있다 (캐시 바운싱).
    //     kThreadCount가 많을수록, kOpsPerThread가 클수록 차이가 커진다.
    //
    //   해결책: 핫한 원자 변수를 스레드가 직접 공유하지 않게 설계.
    //     → Day 05에서 _perThreadJobCount를 스레드별로 분리한 이유가 바로 이것.
    //     각 스레드가 자기 인덱스에만 쓰므로 캐시 바운싱 없음.
    // ─────────────────────────────────────────────────────────────────────────
}

// =============================================================================
// 실험 D: ThreadPool의 _pendingJobs 시뮬레이션
//   Submit(++), Complete(--) 패턴에서 int vs atomic 비교
// =============================================================================
static void ExperimentD_PendingJobsSimulation()
{
    std::cout << "\n[D] _pendingJobs 패턴 시뮬레이션 (++ 후 --)\n";
    std::cout << "---------------------------------------------\n";

    constexpr uint64_t kJobs = 500'000;

    // int 버전 — 최종값이 0이어야 하지만 레이스로 틀릴 수 있음
    {
        volatile int pending = 0;
        std::vector<std::thread> threads;

        // Producer: ++ kJobs번
        threads.emplace_back([&pending, kJobs]
        {
            for (uint64_t i = 0; i < kJobs; ++i)
                ++pending;
        });

        // Consumer: -- kJobs번
        threads.emplace_back([&pending, kJobs]
        {
            for (uint64_t i = 0; i < kJobs; ++i)
                --pending;
        });

        for (auto& th : threads) th.join();
        std::cout << "  int 버전 최종값 (기댓값 0): " << pending;
        std::cout << (pending == 0 ? "  ⚠ 우연히 0 (운이 좋은 경우)" : "  ❌ 0이 아님") << "\n";
    }

    // atomic 버전 — 항상 0
    {
        std::atomic<int> pending{ 0 };
        std::vector<std::thread> threads;

        threads.emplace_back([&pending, kJobs]
        { for (uint64_t i = 0; i < kJobs; ++i) ++pending; });

        threads.emplace_back([&pending, kJobs]
        { for (uint64_t i = 0; i < kJobs; ++i) --pending; });

        for (auto& th : threads) th.join();
        std::cout << "  atomic 버전 최종값 (기댓값 0): " << pending.load();
        std::cout << (pending.load() == 0 ? "  ✅ 항상 0" : "  ❌ 오류") << "\n";
    }
}

// =============================================================================
// main
// =============================================================================
int main()
{
    std::cout << "=====================================================\n";
    std::cout << "  Day 06 — 데이터 레이스: int vs atomic\n";
    std::cout << "=====================================================\n";
    std::cout << "  스레드 수   : " << kThreadCount << "\n";
    std::cout << "  스레드당 ops: " << kOpsPerThread << "\n";
    std::cout << "  총 기댓값   : " << kExpected << "\n";
    std::cout << "\n  주의: 데이터 레이스는 비결정적이다.\n";
    std::cout << "  같은 코드도 실행마다 결과가 다를 수 있다.\n";
    std::cout << "  결과가 맞더라도 '운이 좋은 것'일 뿐이다.\n";

    // 3회 반복 — 비결정적 특성 확인
    for (int trial = 1; trial <= 3; ++trial)
    {
        std::cout << "\n==================== Trial " << trial << " ====================";
        ExperimentA_VolatileInt();
        ExperimentB_AtomicInt();
    }

    ExperimentC_PerformanceCompare();
    ExperimentD_PendingJobsSimulation();

    std::cout << "\n=====================================================\n";
    std::cout << "  오늘의 핵심\n";
    std::cout << "=====================================================\n";
    std::cout << "  counter++   = LOAD + ADD + STORE (3단계, 비원자적)\n";
    std::cout << "  두 스레드가 동시에 LOAD → 하나의 증가가 사라짐 (Lost Update)\n";
    std::cout << "  volatile    = 최적화 방지, 동기화와 무관\n";
    std::cout << "  atomic      = CPU 명령어 수준 원자성 보장\n";
    std::cout << "  데이터 레이스 = C++ Undefined Behavior\n";
    std::cout << "\n  ThreadSanitizer로 탐지하려면:\n";
    std::cout << "  -fsanitize=thread -g 옵션 추가 후 재빌드\n";
    std::cout << "=====================================================\n\n";

    return 0;
}