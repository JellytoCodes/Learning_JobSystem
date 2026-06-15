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
    _localPopCount = std::make_unique<std::atomic<uint64_t>[]>(threadCount);
    _globalPopCount = std::make_unique<std::atomic<uint64_t>[]>(threadCount);
    _stealCount = std::make_unique<std::atomic<uint64_t>[]>(threadCount);

    _localQueues.reserve(threadCount);
    for (uint32_t i = 0; i < threadCount; ++i)
        _localQueues.push_back(std::make_unique<LocalQueue>());

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

thread_local ThreadPool* ThreadPool::_currentPool = nullptr;
thread_local uint32_t ThreadPool::_currentWorkerIdx = 0;

// =============================================================================
// CompleteJobState — 작업 완료 상태 전파 + continuation 실행
// =============================================================================
void ThreadPool::CompleteJobState(
    const std::shared_ptr<JobState>& state,
    std::exception_ptr exception)
{
    std::vector<std::function<void()>> continuations;
    {
        std::unique_lock<std::mutex> lock(state->mutex);
        state->exception = exception;
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
    auto wrappedJob = [job = std::move(job), state = std::move(state)]() mutable
    {
        try
        {
            job();
            CompleteJobState(state);
        }
        catch (...)
        {
            CompleteJobState(state, std::current_exception());
        }
    };

    {
        std::unique_lock<std::mutex> lock(_queueMutex);

        if (_stop)
            throw std::runtime_error("ThreadPool: 종료된 풀에 작업을 제출할 수 없습니다.");

        ++_pendingJobs;

        if (_currentPool == this)
        {
            std::lock_guard<std::mutex> localLock(_localQueues[_currentWorkerIdx]->mutex);
            _localQueues[_currentWorkerIdx]->jobs.push_back(std::move(wrappedJob));
        }
        else
        {
            _jobQueue.push(std::move(wrappedJob));
        }
    }

    _workerCv.notify_all();
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
// SubmitAfterAllSucceeded — 모든 선행 작업 성공 후 후속 작업 자동 제출
// =============================================================================
JobHandle ThreadPool::SubmitAfterAllSucceeded(
    const std::vector<JobHandle>& dependencies,
    std::function<void()> job)
{
    auto state      = std::make_shared<JobState>();
    auto remaining  = std::make_shared<std::atomic<uint32_t>>(
        static_cast<uint32_t>(dependencies.size()));
    auto submitOnce = std::make_shared<std::atomic<bool>>(false);
    auto jobPtr     = std::make_shared<std::function<void()>>(std::move(job));

    auto submitOrCancelIfReady = [this, dependencies, state, remaining, submitOnce, jobPtr]
    {
        if (remaining->load(std::memory_order_acquire) != 0)
            return;

        if (submitOnce->exchange(true, std::memory_order_acq_rel))
            return;

        for (const JobHandle& dependency : dependencies)
        {
            if (dependency.GetException())
            {
                CompleteJobState(
                    state,
                    std::make_exception_ptr(JobCanceledException(
                        "ThreadPool: 선행 작업 실패로 후속 작업이 취소되었습니다.")));
                return;
            }
        }

        EnqueueWithState(std::move(*jobPtr), state);
    };

    if (dependencies.empty())
    {
        submitOrCancelIfReady();
        return JobHandle(std::move(state));
    }

    for (const JobHandle& dependency : dependencies)
    {
        dependency.AddContinuation([remaining, submitOrCancelIfReady]
        {
            if (remaining->fetch_sub(1, std::memory_order_acq_rel) == 1)
                submitOrCancelIfReady();
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
// WaitWithHelping — 기다리는 동안 큐의 다른 작업을 직접 실행
// =============================================================================
void ThreadPool::WaitWithHelping(const JobHandle& handle)
{
    while (!handle.IsDone())
    {
        if (!TryExecuteOneJob(nullptr))
            std::this_thread::yield();
    }

    // 완료된 작업이 예외를 저장했다면 일반 Wait와 동일하게 호출자에게 재전파한다.
    handle.Wait();
}

// =============================================================================
// TryExecuteOneJob — 큐에서 작업 하나를 꺼내 현재 스레드에서 실행
// =============================================================================
bool ThreadPool::TryExecuteOneJob(uint32_t* workerThreadIdx)
{
    std::function<void()> job;

    if (workerThreadIdx && TryPopLocal(*workerThreadIdx, job))
    {
        ExecuteJob(std::move(job), workerThreadIdx);
        return true;
    }

    if (TryPopGlobal(workerThreadIdx, job))
    {
        ExecuteJob(std::move(job), workerThreadIdx);
        return true;
    }

    if (workerThreadIdx)
    {
        if (TrySteal(*workerThreadIdx, job))
        {
            ExecuteJob(std::move(job), workerThreadIdx);
            return true;
        }
    }
    else if (TryStealAny(job))
    {
        ExecuteJob(std::move(job), workerThreadIdx);
        return true;
    }

    return false;
}

// =============================================================================
// TryPopLocal — 자기 local queue에서 LIFO로 작업을 꺼낸다
// =============================================================================
bool ThreadPool::TryPopLocal(uint32_t workerThreadIdx, std::function<void()>& job)
{
    LocalQueue& localQueue = *_localQueues[workerThreadIdx];
    std::unique_lock<std::mutex> lock(localQueue.mutex);

    if (localQueue.jobs.empty())
        return false;

    job = std::move(localQueue.jobs.back());
    localQueue.jobs.pop_back();
    ++_localPopCount[workerThreadIdx];
    return true;
}

// =============================================================================
// TryPopGlobal — 외부 제출 작업이 들어오는 전역 큐에서 FIFO로 꺼낸다
// =============================================================================
bool ThreadPool::TryPopGlobal(uint32_t* workerThreadIdx, std::function<void()>& job)
{
    {
        std::unique_lock<std::mutex> lock(_queueMutex);
        if (_jobQueue.empty())
            return false;

        job = std::move(_jobQueue.front());
        _jobQueue.pop();
    }

    if (workerThreadIdx)
        ++_globalPopCount[*workerThreadIdx];

    return true;
}

// =============================================================================
// TrySteal — 다른 worker local queue 앞쪽에서 FIFO로 작업을 훔친다
// =============================================================================
bool ThreadPool::TrySteal(uint32_t workerThreadIdx, std::function<void()>& job)
{
    const uint32_t count = GetThreadCount();

    for (uint32_t offset = 1; offset < count; ++offset)
    {
        const uint32_t victimIdx = (workerThreadIdx + offset) % count;
        LocalQueue& victimQueue = *_localQueues[victimIdx];

        std::unique_lock<std::mutex> lock(victimQueue.mutex);
        if (victimQueue.jobs.empty())
            continue;

        job = std::move(victimQueue.jobs.front());
        victimQueue.jobs.pop_front();
        ++_stealCount[workerThreadIdx];
        return true;
    }

    return false;
}

bool ThreadPool::TryStealAny(std::function<void()>& job)
{
    for (uint32_t victimIdx = 0; victimIdx < GetThreadCount(); ++victimIdx)
    {
        LocalQueue& victimQueue = *_localQueues[victimIdx];

        std::unique_lock<std::mutex> lock(victimQueue.mutex);
        if (victimQueue.jobs.empty())
            continue;

        job = std::move(victimQueue.jobs.front());
        victimQueue.jobs.pop_front();
        return true;
    }

    return false;
}

bool ThreadPool::HasAnyLocalJob() const
{
    for (const std::unique_ptr<LocalQueue>& localQueue : _localQueues)
    {
        std::unique_lock<std::mutex> lock(localQueue->mutex);
        if (!localQueue->jobs.empty())
            return true;
    }

    return false;
}

// =============================================================================
// ExecuteJob — 작업 실행 후 통계와 pending count 정리
// =============================================================================
void ThreadPool::ExecuteJob(std::function<void()> job, uint32_t* workerThreadIdx)
{
    job();

    // helper로 실행한 작업은 특정 worker index가 없으므로 per-thread stats에서 제외한다.
    if (workerThreadIdx)
        ++_perThreadJobCount[*workerThreadIdx];

    const uint32_t remaining = _pendingJobs.fetch_sub(1) - 1;

    if (remaining == 0)
    {
        std::unique_lock<std::mutex> lock(_completionMutex);
        _completionCv.notify_all();
    }
}

// =============================================================================
// WorkerLoop — 각 워커 스레드가 실행하는 루프
// =============================================================================
void ThreadPool::WorkerLoop(uint32_t threadIdx)
{
    _currentPool = this;
    _currentWorkerIdx = threadIdx;

    while (true)
    {
        {
            std::unique_lock<std::mutex> lock(_queueMutex);

            _workerCv.wait(lock, [this]
            {
                return !_jobQueue.empty() || HasAnyLocalJob() || _stop;
            });

            if (_stop && _jobQueue.empty() && !HasAnyLocalJob())
                return;
        }

        (void)TryExecuteOneJob(&threadIdx);
    }
}
