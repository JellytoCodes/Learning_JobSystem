// =============================================================================
// Day 05 — 스레드별 처리 작업 수 분포 확인
//
// 목표:
//   어느 스레드가 얼마나 일했는지 수치로 보고,
//   로드 밸런싱이 잘 되는 조건과 그렇지 않은 조건의 차이를 확인한다.
//
// 빌드 방법 (experiments 폴더에서):
//   g++ -std=c++17 -O2 -pthread ../src/ThreadPool.cpp Day05_PerThreadStats.cpp -I../src -o Day05
//   ./Day05
// =============================================================================

#include "ThreadPool.h"

#include <iostream>
#include <vector>
#include <chrono>
#include <iomanip>
#include <algorithm>
#include <numeric>
#include <string>
#include <thread>

using Clock    = std::chrono::high_resolution_clock;
using Duration = std::chrono::duration<double, std::milli>;

// =============================================================================
// 통계 출력 헬퍼
// =============================================================================
static void PrintDistribution(const std::vector<ThreadPool::ThreadStats>& stats,
                               const std::string& label)
{
    const uint64_t total = std::accumulate(stats.begin(), stats.end(), 0ULL,
        [](uint64_t sum, const ThreadPool::ThreadStats& s) { return sum + s.jobsProcessed; });

    const uint64_t maxJobs = std::max_element(stats.begin(), stats.end(),
        [](const ThreadPool::ThreadStats& a, const ThreadPool::ThreadStats& b)
        { return a.jobsProcessed < b.jobsProcessed; })->jobsProcessed;

    const uint64_t minJobs = std::min_element(stats.begin(), stats.end(),
        [](const ThreadPool::ThreadStats& a, const ThreadPool::ThreadStats& b)
        { return a.jobsProcessed < b.jobsProcessed; })->jobsProcessed;

    const double avg = static_cast<double>(total) / stats.size();

    std::cout << "\n  [" << label << "]\n";
    std::cout << "  총 처리: " << total << "개  |  평균: "
              << std::fixed << std::setprecision(1) << avg
              << "  |  최대: " << maxJobs
              << "  |  최소: " << minJobs << "\n\n";

    // 막대 그래프로 시각화
    constexpr int kBarWidth = 30;
    for (const auto& s : stats)
    {
        const int barLen = (maxJobs > 0)
            ? static_cast<int>(static_cast<double>(s.jobsProcessed) / maxJobs * kBarWidth)
            : 0;

        std::cout << "  Thread " << std::setw(2) << s.threadIdx << "  ["
                  << std::string(barLen, '=')
                  << std::string(kBarWidth - barLen, ' ')
                  << "] " << std::setw(6) << s.jobsProcessed << " jobs\n";
    }

    // 불균형 지표: (최대 - 최소) / 평균 × 100
    const double imbalance = (avg > 0) ? (maxJobs - minJobs) / avg * 100.0 : 0.0;
    std::cout << "\n  불균형 지표: " << std::fixed << std::setprecision(1)
              << imbalance << "%";
    if (imbalance < 10.0)       std::cout << "  ← ✅ 균형 양호";
    else if (imbalance < 30.0)  std::cout << "  ← ⚠ 약간 불균형";
    else                        std::cout << "  ← ❌ 심각한 불균형";
    std::cout << "\n";
}

// =============================================================================
// 작업 함수들
// =============================================================================

// 균일한 작업 — 모든 청크가 비슷한 시간 소요
static uint64_t UniformWork(uint64_t n)
{
    uint64_t sum = 0;
    for (uint64_t i = 0; i < n; ++i) sum += i;
    return sum;
}

// 불균일한 작업 — 뒤로 갈수록 오래 걸림 (소수 판별)
static bool IsPrime(uint64_t n)
{
    if (n < 2) return false;
    for (uint64_t i = 2; i * i <= n; ++i)
        if (n % i == 0) return false;
    return true;
}

static uint64_t CountPrimesInRange(uint64_t lo, uint64_t hi)
{
    uint64_t count = 0;
    for (uint64_t n = lo; n < hi; ++n)
        if (IsPrime(n)) ++count;
    return count;
}

// =============================================================================
// 실험 1: 균일한 작업 — 스레드 수만큼만 청크 분할
//   청크 = 스레드 수 → 각 스레드가 정확히 1청크씩
//   작업량이 균일하므로 분포가 완벽해야 한다
// =============================================================================
static void Experiment1_UniformChunksEqualToThreads(ThreadPool& pool)
{
    std::cout << "\n===================================================";
    std::cout << "\n  실험 1: 균일한 작업, 청크 = 스레드 수";
    std::cout << "\n===================================================";

    pool.ResetStats();

    const uint32_t threadCount = pool.GetThreadCount();
    const uint64_t workPerChunk = 10'000'000;

    for (uint32_t i = 0; i < threadCount; ++i)
        pool.Submit([=]{ volatile auto r = UniformWork(workPerChunk); (void)r; });

    pool.WaitAll();

    PrintDistribution(pool.GetPerThreadStats(),
        "청크=" + std::to_string(threadCount) + " / 스레드=" + std::to_string(threadCount));

    // ─────────────────────────────────────────────────────────────────────────
    // [예상 결과] 모든 스레드가 정확히 1개씩 처리 → 불균형 0%
    // [실제 관찰] OS 스케줄링에 따라 약간의 차이가 생길 수 있음
    // ─────────────────────────────────────────────────────────────────────────
}

