#include "ThreadPool.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
using Ms = std::chrono::milliseconds;

static void SleepMs(int milliseconds) { std::this_thread::sleep_for(Ms(milliseconds)); }

enum class DependencyPolicy { Completion, AllSucceeded };
enum class JobStatus { Pending, Running, Succeeded, Failed, Canceled };

static const char* ToString(DependencyPolicy policy)
{
    return policy == DependencyPolicy::AllSucceeded ? "all_succeeded" : "completion";
}

static const char* ToString(JobStatus status)
{
    switch (status)
    {
    case JobStatus::Pending:   return "pending";
    case JobStatus::Running:   return "running";
    case JobStatus::Succeeded: return "success";
    case JobStatus::Failed:    return "failed";
    case JobStatus::Canceled:  return "canceled";
    }
    return "unknown";
}

struct JobRecord
{
    std::string name;
    std::vector<size_t> dependencies;
    DependencyPolicy policy = DependencyPolicy::Completion;
    JobStatus status = JobStatus::Pending;
    TimePoint startedAt{};
    TimePoint finishedAt{};
    uint64_t threadId = 0;
    std::string error;
};

class FailureCancelGraph
{
public:
    using JobId = size_t;

    JobId AddJob(std::string name, std::function<void()> job,
        DependencyPolicy policy = DependencyPolicy::Completion)
    {
        _nodes.push_back(Node{std::move(name), std::move(job), policy, {}});
        return _nodes.size() - 1;
    }

    void AddDependency(JobId job, JobId dependency)
    {
        if (job >= _nodes.size() || dependency >= _nodes.size() || dependency >= job)
            throw std::invalid_argument("FailureCancelGraph requires an earlier valid dependency");

        auto& dependencies = _nodes[job].dependencies;
        if (std::find(dependencies.begin(), dependencies.end(), dependency) != dependencies.end())
            throw std::invalid_argument("FailureCancelGraph duplicate dependency");
        dependencies.push_back(dependency);
    }

    std::vector<JobRecord> Run(ThreadPool& pool, TimePoint& graphStartedAt) const
    {
        graphStartedAt = Clock::now();
        auto state = std::make_shared<TraceState>();
        state->graphStartedAt = graphStartedAt;
        state->records.resize(_nodes.size());

        for (JobId id = 0; id < _nodes.size(); ++id)
        {
            state->records[id].name = _nodes[id].name;
            state->records[id].dependencies = _nodes[id].dependencies;
            state->records[id].policy = _nodes[id].policy;
        }

        std::vector<JobHandle> handles(_nodes.size());
        for (JobId id = 0; id < _nodes.size(); ++id)
        {
            const Node& node = _nodes[id];
            std::vector<JobHandle> dependencies;
            for (JobId dependency : node.dependencies)
                dependencies.push_back(handles[dependency]);

            auto tracedJob = [state, id, job = node.job]() mutable
            {
                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    auto& record = state->records[id];
                    record.status = JobStatus::Running;
                    record.startedAt = Clock::now();
                    record.threadId = ThreadId();
                }

                try
                {
                    job();
                }
                catch (const std::exception& exception)
                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    auto& record = state->records[id];
                    record.status = JobStatus::Failed;
                    record.finishedAt = Clock::now();
                    record.error = exception.what();
                    throw;
                }
                catch (...)
                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    auto& record = state->records[id];
                    record.status = JobStatus::Failed;
                    record.finishedAt = Clock::now();
                    record.error = "unknown exception";
                    throw;
                }

