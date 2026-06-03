// =============================================================================
// Day 09 — Submit이 JobHandle을 반환하도록 변경
//
// 목표:
//   Submit 하나로 "핸들 필요 없음"과 "핸들 사용" 두 패턴을 모두 지원하고,
//   핸들을 이용한 단순 의존성(A → B) 패턴을 실험한다.
//
// 빌드 방법 (experiments 폴더에서):
//   g++ -std=c++17 -O2 -pthread ../src/ThreadPool.cpp Day09_SubmitReturnsHandle.cpp -I../src -o Day09
//   ./Day09
// =============================================================================

#include "ThreadPool.h"

#include <iostream>
#include <chrono>
#include <thread>
#include <vector>
#include <iomanip>
#include <atomic>

using Clock    = std::chrono::high_resolution_clock;
using Ms       = std::chrono::milliseconds;
using Duration = std::chrono::duration<double, std::milli>;

static void SleepMs(int ms) { std::this_thread::sleep_for(Ms(ms)); }

// =============================================================================
// 실험 1: 기존 코드 호환 — 반환값 무시
// =============================================================================
static void Experiment1_BackwardCompat(ThreadPool& pool)
{
    std::cout << "\n[실험 1] 기존 코드 호환 — 반환값 무시\n";
    std::cout << "---------------------------------------------\n";

    // [[nodiscard]]지만 명시적으로 void 캐스팅하면 경고 없음
    // (실제 프로젝트에서는 그냥 쓰면 컴파일러가 경고를 낼 수 있음)
    std::atomic<int> counter{ 0 };

    for (int i = 0; i < 5; ++i)
        (void)pool.Submit([&counter] { ++counter; });

    pool.WaitAll();
    std::cout << "  반환값 무시하고 Submit 5개 → counter = " << counter.load() << "\n";
    std::cout << "  기존 void Submit()처럼 동작 확인 ✅\n";
}

// =============================================================================
// 실험 2: 새 코드 패턴 — 반환값 사용
// =============================================================================
static void Experiment2_HandleUsage(ThreadPool& pool)
{
    std::cout << "\n[실험 2] 새 패턴 — auto h = pool.Submit(...)\n";
    std::cout << "---------------------------------------------\n";

    auto h1 = pool.Submit([] { SleepMs(80);  return; });
    auto h2 = pool.Submit([] { SleepMs(200); return; });
    auto h3 = pool.Submit([] { SleepMs(50);  return; });

    // 가장 빠른 h3만 먼저 기다린다
    h3.Wait();
    std::cout << "  h3 완료 (50ms 작업)\n";
    std::cout << "  h1.IsDone() = " << h1.IsDone() << "\n";
    std::cout << "  h2.IsDone() = " << h2.IsDone() << "\n";

    h1.Wait();
    std::cout << "  h1 완료 (80ms 작업)\n";

    h2.Wait();
    std::cout << "  h2 완료 (200ms 작업)\n";
}

