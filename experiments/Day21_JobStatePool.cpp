// =============================================================================
// Day 21 — JobState Pool
//
// 목표:
//   Submit마다 새 JobState를 만들면 작업 수만큼 heap allocation이 발생한다.
//   JobState 객체를 pool에서 재사용하면 객체 allocation 수를 줄일 수 있다.
//
// 주의:
//   이 실험은 ThreadPool 코어를 바로 바꾸지 않고, 수명 규칙과 재사용 효과만
//   독립적으로 검증한다. shared_ptr control block allocation은 아직 남아 있다.
// =============================================================================

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <exception>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

using Clock = std::chrono::steady_clock;

struct BaselineJobState
{
    bool done = false;
    std::exception_ptr exception;
    std::vector<std::function<void()>> continuations;
};

struct PooledJobState
{
    bool done = false;
    std::exception_ptr exception;
    std::vector<std::function<void()>> continuations;
    std::size_t generation = 0;

    void ClearState()
    {
        done = false;
        exception = nullptr;
        continuations.clear();
    }

    void ResetForReuse()
    {
        ClearState();
        ++generation;
    }
};

class JobStatePool
{
public:
    using Handle = std::shared_ptr<PooledJobState>;

    Handle Acquire()
    {
        PooledJobState* state = nullptr;

        {
            std::lock_guard<std::mutex> lock(_mutex);
            ++_acquired;

            if (_freeList.empty())
            {
                state = new PooledJobState();
                ++_created;
            }
            else
            {
                state = _freeList.back().release();
                _freeList.pop_back();
                ++_reused;
            }
        }

        state->ResetForReuse();

        return Handle(state, [this](PooledJobState* returned)
        {
            Release(returned);
        });
    }

    std::size_t CreatedCount() const
    {
        std::lock_guard<std::mutex> lock(_mutex);
        return _created;
    }

    std::size_t ReusedCount() const
    {
        std::lock_guard<std::mutex> lock(_mutex);
        return _reused;
    }

    std::size_t AcquiredCount() const
    {
        std::lock_guard<std::mutex> lock(_mutex);
        return _acquired;
    }

    std::size_t FreeCount() const
    {
        std::lock_guard<std::mutex> lock(_mutex);
        return _freeList.size();
    }

private:
    void Release(PooledJobState* state)
    {
        state->ClearState();

        std::lock_guard<std::mutex> lock(_mutex);
        _freeList.emplace_back(state);
    }

    mutable std::mutex _mutex;
    std::vector<std::unique_ptr<PooledJobState>> _freeList;
    std::size_t _created = 0;
    std::size_t _reused = 0;
    std::size_t _acquired = 0;
};

struct BenchmarkResult
{
    std::string name;
    std::size_t acquisitions = 0;
    std::size_t objectCreations = 0;
    std::size_t objectReuses = 0;
    double elapsedMs = 0.0;
};

static double MsBetween(Clock::time_point begin, Clock::time_point end)
{
    return std::chrono::duration<double, std::milli>(end - begin).count();
}

static BenchmarkResult RunBaselineBenchmark(std::size_t batchSize, std::size_t rounds)
{
    std::vector<std::shared_ptr<BaselineJobState>> live;
    live.reserve(batchSize);

    const Clock::time_point begin = Clock::now();
    for (std::size_t round = 0; round < rounds; ++round)
    {
        live.clear();
        for (std::size_t i = 0; i < batchSize; ++i)
        {
            auto state = std::make_shared<BaselineJobState>();
            state->done = true;
            state->continuations.push_back([] {});
            live.push_back(std::move(state));
        }
    }
    live.clear();
    const Clock::time_point end = Clock::now();

    return BenchmarkResult{
        "make_shared per submit",
        batchSize * rounds,
        batchSize * rounds,
        0,
        MsBetween(begin, end)
    };
}

static BenchmarkResult RunPoolBenchmark(std::size_t batchSize, std::size_t rounds)
{
    JobStatePool pool;
    std::vector<JobStatePool::Handle> live;
    live.reserve(batchSize);

    const Clock::time_point begin = Clock::now();
    for (std::size_t round = 0; round < rounds; ++round)
    {
        live.clear();
        for (std::size_t i = 0; i < batchSize; ++i)
        {
            auto state = pool.Acquire();
            state->done = true;
            state->continuations.push_back([] {});
            live.push_back(std::move(state));
        }
    }
    live.clear();
    const Clock::time_point end = Clock::now();

    return BenchmarkResult{
        "pooled JobState object",
        pool.AcquiredCount(),
        pool.CreatedCount(),
        pool.ReusedCount(),
        MsBetween(begin, end)
    };
}

