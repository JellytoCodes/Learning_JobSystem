// =============================================================================
// Day 04 — SubmitWithFuture<T> 실험
//
// 목표:
//   std::future를 활용해 반환값이 있는 작업을 ThreadPool에서 처리하는 방법과,
//   기존 배열 방식과 비교해 어떤 상황에서 future가 더 적합한지 이해한다.
//
// 빌드 방법 (experiments 폴더에서):
//   g++ -std=c++17 -O2 -pthread ../src/ThreadPool.cpp Day04_SubmitWithFuture.cpp -I../src -o Day04
//   ./Day04
// =============================================================================

#include "ThreadPool.h"

#include <iostream>
#include <vector>
#include <chrono>
#include <iomanip>
#include <stdexcept>
#include <string>

using Clock    = std::chrono::high_resolution_clock;
using Duration = std::chrono::duration<double, std::milli>;

// =============================================================================
// 실험용 작업들
// =============================================================================

// 단순 정수 계산
static int  Square(int x) { return x * x; }

// 문자열 반환
static std::string MakeLabel(int x) { return "result_" + std::to_string(x * x); }

// 의도적으로 예외를 던지는 작업
static int  MightThrow(int x)
{
    if (x < 0) throw std::invalid_argument("음수는 허용하지 않습니다.");
    return x * x;
}

// 무거운 연산 (소수 판별)
static bool IsPrime(uint64_t n)
{
    if (n < 2) return false;
    for (uint64_t i = 2; i * i <= n; ++i)
        if (n % i == 0) return false;
    return true;
}

// =============================================================================
// 실험 1: 기본 사용법 — 단일 future
// =============================================================================
static void Experiment1_BasicUsage(ThreadPool& pool)
{
    std::cout << "\n[실험 1] 기본 사용법 — 단일 future\n";
    std::cout << "---------------------------------------------\n";

    // 반환값 있는 작업 제출
    auto f1 = pool.SubmitWithFuture([] { return Square(7); });
    auto f2 = pool.SubmitWithFuture([] { return MakeLabel(5); });

    // f1.get()은 작업이 완료될 때까지 블록한다
    // 이미 완료됐다면 즉시 반환
    std::cout << "  Square(7)    = " << f1.get() << "\n";
    std::cout << "  MakeLabel(5) = " << f2.get() << "\n";
}

// =============================================================================
// 실험 2: 여러 future 수집 — vector<future>
// =============================================================================
static void Experiment2_MultipleFutures(ThreadPool& pool)
{
    std::cout << "\n[실험 2] 여러 future 수집\n";
    std::cout << "---------------------------------------------\n";

    constexpr int kCount = 8;

    // ── 방식 A: 배열 인덱스로 결과 수집 (기존 Submit 방식) ──────────────────
    std::vector<int> results_A(kCount, 0);
    for (int i = 0; i < kCount; ++i)
    {
        pool.Submit([i, &results_A] { results_A[i] = Square(i); });
    }
    pool.WaitAll();

    // ── 방식 B: future 벡터로 결과 수집 (SubmitWithFuture 방식) ─────────────
    std::vector<std::future<int>> futures;
    futures.reserve(kCount);
    for (int i = 0; i < kCount; ++i)
    {
        futures.push_back(pool.SubmitWithFuture([i] { return Square(i); }));
    }

    std::cout << "  방식 A (배열 직접 쓰기):\n  ";
    for (int v : results_A) std::cout << v << " ";
    std::cout << "\n";

    std::cout << "  방식 B (future 수집):\n  ";
    for (auto& f : futures) std::cout << f.get() << " ";
    std::cout << "\n";

    // ─────────────────────────────────────────────────────────────────────────
    // [비교 정리]
    //
    // 배열 방식:
    //   + 빠름 (future 생성/관리 오버헤드 없음)
    //   + 대량 데이터 처리에 적합
    //   - 인덱스 관리 필요 (thread-safe 접근 보장 필요)
    //   - 예외 처리 어려움
    //
    // future 방식:
    //   + 인덱스 없이 자연스러운 코드
    //   + 예외가 future를 통해 안전하게 전파됨
    //   + 특정 작업만 골라서 기다릴 수 있음
    //   - packaged_task 오버헤드 (대량 작업엔 비용)
    // ─────────────────────────────────────────────────────────────────────────
    std::cout << "\n  → 1만 개 이상 대량 작업: 배열 방식이 유리\n";
    std::cout << "  → 결과마다 다른 처리 필요: future 방식이 유리\n";
}

