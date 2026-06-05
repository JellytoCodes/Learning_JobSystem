#include "ThreadPool.h"

// =============================================================================
// 생성자
// =============================================================================
ThreadPool::ThreadPool(uint32_t threadCount)
{
    if (threadCount == 0)
        throw std::invalid_argument("ThreadPool: threadCount는 1 이상이어야 합니다.");

    // make_unique<T[]>(n): n개의 atomic을 0으로 value-init해 힙에 할당.
    // vector::resize()와 달리 이동을 시도하지 않으므로 atomic과 안전하게 동작.
    _perThreadJobCount = std::make_unique<std::atomic<uint64_t>[]>(threadCount);

    _workers.reserve(threadCount);
    for (uint32_t i = 0; i < threadCount; ++i)
    {
        // threadIdx(i)를 값으로 캡처해 WorkerLoop에 전달.
        // 각 스레드가 자신의 인덱스를 알아야 카운터에 기록할 수 있다.
        _workers.emplace_back([this, i] { WorkerLoop(i); });
    }
}

// =============================================================================
// 소멸자
// =============================================================================
ThreadPool::~ThreadPool()
{
    // -------------------------------------------------------------------------
    // Step 1: _stop 플래그를 세운다.
    //   락을 잡고 세우는 이유:
    //     _stop = true 와 notify_all() 사이에 워커가 wait()에서 빠져나와
    //     다시 잠들어 버리면 종료 신호를 놓칠 수 있다.
    //     락 안에서 _stop을 세워야 이 타이밍 이슈(race condition)를 막을 수 있다.
    // -------------------------------------------------------------------------
    {
        std::unique_lock<std::mutex> lock(_queueMutex);
        _stop = true;
    }

    // Step 2: 잠든 모든 워커 스레드를 깨운다.
    // 깨어난 스레드들은 WorkerLoop에서 _stop 조건을 확인하고 루프를 탈출한다.
    _workerCv.notify_all();

    // Step 3: 모든 스레드가 완전히 종료될 때까지 기다린다.
    // join()을 하지 않으면 스레드가 실행 중인데 ThreadPool이 소멸해 버려
    // _queueMutex, _workerCv 등 멤버 변수에 접근하다가 크래시가 발생한다.
    for (auto& worker : _workers)
        worker.join();
}

// =============================================================================
// CompleteJobState — 작업 완료 상태 전파 + continuation 실행
// =============================================================================
void ThreadPool::CompleteJobState(const std::shared_ptr<JobState>& state)
{
    std::vector<std::function<void()>> continuations;
    {
        std::unique_lock<std::mutex> lock(state->mutex);
        state->done.store(true, std::memory_order_release);
        continuations.swap(state->continuations);
    }

    state->cv.notify_all();

    // continuation은 JobState 락 밖에서 실행한다.
    // 후속 작업 제출 중 다른 락(_queueMutex)을 잡을 수 있으므로 락 중첩을 피한다.
    for (auto& continuation : continuations)
        continuation();
}

// =============================================================================
// EnqueueWithState — 지정된 JobState와 연결된 작업을 큐에 넣는다
// =============================================================================
void ThreadPool::EnqueueWithState(std::function<void()> job, std::shared_ptr<JobState> state)
{
    {
        std::unique_lock<std::mutex> lock(_queueMutex);

        if (_stop)
            throw std::runtime_error("ThreadPool: 종료된 풀에 작업을 제출할 수 없습니다.");

        _jobQueue.push([job = std::move(job), state = std::move(state)]() mutable
        {
            job();
            CompleteJobState(state);
        });

        ++_pendingJobs;
    }

    _workerCv.notify_one();
}

// =============================================================================
// Submit — 작업을 큐에 넣는다
// =============================================================================
JobHandle ThreadPool::Submit(std::function<void()> job)
{
    auto state = std::make_shared<JobState>();
    EnqueueWithState(std::move(job), state);
    return JobHandle(std::move(state));
}

// =============================================================================
// SubmitAfter — 모든 선행 작업 완료 후 후속 작업 자동 제출
// =============================================================================
JobHandle ThreadPool::SubmitAfter(
    const std::vector<JobHandle>& dependencies,
    std::function<void()> job)
{
    auto state      = std::make_shared<JobState>();
    auto remaining  = std::make_shared<std::atomic<uint32_t>>(
        static_cast<uint32_t>(dependencies.size()));
    auto submitOnce = std::make_shared<std::atomic<bool>>(false);
    auto jobPtr     = std::make_shared<std::function<void()>>(std::move(job));

    auto submitIfReady = [this, state, remaining, submitOnce, jobPtr]
    {
        if (remaining->load(std::memory_order_acquire) != 0)
            return;

        if (!submitOnce->exchange(true, std::memory_order_acq_rel))
            EnqueueWithState(std::move(*jobPtr), state);
    };

    if (dependencies.empty())
    {
        submitIfReady();
        return JobHandle(std::move(state));
    }

    for (const JobHandle& dependency : dependencies)
    {
        dependency.AddContinuation([remaining, submitIfReady]
        {
            if (remaining->fetch_sub(1, std::memory_order_acq_rel) == 1)
                submitIfReady();
        });
    }

    return JobHandle(std::move(state));
}

// =============================================================================
// WaitAll — 모든 작업이 완료될 때까지 대기
// =============================================================================
void ThreadPool::WaitAll()
{
    std::unique_lock<std::mutex> lock(_completionMutex);

    // _pendingJobs가 0이 될 때까지 블록.
    // 람다 predicate를 쓰는 이유:
    //   condition_variable은 spurious wakeup(가짜 깨어남)이 발생할 수 있다.
    //   OS 스케줄러가 이유 없이 스레드를 깨울 수 있기 때문.
    //   predicate가 있으면 wait()가 내부에서 조건을 재확인하고,
    //   false면 다시 잠든다 → spurious wakeup을 자동으로 처리.
    _completionCv.wait(lock, [this] { return _pendingJobs.load() == 0; });
}

// =============================================================================
// WorkerLoop — 각 워커 스레드가 실행하는 루프
// =============================================================================
void ThreadPool::WorkerLoop(uint32_t threadIdx)
{
    while (true)
    {
        std::function<void()> job;

        {
            std::unique_lock<std::mutex> lock(_queueMutex);

            _workerCv.wait(lock, [this]
            {
                return !_jobQueue.empty() || _stop;
            });

            if (_stop && _jobQueue.empty())
                return;

            job = std::move(_jobQueue.front());
            _jobQueue.pop();

        }

        job();

        // 이 스레드의 처리 카운터 증가.
        // 자신의 인덱스에만 접근하므로 다른 스레드와 경합 없음.
        ++_perThreadJobCount[threadIdx];

        const uint32_t remaining = _pendingJobs.fetch_sub(1) - 1;

        if (remaining == 0)
        {
            std::unique_lock<std::mutex> lock(_completionMutex);
            _completionCv.notify_all();
        }
    }
}
