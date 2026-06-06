// =============================================================================
// Day 12 — Exception Propagation / Worker Survival
//
// 목표:
//   Submit으로 제출한 작업이 예외를 던져도 워커 스레드가 죽지 않게 만들고,
//   예외를 JobHandle에 저장했다가 Wait()에서 호출자에게 다시 던진다.
//
// 핵심 아이디어:
//   작업 래퍼가 try/catch로 job()을 감싸고,
//   std::current_exception()을 JobState에 저장한다.
//   작업은 "성공/실패와 무관하게 완료"되므로 done=true와 continuation은 항상 실행된다.
//
// 빌드 방법 (experiments 폴더에서):
//   g++ -std=c++17 -O2 -pthread ../src/ThreadPool.cpp Day12_ExceptionPropagation.cpp -I../src -o Day12
//   ./Day12
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
// 실험 1: Submit 작업 예외를 JobHandle::Wait()에서 재전파
// =============================================================================
static void Experiment1_WaitRethrows()
{
    std::cout << "\n[실험 1] Wait()에서 작업 예외 재전파\n";
    std::cout << "---------------------------------------------\n";

    ThreadPool pool(2);

    auto handle = pool.Submit([]
    {
        SleepMs(30);
        throw std::runtime_error("작업 내부 실패");
    });

    try
    {
        handle.Wait();
        std::cout << "  예외 없음 (오류)\n";
    }
    catch (const std::exception& e)
    {
        std::cout << "  catch 성공: " << e.what() << "\n";
    }

    std::cout << "  IsDone()      = " << std::boolalpha << handle.IsDone() << "\n";
    std::cout << "  HasException()= " << handle.HasException() << "\n";
}

// =============================================================================
// 실험 2: 예외가 발생해도 워커 스레드는 계속 다음 작업을 처리
// =============================================================================
static void Experiment2_WorkerSurvives()
{
    std::cout << "\n[실험 2] 예외 이후에도 워커 생존\n";
    std::cout << "---------------------------------------------\n";

    ThreadPool pool(2);
    std::atomic<int> completed{ 0 };

    auto bad = pool.Submit([]
    {
        throw std::runtime_error("의도된 실패");
    });

    std::vector<JobHandle> goodJobs;
    for (int i = 0; i < 5; ++i)
    {
        goodJobs.push_back(pool.Submit([&completed]
        {
            SleepMs(10);
            ++completed;
        }));
    }

    for (auto& handle : goodJobs)
        handle.Wait();

    try
    {
        bad.Wait();
    }
    catch (const std::exception& e)
    {
        std::cout << "  실패 작업 예외 확인: " << e.what() << "\n";
    }

    std::cout << "  이후 정상 작업 완료 수: " << completed.load() << " / 5\n";
    std::cout << "  핵심: 예외가 WorkerLoop 밖으로 새지 않아 워커가 계속 살아 있음\n";
}

// =============================================================================
// 실험 3: SubmitAfter는 실패도 "완료"로 보고 continuation을 실행
//
// dependency counter는 성공 여부가 아니라 완료 여부를 추적한다.
// 따라서 선행 작업이 실패해도 후속 작업은 실행된다.
// 실제 엔진/툴에서는 실패 시 후속 작업 취소, 오류 전파, fallback 실행 중
// 어떤 정책을 쓸지 별도로 설계해야 한다.
// =============================================================================
static void Experiment3_DependencyCompletionIsNotSuccess()
{
    std::cout << "\n[실험 3] 의존성 완료와 성공은 별개\n";
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

    auto after = pool.SubmitAfter({ failed }, [&Log]
    {
        Log("B 실행: A가 성공해서가 아니라 완료됐기 때문에 실행됨");
    });

    after.Wait();

    try
    {
        failed.Wait();
    }
    catch (const std::exception& e)
    {
        Log(std::string("A 예외 확인: ") + e.what());
    }

    PrintLog(log);
    std::cout << "  정책 포인트: dependency counter는 완료 순서만 보장하고 실패 정책은 별도 문제\n";
}

// =============================================================================
// main
// =============================================================================
int main()
{
    std::cout << "=====================================================\n";
    std::cout << "  Day 12 — Exception Propagation / Worker Survival\n";
    std::cout << "=====================================================\n";

    Experiment1_WaitRethrows();
    Experiment2_WorkerSurvives();
    Experiment3_DependencyCompletionIsNotSuccess();

    std::cout << "\n=====================================================\n";
    std::cout << "  오늘의 핵심\n";
    std::cout << "=====================================================\n";
    std::cout << "  job() 예외          : worker wrapper에서 catch\n";
    std::cout << "  std::exception_ptr  : JobState에 실패 원인 저장\n";
    std::cout << "  JobHandle::Wait     : 완료 대기 후 예외 재전파\n";
    std::cout << "  WorkerLoop          : 예외가 새지 않으므로 계속 작업 처리\n";
    std::cout << "  의존성 카운터       : 성공 여부가 아니라 완료 여부를 추적\n";
    std::cout << "=====================================================\n\n";

    return 0;
}
