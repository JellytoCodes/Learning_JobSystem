// =============================================================================
// Day 03 — 청크 수(ChunkCount)가 Speedup에 미치는 영향
//
// 목표:
//   같은 작업량을 몇 개의 청크로 나누느냐에 따라
//   병렬 효율이 어떻게 달라지는지 수치로 확인한다.
//
// 핵심 개념:
//   1. 로드 밸런싱 (Load Balancing)
//      청크 수 < 스레드 수 → 일부 스레드가 놀아서 비효율
//      청크 수 >> 스레드 수 → 모든 스레드가 고르게 일함
//
//   2. 오버헤드 (Overhead)
//      청크가 너무 작으면 Submit/mutex/queue 비용이 작업 자체보다 커짐
//      → "너무 잘게 쪼개도 느려진다"
//
//   3. 스윗 스팟 (Sweet Spot)
//      일반적으로 청크 수 = 스레드 수 × 4~8 이 좋은 출발점
//
// 빌드 방법 (experiments 폴더에서):
//   g++ -std=c++17 -O2 -pthread ../src/ThreadPool.cpp Day03_ChunkCount.cpp -I../src -o Day03
//   ./Day03
// =============================================================================

#include "ThreadPool.h"

#include <iostream>
#include <vector>
#include <chrono>
#include <iomanip>
#include <numeric>
#include <cmath>

using Clock    = std::chrono::high_resolution_clock;
using Duration = std::chrono::duration<double, std::milli>;

// =============================================================================
// 소수 세기 (Day 01 벤치마크와 동일한 작업)
// =============================================================================
static uint64_t CountPrimesInRange(uint64_t min, uint64_t max)
{
    uint64_t count = 0;
    for (uint64_t n = min; n < max; ++n)
    {
        if (n < 2) continue;
        bool isPrime = true;
        for (uint64_t i = 2; i * i <= n; ++i)
            if (n % i == 0) { isPrime = false; break; }
        if (isPrime) ++count;
    }
    return count;
}

// =============================================================================
// 싱글스레드 기준 시간 측정
// =============================================================================
static double MeasureSingleThread(uint64_t rangeEnd)
{
    auto start = Clock::now();
    // volatile: 컴파일러가 "결과를 안 쓰니까 연산 자체를 날려버리는" 최적화를 막는다.
    volatile uint64_t result = CountPrimesInRange(2, rangeEnd);
    (void)result;
    return Duration(Clock::now() - start).count();
}

// =============================================================================
// ThreadPool 기준 시간 측정 (청크 수 가변)
// =============================================================================
static double MeasureThreadPool(uint64_t rangeEnd, uint32_t chunkCount, uint32_t threadCount)
{
    ThreadPool pool(threadCount);
    std::vector<uint64_t> results(chunkCount, 0);
    const uint64_t chunkSize = rangeEnd / chunkCount;

    auto start = Clock::now();

    for (uint32_t i = 0; i < chunkCount; ++i)
    {
        uint64_t lo = static_cast<uint64_t>(i) * chunkSize;
        uint64_t hi = (i + 1 == chunkCount) ? rangeEnd : lo + chunkSize;
        pool.Submit([=, &results] { results[i] = CountPrimesInRange(lo, hi); });
    }
    pool.WaitAll();

    return Duration(Clock::now() - start).count();
}

// =============================================================================
// 결과 출력 헬퍼
// =============================================================================
static void PrintTableHeader(uint32_t threadCount)
{
    std::cout << "\n";
    std::cout << "  스레드 수: " << threadCount << "\n\n";
    std::cout << "  "
              << std::setw(10) << "청크 수"
              << std::setw(12) << "시간(ms)"
              << std::setw(12) << "Speedup"
              << std::setw(12) << "효율(%)"
              << "  진단\n";
    std::cout << "  " << std::string(56, '-') << "\n";
}

static void PrintRow(uint32_t chunkCount, double elapsedMs,
                     double singleMs, uint32_t threadCount)
{
    const double speedup    = singleMs / elapsedMs;
    const double efficiency = speedup / threadCount * 100.0;

    // 효율 진단
    std::string diagnosis;
    if (chunkCount < threadCount)
        diagnosis = "← 청크 부족 (스레드 놀음)";
    else if (efficiency > 85.0)
        diagnosis = "← ✅ 스윗 스팟";
    else if (efficiency > 60.0)
        diagnosis = "← 양호";
    else
        diagnosis = "← 오버헤드 증가";

    std::cout << "  "
              << std::setw(10) << chunkCount
              << std::setw(12) << std::fixed << std::setprecision(1) << elapsedMs
              << std::setw(12) << std::fixed << std::setprecision(2) << speedup
              << std::setw(12) << std::fixed << std::setprecision(1) << efficiency
              << "  " << diagnosis << "\n";
}