                std::lock_guard<std::mutex> lock(state->mutex);
                auto& record = state->records[id];
                record.status = JobStatus::Succeeded;
                record.finishedAt = Clock::now();
            };

            if (dependencies.empty())
                handles[id] = pool.Submit(std::move(tracedJob));
            else if (node.policy == DependencyPolicy::AllSucceeded)
                handles[id] = pool.SubmitAfterAllSucceeded(dependencies, std::move(tracedJob));
            else
                handles[id] = pool.SubmitAfter(dependencies, std::move(tracedJob));
        }

        for (JobId id = 0; id < handles.size(); ++id)
        {
            try
            {
                handles[id].Wait();
            }
            catch (const JobCanceledException& exception)
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                auto& record = state->records[id];
                record.status = JobStatus::Canceled;
                record.finishedAt = LatestDependencyFinish(*state, record.dependencies);
                record.error = exception.what();
            }
            catch (...)
            {
                // The traced wrapper already captured the failure.
            }
        }

        pool.WaitAll();
        std::lock_guard<std::mutex> lock(state->mutex);
        return state->records;
    }

private:
    struct Node
    {
        std::string name;
        std::function<void()> job;
        DependencyPolicy policy;
        std::vector<JobId> dependencies;
    };

    struct TraceState
    {
        std::mutex mutex;
        TimePoint graphStartedAt{};
        std::vector<JobRecord> records;
    };

    static uint64_t ThreadId()
    {
        return 1ULL + static_cast<uint64_t>(
            std::hash<std::thread::id>{}(std::this_thread::get_id()) % 900000ULL);
    }

    static TimePoint LatestDependencyFinish(const TraceState& state,
        const std::vector<JobId>& dependencies)
    {
        TimePoint latest = state.graphStartedAt;
        for (JobId dependency : dependencies)
            latest = std::max(latest, state.records[dependency].finishedAt);
        return latest;
    }

    std::vector<Node> _nodes;
};

static std::string EscapeJson(const std::string& text)
{
    std::string escaped;
    for (char character : text)
    {
        switch (character)
        {
        case '\\': escaped += "\\\\"; break;
        case '"':  escaped += "\\\""; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        default:   escaped += character; break;
        }
    }
    return escaped;
}

static int64_t ToUs(TimePoint origin, TimePoint point)
{
    return std::chrono::duration_cast<std::chrono::microseconds>(point - origin).count();
}

static bool ExportChromeTrace(const std::string& path, TimePoint origin,
    const std::vector<JobRecord>& records)
{
    constexpr uint64_t SchedulerThreadId = 999999;
    std::ofstream output(path, std::ios::binary);
    if (!output) return false;

    output << "{\n  \"displayTimeUnit\": \"ms\",\n  \"traceEvents\": [\n";
    bool first = true;
    auto Separator = [&]
    {
        if (!first) output << ",\n";
        first = false;
    };

    Separator();
    output << "    {\"name\":\"process_name\",\"ph\":\"M\",\"pid\":1,\"tid\":0,"
           << "\"args\":{\"name\":\"Day27 Failure Cancel Trace\"}}";
    Separator();
    output << "    {\"name\":\"thread_name\",\"ph\":\"M\",\"pid\":1,"
           << "\"tid\":" << SchedulerThreadId
           << ",\"args\":{\"name\":\"Dependency Scheduler\"}}";

    for (const auto& record : records)
    {
        Separator();
        if (record.status == JobStatus::Canceled)
        {
            output << "    {\"name\":\"" << EscapeJson(record.name) << "\","
                   << "\"cat\":\"job_state\",\"ph\":\"i\",\"s\":\"t\",\"pid\":1,"
                   << "\"tid\":" << SchedulerThreadId << ",\"ts\":" << ToUs(origin, record.finishedAt)
                   << ",\"args\":{\"status\":\"canceled\",\"policy\":\"" << ToString(record.policy)
                   << "\",\"timestamp_source\":\"latest_dependency_finish\",\"reason\":\""
                   << EscapeJson(record.error) << "\"}}";
        }
        else
        {
            output << "    {\"name\":\"" << EscapeJson(record.name) << "\","
                   << "\"cat\":\"job\",\"ph\":\"X\",\"pid\":1,\"tid\":" << record.threadId
                   << ",\"ts\":" << ToUs(origin, record.startedAt)
                   << ",\"dur\":" << std::max<int64_t>(0, ToUs(record.startedAt, record.finishedAt))
                   << ",\"args\":{\"status\":\"" << ToString(record.status)
                   << "\",\"policy\":\"" << ToString(record.policy)
                   << "\",\"error\":\"" << EscapeJson(record.error) << "\"}}";
        }
    }

    uint64_t flowId = 1;
    for (const auto& canceled : records)
    {
        if (canceled.status != JobStatus::Canceled) continue;
        for (size_t dependency : canceled.dependencies)
        {
            const auto& cause = records[dependency];
            if (cause.status != JobStatus::Failed && cause.status != JobStatus::Canceled) continue;
            const uint64_t causeThread = cause.status == JobStatus::Canceled
                ? SchedulerThreadId : cause.threadId;

            Separator();
            output << "    {\"name\":\"cancel propagation\",\"cat\":\"cancel_flow\",\"ph\":\"s\","
                   << "\"pid\":1,\"tid\":" << causeThread << ",\"ts\":" << ToUs(origin, cause.finishedAt)
                   << ",\"id\":" << flowId << "}";
            Separator();
            output << "    {\"name\":\"cancel propagation\",\"cat\":\"cancel_flow\",\"ph\":\"f\","
                   << "\"pid\":1,\"tid\":" << SchedulerThreadId << ",\"ts\":" << ToUs(origin, canceled.finishedAt)
                   << ",\"id\":" << flowId++ << "}";
        }
    }

    output << "\n  ]\n}\n";
    return static_cast<bool>(output);
}