// =============================================================================
// 실험 3: 단순 의존성 — A 완료 후 B 제출
//
// [순차 실행]
//   A 완료 → B 제출 → B 완료
//   총 시간 ≈ A + B
//
// [병렬 실행 — 의존성 없을 때]
//   A, B 동시 제출 → 병렬 완료
//   총 시간 ≈ max(A, B)
//
// A → B 의존성이 있을 때는 "A 완료 후 B 제출"이 정확한 표현이다.
// 이 패턴이 Day 11에서 만들 atomic 의존성 카운터의 기반이 된다.
// =============================================================================
static void Experiment3_SimpleDependency(ThreadPool& pool)
{
    std::cout << "\n[실험 3] 단순 의존성 A → B\n";
    std::cout << "---------------------------------------------\n";

    std::vector<std::string> log;
    std::mutex logMutex;
    auto Log = [&](const std::string& msg)
    {
        std::unique_lock<std::mutex> lock(logMutex);
        log.push_back(msg);
    };

    // ── 의존성 없음 (병렬) ───────────────────────────────────────────────────
    {
        log.clear();
        auto start = Clock::now();

        auto hA = pool.Submit([&Log] { SleepMs(100); Log("A 완료"); });
        auto hB = pool.Submit([&Log] { SleepMs(100); Log("B 완료"); });
        hA.Wait(); hB.Wait();

        Duration elapsed = Clock::now() - start;
        std::cout << "  [병렬] ";
        for (auto& s : log) std::cout << s << " → ";
        std::cout << std::fixed << std::setprecision(0) << elapsed.count() << "ms\n";
    }

    // ── A → B 의존성 (핸들로 구현) ───────────────────────────────────────────
    {
        log.clear();
        auto start = Clock::now();

        // A 완료를 기다린 뒤 B를 제출한다.
        // B는 A의 결과에 의존하는 상황을 시뮬레이션.
        auto hA = pool.Submit([&Log] { SleepMs(100); Log("A 완료"); });
        hA.Wait();   // A가 끝나야 B를 제출
        auto hB = pool.Submit([&Log] { SleepMs(100); Log("B 완료"); });
        hB.Wait();

        Duration elapsed = Clock::now() - start;
        std::cout << "  [A→B]  ";
        for (auto& s : log) std::cout << s << " → ";
        std::cout << elapsed.count() << "ms  (병렬보다 ~100ms 더 걸림)\n";
    }

    // ─────────────────────────────────────────────────────────────────────────
    // [한계점]
    //   지금 방식은 "메인 스레드가 hA.Wait()로 블록"되는 구조다.
    //   메인 스레드가 블록되지 않고 A 완료 시 B가 자동으로 큐에 올라가려면
    //   "의존성 카운터" 방식이 필요하다 → Day 11에서 구현.
    // ─────────────────────────────────────────────────────────────────────────
    std::cout << "\n  ⚠ 현재 방식의 한계:\n";
    std::cout << "     hA.Wait()로 메인 스레드가 블록됨\n";
    std::cout << "     A 완료 시 B가 자동 실행되려면 → Day 11 의존성 카운터 필요\n";
}

// =============================================================================
// 실험 4: Fan-out + Fan-in 패턴
//   A를 여러 서브태스크로 쪼개고(fan-out),
//   전부 완료 후 결과를 합산(fan-in)한다.
// =============================================================================
static void Experiment4_FanOutFanIn(ThreadPool& pool)
{
    std::cout << "\n[실험 4] Fan-out + Fan-in\n";
    std::cout << "---------------------------------------------\n";

    constexpr int kParts = 8;
    std::vector<int> partialResults(kParts, 0);
    std::vector<JobHandle> handles;
    handles.reserve(kParts);

    auto start = Clock::now();

    // Fan-out: 8개 서브태스크로 분산
    for (int i = 0; i < kParts; ++i)
    {
        handles.push_back(pool.Submit([i, &partialResults]
        {
            SleepMs(20 + i * 5);          // 작업마다 약간씩 다른 시간
            partialResults[i] = (i + 1) * 10;
        }));
    }

    // Fan-in: 모든 서브태스크 완료 대기 후 합산
    for (auto& h : handles) h.Wait();

    int total = 0;
    for (int v : partialResults) total += v;

    Duration elapsed = Clock::now() - start;
    std::cout << "  " << kParts << "개 서브태스크 병렬 처리\n";
    std::cout << "  합산 결과: " << total << " (예상: " << kParts * (kParts + 1) / 2 * 10 << ")\n";
    std::cout << "  소요 시간: " << std::fixed << std::setprecision(1)
              << elapsed.count() << " ms\n";
    std::cout << "  (순차 실행이면 "
              << 20 * kParts + 5 * kParts * (kParts - 1) / 2 << "ms였을 것)\n";
}

// =============================================================================
// main
// =============================================================================
int main()
{
    const uint32_t kThreadCount = std::thread::hardware_concurrency();
    ThreadPool pool(kThreadCount);

    std::cout << "=====================================================\n";
    std::cout << "  Day 09 — Submit → JobHandle 반환\n";
    std::cout << "=====================================================\n";
    std::cout << "  스레드 수: " << kThreadCount << "\n";

    Experiment1_BackwardCompat(pool);
    Experiment2_HandleUsage(pool);
    Experiment3_SimpleDependency(pool);
    Experiment4_FanOutFanIn(pool);

    std::cout << "\n=====================================================\n";
    std::cout << "  오늘의 핵심\n";
    std::cout << "=====================================================\n";
    std::cout << "  Submit → JobHandle : 단일 API로 두 패턴 지원\n";
    std::cout << "  [[nodiscard]]       : 핸들 무시 시 컴파일러 경고\n";
    std::cout << "  A→B 의존성 현재 방식: hA.Wait() → hB Submit (메인 블록)\n";
    std::cout << "  다음 단계 (Day 11)  : atomic 카운터로 자동 의존성 실행\n";
    std::cout << "=====================================================\n\n";

    return 0;
}