static void PrintBenchmark(const BenchmarkResult& result)
{
    std::cout << "  " << std::left << std::setw(24) << result.name
              << " acquisitions=" << std::setw(8) << result.acquisitions
              << " created=" << std::setw(8) << result.objectCreations
              << " reused=" << std::setw(8) << result.objectReuses
              << " elapsed=" << std::fixed << std::setprecision(2)
              << result.elapsedMs << " ms\n";
}

static void Experiment1_ResetAndLifetime()
{
    std::cout << "\n[실험 1] reset + lifetime rule\n";
    std::cout << "---------------------------------------------\n";

    JobStatePool pool;
    auto first = pool.Acquire();
    PooledJobState* firstAddress = first.get();
    first->done = true;
    first->exception = std::make_exception_ptr(std::runtime_error("dirty"));
    first->continuations.push_back([] {});
    const std::size_t firstGeneration = first->generation;

    auto stillAlive = first;
    first.reset();

    std::cout << "  free while alias alive: " << pool.FreeCount() << "\n";
    stillAlive.reset();
    std::cout << "  free after last handle: " << pool.FreeCount() << "\n";

    auto second = pool.Acquire();
    std::cout << "  same object reused: " << std::boolalpha
              << (second.get() == firstAddress) << "\n";
    std::cout << "  state reset: done=" << second->done
              << ", continuations=" << second->continuations.size()
              << ", generation " << firstGeneration << " -> "
              << second->generation << "\n";
}

static void Experiment2_BatchReuse()
{
    std::cout << "\n[실험 2] batch 단위 재사용 확인\n";
    std::cout << "---------------------------------------------\n";

    JobStatePool pool;
    std::vector<JobStatePool::Handle> live;
    std::vector<PooledJobState*> firstBatch;
    std::vector<PooledJobState*> secondBatch;

    constexpr std::size_t kBatchSize = 16;
    live.reserve(kBatchSize);
    firstBatch.reserve(kBatchSize);
    secondBatch.reserve(kBatchSize);

    for (std::size_t i = 0; i < kBatchSize; ++i)
    {
        live.push_back(pool.Acquire());
        firstBatch.push_back(live.back().get());
    }

    live.clear();

    for (std::size_t i = 0; i < kBatchSize; ++i)
    {
        live.push_back(pool.Acquire());
        secondBatch.push_back(live.back().get());
    }

    std::unordered_set<PooledJobState*> firstSet(firstBatch.begin(), firstBatch.end());
    std::size_t reusedAddresses = 0;
    for (PooledJobState* state : secondBatch)
    {
        if (firstSet.find(state) != firstSet.end())
            ++reusedAddresses;
    }

    std::cout << "  created objects: " << pool.CreatedCount() << "\n";
    std::cout << "  reused acquisitions: " << pool.ReusedCount() << "\n";
    std::cout << "  address overlap: " << reusedAddresses << " / " << kBatchSize << "\n";
}

static void Experiment3_AllocationPressure()
{
    std::cout << "\n[실험 3] allocation pressure 비교\n";
    std::cout << "---------------------------------------------\n";

    constexpr std::size_t kBatchSize = 50000;
    constexpr std::size_t kRounds = 20;

    const BenchmarkResult baseline = RunBaselineBenchmark(kBatchSize, kRounds);
    const BenchmarkResult pooled = RunPoolBenchmark(kBatchSize, kRounds);

    PrintBenchmark(baseline);
    PrintBenchmark(pooled);

    const std::size_t avoidedCreations =
        baseline.objectCreations > pooled.objectCreations
            ? baseline.objectCreations - pooled.objectCreations
            : 0;

    std::cout << "  avoided JobState object creations: " << avoidedCreations << "\n";
    std::cout << "  note: shared_ptr control block allocation은 아직 별도 최적화 대상\n";
}

int main()
{
    std::cout << "=====================================================\n";
    std::cout << "  Day 21 — JobState Pool\n";
    std::cout << "=====================================================\n";

    Experiment1_ResetAndLifetime();
    Experiment2_BatchReuse();
    Experiment3_AllocationPressure();

    std::cout << "\n오늘의 핵심\n";
    std::cout << "  - 핸들이 살아 있는 JobState는 pool로 돌아가지 않는다.\n";
    std::cout << "  - 반환된 JobState는 done/exception/continuation을 반드시 reset해야 한다.\n";
    std::cout << "  - 객체 재사용은 allocation 수를 줄이지만 control block 비용은 남는다.\n";
    return 0;
}