static void Check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

int main(int argc, char* argv[])
{
    const std::string outputPath = argc > 1 ? argv[1] : "Day27_FailureCancelTrace.json";
    ThreadPool pool(3);
    FailureCancelGraph graph;
    bool cleanupRan = false;
    bool buildRan = false;
    bool publishRan = false;

    const auto load = graph.AddJob("LoadInput", [] { SleepMs(12); });
    const auto decode = graph.AddJob("Decode", []
    {
        SleepMs(18);
        throw std::runtime_error("corrupt input payload");
    }, DependencyPolicy::AllSucceeded);
    graph.AddDependency(decode, load);

    const auto cleanup = graph.AddJob("Cleanup", [&] { cleanupRan = true; SleepMs(5); });
    graph.AddDependency(cleanup, decode);
    const auto build = graph.AddJob("BuildOutput", [&] { buildRan = true; }, DependencyPolicy::AllSucceeded);
    graph.AddDependency(build, decode);
    const auto publish = graph.AddJob("Publish", [&] { publishRan = true; }, DependencyPolicy::AllSucceeded);
    graph.AddDependency(publish, build);

    TimePoint origin;
    const auto records = graph.Run(pool, origin);
    Check(records[load].status == JobStatus::Succeeded, "LoadInput should succeed");
    Check(records[decode].status == JobStatus::Failed, "Decode should fail");
    Check(records[cleanup].status == JobStatus::Succeeded && cleanupRan, "Cleanup should run");
    Check(records[build].status == JobStatus::Canceled, "BuildOutput should be canceled");
    Check(records[publish].status == JobStatus::Canceled, "Publish should be canceled");
    Check(!buildRan && !publishRan, "canceled job body executed");
    Check(ExportChromeTrace(outputPath, origin, records), "trace export failed");

    std::cout << "job              policy          status\n";
    std::cout << "---------------------------------------------\n";
    for (const auto& record : records)
        std::cout << std::left << std::setw(17) << record.name
                  << std::setw(16) << ToString(record.policy)
                  << ToString(record.status) << "\n";

    std::cout << "\ntrace file: " << outputPath << "\n";
    std::cout << "Canceled jobs are scheduler events; their timestamps are inferred from dependencies.\n";
    return 0;
}
