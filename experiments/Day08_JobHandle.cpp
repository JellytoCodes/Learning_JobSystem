// =============================================================================
// Day 08 — JobHandle 설계 및 사용법
//
// 목표:
//   특정 작업 하나의 완료를 선택적으로 기다리는 JobHandle을 이해하고,
//   WaitAll과의 차이를 실험으로 확인한다.
//
// 빌드 방법 (experiments 폴더에서):
//   g++ -std=c++17 -O2 -pthread ../src/ThreadPool.cpp Day08_JobHandle.cpp -I../src -o Day08
//   ./Day08
// =============================================================================

#include "ThreadPool.h"

#include <iostream>
#include <chrono>
#include <thread>
#include <vector>
#include <iomanip>

using Clock    = std::chrono::high_resolution_clock;
using Ms       = std::chrono::milliseconds;
using Duration = std::chrono::duration<double, std::milli>;

static void SleepMs(int ms) { std::this_thread::sleep_for(Ms(ms)); }

// =============================================================================
// 실험 1: 기본 사용법 — Wait() / IsDone()
// =============================================================================
static void Experiment1_BasicUsage(ThreadPool& pool)
{
    std::cout << "\n[실험 1] 기본 사용법\n";
    std::cout << "---------------------------------------------\n";

    // default 생성 핸들 — 유효하지 않음
    JobHandle empty;
    std::cout << "  empty.IsValid() = " << std::boolalpha << empty.IsValid() << "\n";
    std::cout << "  empty.IsDone()  = " << empty.IsDone()  << "\n";
    empty.Wait();   // 즉시 반환 (유효하지 않으므로)
    std::cout << "  empty.Wait()    = 즉시 반환 ✅\n\n";

    // 실제 작업 제출
    std::atomic<bool> ran{ false };
    auto handle = pool.SubmitWithHandle([&ran]
    {
        SleepMs(100);   // 100ms 걸리는 작업
        ran = true;
    });

    std::cout << "  제출 직후 IsDone() = " << handle.IsDone() << "  (아직 실행 중)\n";
    std::cout << "  Wait() 호출 중...\n";

    auto start = Clock::now();
    handle.Wait();   // 완료될 때까지 블록
    Duration elapsed = Clock::now() - start;

    std::cout << "  Wait() 반환 후 IsDone() = " << handle.IsDone() << "\n";
    std::cout << "  ran = " << ran.load() << "\n";
    std::cout << "  대기 시간: " << std::fixed << std::setprecision(1)
              << elapsed.count() << " ms (작업이 100ms였으므로 비슷해야 함)\n";
}

// =============================================================================
// 실험 2: WaitAll vs JobHandle — 선택적 대기
//
// WaitAll: 느린 작업(500ms)과 빠른 작업(50ms)이 섞여 있으면
//          빠른 작업이 끝나도 느린 작업까지 기다려야 함.
//
// JobHandle: 빠른 작업 핸들만 골라서 기다리고 먼저 처리 가능.
// =============================================================================
static void Experiment2_SelectiveWait(ThreadPool& pool)
{
    std::cout << "\n[실험 2] 선택적 대기 — WaitAll vs JobHandle\n";
    std::cout << "---------------------------------------------\n";

    // ── WaitAll 방식 ──────────────────────────────────────────────────────────
    {
        std::cout << "  [WaitAll 방식]\n";
        auto start = Clock::now();

        pool.Submit([] { SleepMs(500); });   // 느린 작업
        pool.Submit([] { SleepMs(50);  });   // 빠른 작업

        pool.WaitAll();   // 500ms짜리가 끝날 때까지 기다림

        Duration elapsed = Clock::now() - start;
        std::cout << "  빠른 작업 결과를 쓸 수 있는 시점: "
                  << elapsed.count() << " ms (500ms 기다린 뒤)\n\n";
    }

    // ── JobHandle 방식 ────────────────────────────────────────────────────────
    {
        std::cout << "  [JobHandle 방식]\n";
        auto start = Clock::now();

        auto slowHandle = pool.SubmitWithHandle([] { SleepMs(500); });
        auto fastHandle = pool.SubmitWithHandle([] { SleepMs(50);  });

        fastHandle.Wait();   // 빠른 작업만 기다림
        Duration fastElapsed = Clock::now() - start;
        std::cout << "  빠른 작업 완료 시점: " << fastElapsed.count() << " ms\n";
        std::cout << "  ↑ 이 시점에 빠른 작업 결과를 처리 가능 (느린 작업은 아직 실행 중)\n";

        slowHandle.Wait();   // 이제 느린 작업도 기다림
        Duration slowElapsed = Clock::now() - start;
        std::cout << "  느린 작업 완료 시점: " << slowElapsed.count() << " ms\n";
    }
}

