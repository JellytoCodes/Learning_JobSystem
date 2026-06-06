// =============================================================================
// Day 13 — Failure Policy: Completion vs Success
//
// 목표:
//   Day 12에서 남겨둔 "선행 작업 실패 시 후속 작업을 어떻게 할 것인가"를
//   두 가지 정책으로 비교한다.
//
// 정책 1: SubmitAfter
//   선행 작업의 성공/실패와 무관하게 완료만 확인하고 후속 작업을 실행한다.
//
// 정책 2: SubmitAfterAllSucceeded
//   선행 작업이 모두 성공했을 때만 후속 작업을 실행한다.
//   하나라도 실패하면 후속 작업은 실행되지 않고 JobCanceledException으로 완료된다.
//
// 빌드 방법 (experiments 폴더에서):
//   g++ -std=c++17 -O2 -pthread ../src/ThreadPool.cpp Day13_FailurePolicy.cpp -I../src -o Day13
//   ./Day13
// =============================================================================

#include "ThreadPool.h"

#include <atomic>
#include <chrono>
#include <exception>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using Ms = std::chrono::milliseconds;

static void SleepMs(int ms) { std::this_thread::sleep_for(Ms(ms)); }

static void PrintLog(const std::vector<std::string>& log)
{
    for (const auto& line : log)
        std::cout << "  " << line << "\n";
}

// =============================================================================
// 실험 1: 완료 기반 정책은 실패한 선행 작업 뒤에도 후속 작업을 실행한다
// =============================================================================
static void Experiment1_CompletionPolicy()
{
    std::cout << "\n[실험 1] SubmitAfter — 완료 기반 정책\n";
    std::cout << "---------------------------------------------\n";

    ThreadPool pool(2);
    std::vector<std::string> log;
    std::mutex logMutex;

    auto Log = [&](const std::string& text)
    {
        std::unique_lock<std::mutex> lock(logMutex);
        log.push_back(text);
    };

    auto failed = pool.Submit([&Log]
    {
        Log("A 시작");
        throw std::runtime_error("A 실패");
    });

    auto next = pool.SubmitAfter({ failed }, [&Log]
    {
        Log("B 실행: A의 성공 여부와 무관하게 완료 후 실행");
    });

    next.Wait();

    try
    {
        failed.Wait();
    }
    catch (const std::exception& e)
    {
        Log(std::string("A 예외 확인: ") + e.what());
    }

    PrintLog(log);
    std::cout << "  해석: 완료 기반 정책은 cleanup, logging, fallback 제출에 적합\n";
}

// =============================================================================
// 실험 2: 성공 기반 정책은 실패한 선행 작업 뒤의 후속 작업을 취소한다
// =============================================================================
static void Experiment2_SuccessPolicyCancels()
{
    std::cout << "\n[실험 2] SubmitAfterAllSucceeded — 성공 기반 정책\n";
    std::cout << "---------------------------------------------\n";

    ThreadPool pool(2);
    std::atomic<bool> nextRan{ false };

    auto failed = pool.Submit([]
    {
        SleepMs(20);
        throw std::runtime_error("선행 작업 실패");
    });

    auto next = pool.SubmitAfterAllSucceeded({ failed }, [&nextRan]
    {
        nextRan = true;
    });

    try
    {
        next.Wait();
        std::cout << "  후속 작업 완료 (오류)\n";
    }
    catch (const JobCanceledException& e)
    {
        std::cout << "  후속 작업 취소 확인: " << e.what() << "\n";
    }

    try
    {
        failed.Wait();
    }
    catch (const std::exception& e)
    {
        std::cout << "  선행 작업 예외 확인: " << e.what() << "\n";
    }

    std::cout << "  nextRan = " << std::boolalpha << nextRan.load() << "\n";
    std::cout << "  해석: 성공 기반 정책은 실패한 입력을 쓰면 안 되는 후속 계산에 적합\n";
}

// =============================================================================
// 실험 3: 여러 선행 작업 중 하나만 실패해도 Fan-in을 취소한다
// =============================================================================
static void Experiment3_FanInCancelOnAnyFailure()
{
    std::cout << "\n[실험 3] Fan-in — 하나라도 실패하면 최종 작업 취소\n";
    std::cout << "---------------------------------------------\n";

    ThreadPool pool(4);
    std::vector<JobHandle> dependencies;
    dependencies.reserve(4);

    std::atomic<int> completedParts{ 0 };
    std::atomic<bool> finalRan{ false };

    for (int i = 0; i < 4; ++i)
    {
        dependencies.push_back(pool.Submit([i, &completedParts]
        {
            SleepMs(20 + i * 10);
            if (i == 2)
                throw std::runtime_error("Part 2 실패");
            ++completedParts;
        }));
    }

    auto final = pool.SubmitAfterAllSucceeded(dependencies, [&finalRan]
    {
        finalRan = true;
    });

    try
    {
        final.Wait();
        std::cout << "  Final 실행됨 (오류)\n";
    }
    catch (const JobCanceledException& e)
    {
        std::cout << "  Final 취소 확인: " << e.what() << "\n";
    }

    for (auto& dependency : dependencies)
    {
        try
        {
            dependency.Wait();
        }
        catch (const std::exception&)
        {
            // 실패한 선행 작업은 위 정책 확인에 이미 반영됐으므로 여기서는 삼킨다.
        }
    }

    std::cout << "  성공한 Part 수: " << completedParts.load() << " / 4\n";
    std::cout << "  finalRan = " << std::boolalpha << finalRan.load() << "\n";
    std::cout << "  핵심: dependency counter 위에 실패 정책을 분리해서 얹을 수 있음\n";
}

// =============================================================================
// main
// =============================================================================
int main()
{
    std::cout << "=====================================================\n";
    std::cout << "  Day 13 — Failure Policy: Completion vs Success\n";
    std::cout << "=====================================================\n";

    Experiment1_CompletionPolicy();
    Experiment2_SuccessPolicyCancels();
    Experiment3_FanInCancelOnAnyFailure();

    std::cout << "\n=====================================================\n";
    std::cout << "  오늘의 핵심\n";
    std::cout << "=====================================================\n";
    std::cout << "  SubmitAfter              : 완료 기반 continuation\n";
    std::cout << "  SubmitAfterAllSucceeded  : 성공 기반 continuation\n";
    std::cout << "  JobCanceledException     : 실행되지 않은 후속 작업의 상태 표현\n";
    std::cout << "  실패 정책                : JobSystem 코어 위에 별도 계층으로 설계 가능\n";
    std::cout << "=====================================================\n\n";

    return 0;
}