// =============================================================================
// main
// =============================================================================
int main()
{
    constexpr uint64_t kRangeEnd   = 2'000'000;
    const uint32_t     kThreadCount = std::thread::hardware_concurrency();

    std::cout << "=====================================================\n";
    std::cout << "  Day 03 — 청크 수(ChunkCount) vs Speedup\n";
    std::cout << "=====================================================\n";
    std::cout << "  탐색 범위  : 2 ~ " << kRangeEnd << "\n";
    std::cout << "  논리 코어  : " << kThreadCount << "\n";

    // ─────────────────────────────────────────────────────────────────────────
    // 싱글스레드 기준 시간 (3회 평균)
    // ─────────────────────────────────────────────────────────────────────────
    std::cout << "\n  싱글스레드 기준 측정 중...\n";
    double singleMs = 0.0;
    constexpr int kWarmup = 3;
    for (int i = 0; i < kWarmup; ++i)
        singleMs += MeasureSingleThread(kRangeEnd);
    singleMs /= kWarmup;
    std::cout << "  싱글스레드 평균: " << std::fixed << std::setprecision(1)
              << singleMs << " ms\n";

    // ─────────────────────────────────────────────────────────────────────────
    // 청크 수 변화 실험
    //
    // 실험 구간:
    //   A. 청크 < 스레드  → 로드 밸런싱 실패 구간
    //   B. 청크 = 스레드  → 이론상 완벽한 분배, 실제로는?
    //   C. 청크 > 스레드  → 오버스플릿, 스윗 스팟 구간
    //   D. 청크 >> 스레드 → 오버헤드 역전 구간
    // ─────────────────────────────────────────────────────────────────────────
    const std::vector<uint32_t> chunkCounts = {
        1,                           // A: 극단적으로 부족 (병렬성 없음)
        kThreadCount / 2,            // A: 스레드 절반만 활용
        kThreadCount,                // B: 청크 = 스레드 (이론 최적)
        kThreadCount * 2,            // C: 2배
        kThreadCount * 4,            // C: 4배 (일반적 스윗 스팟)
        kThreadCount * 8,            // C: 8배
        kThreadCount * 16,           // D: 오버헤드 시작 구간
        kThreadCount * 32,           // D: 오버헤드 증가
        512,                         // 고정값 비교
        1024,                        // 고정값 비교
    };

    PrintTableHeader(kThreadCount);

    for (uint32_t chunks : chunkCounts)
    {
        if (chunks == 0) chunks = 1;
        const double elapsed = MeasureThreadPool(kRangeEnd, chunks, kThreadCount);
        PrintRow(chunks, elapsed, singleMs, kThreadCount);
    }

    // ─────────────────────────────────────────────────────────────────────────
    // [관찰 포인트]
    //
    // 1. 청크 1개: Speedup ≈ 1.0
    //    작업이 나뉘지 않아서 스레드 1개만 일한다.
    //    ThreadPool 오버헤드가 더해져서 싱글보다 오히려 느릴 수 있다.
    //
    // 2. 청크 = 스레드 수: 이론상 최적처럼 보이지만 실제로는 부족할 때가 많다.
    //    모든 청크의 작업량이 완벽히 같아야 하는데, 소수 계산처럼
    //    구간마다 무게가 다른 작업은 한 스레드가 늦게 끝나면 전체가 기다린다.
    //    이 현상을 "Load Imbalance" 또는 "Tail Latency" 라고 부른다.
    //
    // 3. 청크 = 스레드 × 4~8: 각 스레드가 여러 청크를 나눠 처리.
    //    한 청크가 늦게 끝나도 다른 청크를 바로 이어받아 처리 → 놀지 않음.
    //    이것이 "동적 로드 밸런싱"의 핵심 아이디어다.
    //
    // 4. 청크가 너무 많으면: Submit + mutex + queue 비용이 쌓여서 역효과.
    //    작업 자체보다 관리 비용이 커지는 구간이 반드시 존재한다.
    // ─────────────────────────────────────────────────────────────────────────

    std::cout << "\n=====================================================\n";
    std::cout << "  핵심 정리\n";
    std::cout << "=====================================================\n";
    std::cout << "  청크 < 스레드  : 일부 스레드가 놀아서 Speedup 낮음\n";
    std::cout << "  청크 = 스레드  : 이론 최적이지만 Load Imbalance 발생 가능\n";
    std::cout << "  청크 = N × 스레드 (N=4~8) : 동적 밸런싱, 실전 스윗 스팟\n";
    std::cout << "  청크 >> 스레드 : Submit/Queue 오버헤드가 역전\n\n";
    std::cout << "  → ThreadPool의 인터페이스가 청크 수를 노출하는 이유:\n";
    std::cout << "    호출자가 작업 특성에 맞게 조절할 수 있어야 하기 때문.\n";
    std::cout << "    Week 4에서 만들 parallel_for가 이걸 자동으로 계산해준다.\n";
    std::cout << "=====================================================\n\n";

    return 0;
}