// =============================================================================
// 실험 3: 예외 전파 — future를 통한 안전한 예외 처리
// =============================================================================
static void Experiment3_ExceptionPropagation(ThreadPool& pool)
{
    std::cout << "\n[실험 3] 예외 전파 — future 없이 vs future 사용\n";
    std::cout << "---------------------------------------------\n";

    // future 없는 Submit에서 예외가 발생하면?
    // → 워커 스레드 안에서 예외가 터지고, 아무도 잡지 않으면
    //   std::terminate() 호출 → 프로그램 크래시
    // → 그래서 Submit 내부의 job은 예외를 던지면 안 된다 (직접 catch 필요)
    std::cout << "  [Submit] 예외 발생 시 catch해서 처리:\n";
    pool.Submit([]
    {
        try { MightThrow(-1); }
        catch (const std::exception& e)
        {
            std::cout << "  워커 스레드에서 잡은 예외: " << e.what() << "\n";
        }
    });
    pool.WaitAll();

    // future를 쓰면 예외가 future 안에 저장된다
    // get()을 호출하는 스레드(메인)에서 예외가 다시 던져짐
    std::cout << "\n  [SubmitWithFuture] 예외가 future에 저장되어 get()에서 발생:\n";
    auto f_ok  = pool.SubmitWithFuture([] { return MightThrow(5);  });
    auto f_err = pool.SubmitWithFuture([] { return MightThrow(-1); });

    std::cout << "  f_ok.get()  = " << f_ok.get() << " (정상)\n";
    try
    {
        int val = f_err.get();  // 여기서 예외가 다시 던져진다
        (void)val;
    }
    catch (const std::exception& e)
    {
        std::cout << "  f_err.get() → 예외 수신: " << e.what() << "\n";
    }
}

// =============================================================================
// 실험 4: 무거운 작업 + 비동기 결과 수집
// =============================================================================
static void Experiment4_AsyncHeavyWork(ThreadPool& pool)
{
    std::cout << "\n[실험 4] 무거운 작업 비동기 처리\n";
    std::cout << "---------------------------------------------\n";

    // 소수 판별 10개를 비동기로 제출
    const std::vector<uint64_t> candidates = {
        982451653,   // 50,000,000번째 소수
        999999937,   // 소수
        1000000007,  // 소수
        999999893,   // 소수
        999999883,   // 소수
        100000000,   // 비소수
        1000000009,  // 소수
        987654321,   // 비소수
        998244353,   // 소수
        1000000021,  // 소수
    };

    std::vector<std::future<bool>> futures;
    futures.reserve(candidates.size());

    auto start = Clock::now();

    // 전부 제출 (비동기 — 메인 스레드는 즉시 다음 줄로)
    for (uint64_t n : candidates)
        futures.push_back(pool.SubmitWithFuture([n] { return IsPrime(n); }));

    // 결과 수집
    std::cout << "  " << std::setw(14) << "숫자" << "  소수 여부\n";
    for (size_t i = 0; i < candidates.size(); ++i)
        std::cout << "  " << std::setw(14) << candidates[i]
                  << "  " << (futures[i].get() ? "✅ 소수" : "❌ 비소수") << "\n";

    Duration elapsed = Clock::now() - start;
    std::cout << "\n  소요 시간: " << std::fixed << std::setprecision(1)
              << elapsed.count() << " ms (병렬 처리)\n";
}

// =============================================================================
// main
// =============================================================================
int main()
{
    const uint32_t kThreadCount = std::thread::hardware_concurrency();
    ThreadPool pool(kThreadCount);

    std::cout << "=====================================================\n";
    std::cout << "  Day 04 — SubmitWithFuture<T>\n";
    std::cout << "=====================================================\n";
    std::cout << "  스레드 수: " << kThreadCount << "\n";

    Experiment1_BasicUsage(pool);
    Experiment2_MultipleFutures(pool);
    Experiment3_ExceptionPropagation(pool);
    Experiment4_AsyncHeavyWork(pool);

    std::cout << "\n=====================================================\n";
    std::cout << "  오늘의 핵심\n";
    std::cout << "=====================================================\n";
    std::cout << "  packaged_task  : callable을 감싸 future와 연결\n";
    std::cout << "  shared_ptr 감싸기 : move-only 타입을 function에 넣는 관용구\n";
    std::cout << "  invoke_result_t : 함수 반환 타입을 컴파일 타임에 추론\n";
    std::cout << "  f.get()         : 완료 대기 + 결과/예외 수신\n";
    std::cout << "=====================================================\n\n";

    return 0;
}