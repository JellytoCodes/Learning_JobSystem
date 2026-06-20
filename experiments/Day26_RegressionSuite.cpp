// =============================================================================
// Day 26 - ThreadPool Stress / Regression Suite
//
// Goal:
//   Re-run the core contracts accumulated through Day25 in one executable.
//   The optional command-line multiplier increases workload without changing
//   the expected results.
//
// Usage:
//   Day26_RegressionSuite.exe       // normal regression pass
//   Day26_RegressionSuite.exe 10    // heavier stress pass
// =============================================================================

#include "ThreadPool.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using Clock = std::chrono::steady_clock;

static void Check(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

static uint64_t SumProcessed(const ThreadPool& pool)
{
    uint64_t total = 0;
    for (const ThreadPool::QueueStats& stat : pool.GetQueueStats())
        total += stat.jobsProcessed;
    return total;
}

static uint64_t SumGlobalPops(const ThreadPool& pool)
{
    uint64_t total = 0;
    for (const ThreadPool::QueueStats& stat : pool.GetQueueStats())
        total += stat.globalPops;
    return total;
}

static uint64_t SumLocalWork(const ThreadPool& pool)
{
    uint64_t total = 0;
    for (const ThreadPool::QueueStats& stat : pool.GetQueueStats())
        total += stat.localPops + stat.steals;
    return total;
}

class RegressionSuite
{
public:
    void Run(const std::string& name, const std::function<void()>& test)
    {
        const auto startedAt = Clock::now();

        try
        {
            test();
            const double elapsedMs = ToMilliseconds(startedAt, Clock::now());
            std::cout << "[PASS] " << std::left << std::setw(34) << name
                      << std::right << std::fixed << std::setprecision(2)
                      << elapsedMs << " ms\n";
            ++_passed;
        }
        catch (const std::exception& e)
        {
            const double elapsedMs = ToMilliseconds(startedAt, Clock::now());
            std::cout << "[FAIL] " << std::left << std::setw(34) << name
                      << std::right << std::fixed << std::setprecision(2)
                      << elapsedMs << " ms\n"
                      << "       " << e.what() << "\n";
            ++_failed;
        }
    }

    int PrintSummary() const
    {
        std::cout << "------------------------------------------------------------\n";
        std::cout << "passed: " << _passed << ", failed: " << _failed << "\n";
        return _failed == 0 ? 0 : 1;
    }

private:
    static double ToMilliseconds(Clock::time_point start, Clock::time_point end)
    {
        return std::chrono::duration<double, std::milli>(end - start).count();
    }

    int _passed = 0;
    int _failed = 0;
};

static void TestHighVolumeExternalSubmit(int multiplier)
{
    ThreadPool pool(8);
    const uint64_t jobCount = 5000ULL * static_cast<uint64_t>(multiplier);
    std::atomic<uint64_t> sum{0};

    for (uint64_t i = 0; i < jobCount; ++i)
    {
        (void)pool.Submit([&sum, i]
        {
            sum.fetch_add(i, std::memory_order_relaxed);
        });
    }

    pool.WaitAll();

    const uint64_t expected = jobCount * (jobCount - 1) / 2;
    Check(sum.load() == expected, "high-volume sum mismatch");
    Check(pool.GetPendingJobCount() == 0, "pending jobs did not return to zero");
    Check(SumProcessed(pool) == jobCount, "processed count mismatch");
    Check(SumGlobalPops(pool) == jobCount, "external jobs did not all use global queue");
}

static void TestNestedSubmitAndSteal(int multiplier)
{
    ThreadPool pool(4);
    constexpr int rootCount = 4;
    const int childrenPerRoot = 400 * multiplier;
    std::atomic<int> childCompleted{0};

    for (int root = 0; root < rootCount; ++root)
    {
        (void)pool.Submit([&pool, &childCompleted, childrenPerRoot]
        {
            for (int child = 0; child < childrenPerRoot; ++child)
            {
                (void)pool.Submit([&childCompleted]
                {
                    childCompleted.fetch_add(1, std::memory_order_relaxed);
                });
            }
        });
    }

    pool.WaitAll();

    const uint64_t childCount =
        static_cast<uint64_t>(rootCount) * static_cast<uint64_t>(childrenPerRoot);
    Check(childCompleted.load() == static_cast<int>(childCount), "nested child count mismatch");
    Check(SumGlobalPops(pool) == rootCount, "root jobs did not all use global queue");
    Check(SumLocalWork(pool) == childCount, "nested jobs were lost from local/steal paths");
    Check(SumProcessed(pool) == childCount + rootCount, "nested processed count mismatch");
}

static void TestDependencyFanIn(int multiplier)
{
    ThreadPool pool(6);
    const int dependencyCount = 64 * multiplier;
    std::atomic<int> completed{0};
    std::atomic<bool> fanInObservedAll{false};
    std::vector<JobHandle> dependencies;
    dependencies.reserve(static_cast<size_t>(dependencyCount));

    for (int i = 0; i < dependencyCount; ++i)
    {
        dependencies.push_back(pool.Submit([&completed]
        {
            completed.fetch_add(1, std::memory_order_relaxed);
        }));
    }

    JobHandle fanIn = pool.SubmitAfter(dependencies, [&completed, &fanInObservedAll, dependencyCount]
    {
        fanInObservedAll.store(completed.load(std::memory_order_relaxed) == dependencyCount);
    });

    fanIn.Wait();
    pool.WaitAll();

    Check(completed.load() == dependencyCount, "dependency completion count mismatch");
    Check(fanInObservedAll.load(), "fan-in ran before every dependency completed");
}

static void TestFailurePolicies()
{
    ThreadPool pool(3);
    std::atomic<bool> completionContinuationRan{false};
    std::atomic<bool> successContinuationRan{false};

    JobHandle failed = pool.Submit([]
    {
        throw std::runtime_error("expected failure");
    });

    JobHandle completion = pool.SubmitAfter({ failed }, [&completionContinuationRan]
    {
        completionContinuationRan = true;
    });

    JobHandle successOnly = pool.SubmitAfterAllSucceeded({ failed }, [&successContinuationRan]
    {
        successContinuationRan = true;
    });

    bool failureObserved = false;
    bool cancellationObserved = false;

    try
    {
        failed.Wait();
    }
    catch (const std::runtime_error&)
    {
        failureObserved = true;
    }

    completion.Wait();

    try
    {
        successOnly.Wait();
    }
    catch (const JobCanceledException&)
    {
        cancellationObserved = true;
    }

    pool.WaitAll();

    Check(failureObserved, "failed job exception was not propagated");
    Check(completionContinuationRan.load(), "completion continuation did not run");
    Check(!successContinuationRan.load(), "success-only continuation unexpectedly ran");
    Check(cancellationObserved, "success-only continuation was not canceled");
}

static void TestHelpingWaitUnderSaturation(int multiplier)
{
    const int rounds = 20 * multiplier;

    for (int round = 0; round < rounds; ++round)
    {
        ThreadPool pool(2);
        std::atomic<int> parentsCompleted{0};
        std::atomic<int> childrenCompleted{0};

        for (int parent = 0; parent < 2; ++parent)
        {
            (void)pool.Submit([&pool, &parentsCompleted, &childrenCompleted]
            {
                JobHandle child = pool.Submit([&childrenCompleted]
                {
                    childrenCompleted.fetch_add(1, std::memory_order_relaxed);
                });

                pool.WaitWithHelping(child);
                parentsCompleted.fetch_add(1, std::memory_order_relaxed);
            });
        }

        pool.WaitAll();
        Check(parentsCompleted.load() == 2, "helping wait parent count mismatch");
        Check(childrenCompleted.load() == 2, "helping wait child count mismatch");
    }
}

static void TestRepeatedPoolLifecycle(int multiplier)
{
    const int rounds = 20 * multiplier;
    constexpr int jobsPerRound = 50;
    std::atomic<int> completed{0};

    for (int round = 0; round < rounds; ++round)
    {
        ThreadPool pool(3);
        for (int job = 0; job < jobsPerRound; ++job)
        {
            (void)pool.Submit([&completed]
            {
                completed.fetch_add(1, std::memory_order_relaxed);
            });
        }
        pool.WaitAll();
    }

    Check(completed.load() == rounds * jobsPerRound, "repeated lifecycle lost jobs");
}

static int ParseMultiplier(int argc, char* argv[])
{
    if (argc < 2)
        return 1;

    const int multiplier = std::stoi(argv[1]);
    if (multiplier < 1 || multiplier > 100)
        throw std::invalid_argument("multiplier must be between 1 and 100");
    return multiplier;
}

int main(int argc, char* argv[])
{
    try
    {
        const int multiplier = ParseMultiplier(argc, argv);

        std::cout << "============================================================\n";
        std::cout << "  Day 26 - ThreadPool Stress / Regression Suite\n";
        std::cout << "  workload multiplier: " << multiplier << "\n";
        std::cout << "============================================================\n";

        RegressionSuite suite;
        suite.Run("high-volume external submit", [multiplier]
        {
            TestHighVolumeExternalSubmit(multiplier);
        });
        suite.Run("nested submit and local/steal", [multiplier]
        {
            TestNestedSubmitAndSteal(multiplier);
        });
        suite.Run("dependency fan-in", [multiplier]
        {
            TestDependencyFanIn(multiplier);
        });
        suite.Run("failure policies", []
        {
            TestFailurePolicies();
        });
        suite.Run("helping wait under saturation", [multiplier]
        {
            TestHelpingWaitUnderSaturation(multiplier);
        });
        suite.Run("repeated pool lifecycle", [multiplier]
        {
            TestRepeatedPoolLifecycle(multiplier);
        });

        return suite.PrintSummary();
    }
    catch (const std::exception& e)
    {
        std::cerr << "Day26 setup failed: " << e.what() << "\n";
        return 2;
    }
}