// =============================================================================
// 실험 3: IsDone() 폴링 — 논블로킹 진행 상황 확인
// =============================================================================
static void Experiment3_IsDonePolling(ThreadPool& pool)
{
    std::cout << "\n[실험 3] IsDone() 논블로킹 폴링\n";
    std::cout << "---------------------------------------------\n";

    auto handle = pool.SubmitWithHandle([] { SleepMs(300); });

    std::cout << "  작업 실행 중, 50ms마다 IsDone() 확인:\n";
    int checkCount = 0;
    while (!handle.IsDone())
    {
        ++checkCount;
        std::cout << "  [" << checkCount * 50 << "ms] IsDone() = false, 다른 작업 수행 가능\n";
        SleepMs(50);
    }
    std::cout << "  IsDone() = true — 완료!\n";
    std::cout << "  총 " << checkCount << "번 확인 후 완료\n";
}

// =============================================================================
// 실험 4: 핸들 복사 — 여러 곳에서 같은 작업 대기
// =============================================================================
static void Experiment4_HandleCopy(ThreadPool& pool)
{
    std::cout << "\n[실험 4] 핸들 복사 — 여러 스레드에서 같은 작업 대기\n";
    std::cout << "---------------------------------------------\n";

    auto handle1 = pool.SubmitWithHandle([] { SleepMs(200); });
    auto handle2 = handle1;   // 복사 — 같은 JobState를 공유

    std::cout << "  handle1과 handle2는 같은 작업을 가리킴\n";
    std::cout << "  handle1.IsDone() = " << handle1.IsDone() << "\n";
    std::cout << "  handle2.IsDone() = " << handle2.IsDone() << "\n\n";

    // 두 스레드가 같은 작업을 각자 기다림
    std::thread waiter1([&handle1] { handle1.Wait(); std::cout << "  waiter1: 완료 확인\n"; });
    std::thread waiter2([&handle2] { handle2.Wait(); std::cout << "  waiter2: 완료 확인\n"; });

    waiter1.join();
    waiter2.join();

    std::cout << "  handle1.IsDone() = " << handle1.IsDone() << "\n";
    std::cout << "  handle2.IsDone() = " << handle2.IsDone() << "\n";
    std::cout << "  shared_ptr 덕분에 둘 다 동일한 완료 상태를 공유\n";
}

// =============================================================================
// main
// =============================================================================
int main()
{
    const uint32_t kThreadCount = std::thread::hardware_concurrency();
    ThreadPool pool(kThreadCount);

    std::cout << "=====================================================\n";
    std::cout << "  Day 08 — JobHandle\n";
    std::cout << "=====================================================\n";
    std::cout << "  스레드 수: " << kThreadCount << "\n";

    Experiment1_BasicUsage(pool);
    Experiment2_SelectiveWait(pool);
    Experiment3_IsDonePolling(pool);
    Experiment4_HandleCopy(pool);

    std::cout << "\n=====================================================\n";
    std::cout << "  오늘의 핵심\n";
    std::cout << "=====================================================\n";
    std::cout << "  JobHandle   : 특정 작업 하나만 골라 Wait/IsDone\n";
    std::cout << "  WaitAll     : 풀의 모든 작업 완료 대기\n";
    std::cout << "  shared_ptr  : 핸들 복사 시 같은 JobState 공유\n";
    std::cout << "  memory_order_acquire/release : done 플래그 가시성 보장\n";
    std::cout << "=====================================================\n\n";

    return 0;
}