// =============================================================================
// 실험 2: 불균일한 작업 — 청크가 뒤로 갈수록 무거움
//   소수 판별은 숫자가 클수록 오래 걸린다.
//   청크를 앞부터 배분하면 앞 청크 담당 스레드는 금방 끝나고 논다.
// =============================================================================
static void Experiment2_NonUniformWork(ThreadPool& pool)
{
    std::cout << "\n===================================================";
    std::cout << "\n  실험 2: 불균일한 작업, 청크 = 스레드 수";
    std::cout << "\n===================================================";

    pool.ResetStats();

    const uint32_t threadCount = pool.GetThreadCount();
    constexpr uint64_t kTotal = 1'000'000;
    const uint64_t chunkSize  = kTotal / threadCount;

    // 불균일: 앞 청크는 작은 수(빠름), 뒤 청크는 큰 수(느림)
    for (uint32_t i = 0; i < threadCount; ++i)
    {
        uint64_t lo = i * chunkSize;
        uint64_t hi = (i + 1 == threadCount) ? kTotal : lo + chunkSize;
        pool.Submit([=]{ volatile auto r = CountPrimesInRange(lo, hi); (void)r; });
    }

    pool.WaitAll();

    PrintDistribution(pool.GetPerThreadStats(),
        "불균일 작업 / 청크=" + std::to_string(threadCount));

    // ─────────────────────────────────────────────────────────────────────────
    // [예상 결과] 앞 스레드들은 일찍 끝나 1개 처리, 뒤 스레드는 1개 처리
    //             작업량 자체가 다르므로 불균형이 보이지 않을 수 있음
    //             청크 수를 늘렸을 때와 비교하면 차이가 명확해짐
    // ─────────────────────────────────────────────────────────────────────────
}

// =============================================================================
// 실험 3: 불균일한 작업 — 청크를 충분히 잘게 나눔 (Day 03 교훈 적용)
//   청크 = 스레드 × 8 → 동적 로드 밸런싱 효과 확인
// =============================================================================
static void Experiment3_NonUniformWorkMoreChunks(ThreadPool& pool)
{
    std::cout << "\n===================================================";
    std::cout << "\n  실험 3: 불균일한 작업, 청크 = 스레드 × 8";
    std::cout << "\n===================================================";

    pool.ResetStats();

    const uint32_t threadCount = pool.GetThreadCount();
    const uint32_t chunkCount  = threadCount * 8;
    constexpr uint64_t kTotal  = 1'000'000;
    const uint64_t chunkSize   = kTotal / chunkCount;

    for (uint32_t i = 0; i < chunkCount; ++i)
    {
        uint64_t lo = static_cast<uint64_t>(i) * chunkSize;
        uint64_t hi = (i + 1 == chunkCount) ? kTotal : lo + chunkSize;
        pool.Submit([=]{ volatile auto r = CountPrimesInRange(lo, hi); (void)r; });
    }

    pool.WaitAll();

    PrintDistribution(pool.GetPerThreadStats(),
        "불균일 작업 / 청크=" + std::to_string(chunkCount)
        + " (스레드×8)");

    // ─────────────────────────────────────────────────────────────────────────
    // [예상 결과] 앞 스레드들이 빠른 청크를 처리한 뒤 느린 청크도 가져옴
    //             결과적으로 모든 스레드가 여러 청크를 처리하며 균형 개선
    // ─────────────────────────────────────────────────────────────────────────
    std::cout << "\n  → 실험 2 vs 3 불균형 지표를 비교하면\n";
    std::cout << "    \"청크를 잘게 나눌수록 균형이 잡힌다\"는 Day 03 결론이 보인다.\n";
}

// =============================================================================
// main
// =============================================================================
int main()
{
    const uint32_t kThreadCount = std::min(8u, std::thread::hardware_concurrency());
    ThreadPool pool(kThreadCount);

    std::cout << "=====================================================\n";
    std::cout << "  Day 05 — 스레드별 작업 분포 시각화\n";
    std::cout << "=====================================================\n";
    std::cout << "  워커 스레드 수: " << kThreadCount << "\n";

    Experiment1_UniformChunksEqualToThreads(pool);
    Experiment2_NonUniformWork(pool);
    Experiment3_NonUniformWorkMoreChunks(pool);

    std::cout << "\n=====================================================\n";
    std::cout << "  오늘의 핵심\n";
    std::cout << "=====================================================\n";
    std::cout << "  vector<atomic<T>> resize()로 초기화 (push_back 불가)\n";
    std::cout << "  ++_perThreadJobCount[threadIdx] — 자기 인덱스만 접근, 락 불필요\n";
    std::cout << "  불균형 지표 = (최대-최소) / 평균 × 100\n";
    std::cout << "  청크를 잘게 나눌수록 불균형 개선 (Day 03 연결)\n";
    std::cout << "=====================================================\n\n";

    return 0;
}