#include "ThreadPool.h"
#include <iostream>
#include <vector>
#include <chrono>
#include <numeric>
#include <iomanip>

// =============================================================================
// 벤치마크용 연산: 소수(Prime) 개수 세기
//
// 왜 소수 세기인가?
//   - 순수 CPU 연산이라 메모리 대역폭/IO 영향 없음
//   - 범위를 쪼개 각 스레드에 분배하기 쉬움
//   - 결과 검증이 가능 (싱글스레드 결과 == 멀티스레드 결과여야 함)
// =============================================================================

// [min, max) 범위에서 소수의 개수를 반환하는 함수.
// 에라토스테네스 체 대신 시험 나눗셈(trial division) 사용.
// 의도적으로 느리게 유지해 스레드 효과가 잘 드러나도록 한다.
static uint64_t CountPrimesInRange(uint64_t min, uint64_t max)
{
    uint64_t count = 0;
    for (uint64_t n = min; n < max; ++n)
    {
        if (n < 2) continue;
        bool isPrime = true;
        for (uint64_t i = 2; i * i <= n; ++i)
        {
            if (n % i == 0) { isPrime = false; break; }
        }
        if (isPrime) ++count;
    }
    return count;
}

// =============================================================================
// 벤치마크 유틸
// =============================================================================
using Clock    = std::chrono::high_resolution_clock;
using Ms       = std::chrono::milliseconds;
using Duration = std::chrono::duration<double, std::milli>;

static void PrintSeparator()  { std::cout << std::string(60, '-') << '\n'; }
static void PrintHeader(const std::string& title)
{
    PrintSeparator();
    std::cout << "  " << title << '\n';
    PrintSeparator();
}

// =============================================================================
// 테스트 1: 싱글스레드 — 기준점(Baseline)
// =============================================================================
static uint64_t RunSingleThread(uint64_t rangeEnd, uint32_t chunkCount)
{
    const uint64_t chunkSize = rangeEnd / chunkCount;
    uint64_t total = 0;

    auto start = Clock::now();

    // 작업을 chunkCount개로 나눠 순차 실행.
    // ThreadPool 없이 메인 스레드 하나가 전부 처리한다.
    for (uint32_t i = 0; i < chunkCount; ++i)
    {
        uint64_t lo = i * chunkSize;
        uint64_t hi = (i + 1 == chunkCount) ? rangeEnd : lo + chunkSize;
        total += CountPrimesInRange(lo, hi);
    }

    Duration elapsed = Clock::now() - start;
    std::cout << "  [Single Thread]  " << std::fixed << std::setprecision(1)
              << elapsed.count() << " ms  |  소수 개수: " << total << '\n';
    return total;
}

// =============================================================================
// 테스트 2: ThreadPool — 작업을 병렬 분산
// =============================================================================
static uint64_t RunThreadPool(uint64_t rangeEnd, uint32_t chunkCount, uint32_t threadCount)
{
    ThreadPool pool(threadCount);

    // 각 청크의 결과를 담을 배열. 인덱스별로 독립적인 원소에 접근하므로 락 불필요.
    std::vector<uint64_t> results(chunkCount, 0);

    const uint64_t chunkSize = rangeEnd / chunkCount;

    auto start = Clock::now();

    // chunkCount개의 작업을 Submit.
    // 각 람다는 [i, &results, ...] 캡처로 독립적인 청크를 처리.
    for (uint32_t i = 0; i < chunkCount; ++i)
    {
        uint64_t lo = static_cast<uint64_t>(i) * chunkSize;
        uint64_t hi = (i + 1 == chunkCount) ? rangeEnd : lo + chunkSize;

        // [=]으로 lo, hi를 값 복사 캡처.
        // 참조 캡처하면 루프 변수가 바뀌어 버릴 수 있다 (dangling reference).
        pool.Submit([=, &results]
        {
            results[i] = CountPrimesInRange(lo, hi);
        });
    }

    // 모든 작업이 완료될 때까지 메인 스레드를 블록.
    pool.WaitAll();

    Duration elapsed = Clock::now() - start;

    uint64_t total = std::accumulate(results.begin(), results.end(), 0ULL);
    std::cout << "  [Thread Pool x" << threadCount << "]  "
              << std::fixed << std::setprecision(1)
              << elapsed.count() << " ms  |  소수 개수: " << total << '\n';
    return total;
}

// =============================================================================
// main
// =============================================================================
int main()
{
    // -------------------------------------------------------------------------
    // 벤치마크 파라미터
    // -------------------------------------------------------------------------
    constexpr uint64_t kRangeEnd   = 2'000'000;    // 2까지 소수 탐색 범위
    constexpr uint32_t kChunkCount = 64;            // 작업 분할 단위
    const     uint32_t kCoreCount  = std::thread::hardware_concurrency();

    std::cout << "\n";
    std::cout << "  JobSystem — ThreadPool 벤치마크\n";
    std::cout << "  탐색 범위: 2 ~ " << kRangeEnd << "\n";
    std::cout << "  청크 수: " << kChunkCount << "\n";
    std::cout << "  논리 코어 수: " << kCoreCount << "\n\n";

    // -------------------------------------------------------------------------
    // 각 테스트 실행
    // -------------------------------------------------------------------------
    PrintHeader("싱글스레드 (기준점)");
    const uint64_t expected = RunSingleThread(kRangeEnd, kChunkCount);

    std::cout << '\n';
    PrintHeader("ThreadPool 병렬 실행");

    // 2, 4, 코어 수 순서로 스레드 수를 늘려가며 측정.
    // 스레드가 많다고 무조건 빠르지 않다는 것을 확인할 수 있다.
    for (uint32_t t : { 2u, 4u, kCoreCount })
    {
        if (t > kCoreCount) continue;
        const uint64_t result = RunThreadPool(kRangeEnd, kChunkCount, t);

        // 결과 검증: 싱글스레드와 동일해야 올바르게 구현된 것.
        if (result != expected)
            std::cout << "    ❌ 결과 불일치! (예상: " << expected << ", 실제: " << result << ")\n";
        else
            std::cout << "    ✅ 결과 일치\n";
    }

    // -------------------------------------------------------------------------
    // Speedup 정리 출력
    // -------------------------------------------------------------------------
    std::cout << '\n';
    PrintHeader("요약");
    std::cout << "  싱글스레드 대비 병렬 처리 속도 향상(Speedup)을 위 결과에서 직접 계산하세요.\n";
    std::cout << "  Speedup = 싱글스레드 시간 / 멀티스레드 시간\n";
    std::cout << "  이론상 최대 Speedup = 스레드 수 (Amdahl의 법칙에 따라 실제는 그보다 낮음)\n";
    PrintSeparator();
    std::cout << '\n';

    return 0;
}