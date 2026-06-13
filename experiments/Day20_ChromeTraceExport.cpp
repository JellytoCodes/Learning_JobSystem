// =============================================================================
// Day 20 - Chrome Trace JSON Export
//
// 목표:
//   Day19에서 수집한 ready/start/end 데이터를 Chrome Trace Event 형식으로
//   내보내 Perfetto나 chrome://tracing에서 타임라인으로 확인한다.
//
// 주요 event phase:
//   X : duration이 있는 실행/대기 구간
//   s/f : dependency flow 시작/끝
//   M : process/thread 이름 metadata
// =============================================================================

#include "ThreadPool.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
using Ms = std::chrono::milliseconds;

static void SleepMs(int ms) { std::this_thread::sleep_for(Ms(ms)); }

struct TraceRecord
{
    std::string name;
    std::vector<size_t> dependencies;
    TimePoint startedAt{};
    TimePoint finishedAt{};
    uint64_t threadId = 0;
    bool executed = false;
    bool succeeded = false;
};

class ChromeTraceRecorder
{
public:
    using JobId = size_t;

    void BeginGraph(
        TimePoint graphStartedAt,
        const std::vector<std::string>& names,
        const std::vector<std::vector<JobId>>& dependencies)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _graphStartedAt = graphStartedAt;
        _records.clear();
        _records.resize(names.size());

        for (JobId id = 0; id < names.size(); ++id)
        {
            _records[id].name = names[id];
            _records[id].dependencies = dependencies[id];
        }
    }

    void RecordExecution(
        JobId id,
        TimePoint startedAt,
        TimePoint finishedAt,
        bool succeeded)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        TraceRecord& record = _records[id];
        record.startedAt = startedAt;
        record.finishedAt = finishedAt;
        record.threadId = ThreadId();
        record.executed = true;
        record.succeeded = succeeded;
    }

    bool Export(const std::string& path) const
    {
        TimePoint graphStartedAt;
        std::vector<TraceRecord> records;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            graphStartedAt = _graphStartedAt;
            records = _records;
        }

        std::ofstream output(path, std::ios::binary);
        if (!output)
            return false;

        output << "{\n  \"displayTimeUnit\": \"ms\",\n  \"traceEvents\": [\n";
        bool first = true;

        auto WriteSeparator = [&]
        {
            if (!first)
                output << ",\n";
            first = false;
        };

        WriteSeparator();
        output << "    {\"name\":\"process_name\",\"ph\":\"M\",\"pid\":1,\"tid\":0,"
               << "\"args\":{\"name\":\"Learning_JobSystem\"}}";

        std::set<uint64_t> workerThreads;
        for (const TraceRecord& record : records)
        {
            if (record.executed)
                workerThreads.insert(record.threadId);
        }

        for (uint64_t threadId : workerThreads)
        {
            WriteSeparator();
            output << "    {\"name\":\"thread_name\",\"ph\":\"M\",\"pid\":1,"
                   << "\"tid\":" << threadId
                   << ",\"args\":{\"name\":\"Worker " << threadId << "\"}}";
        }

        for (JobId id = 0; id < records.size(); ++id)
        {
            const TraceRecord& record = records[id];
            if (!record.executed)
                continue;

            const TimePoint readyAt = ReadyTime(records, record, graphStartedAt);
            const uint64_t queueThreadId = QueueThreadId(id);

            WriteSeparator();
            output << "    {\"name\":\"thread_name\",\"ph\":\"M\",\"pid\":1,"
                   << "\"tid\":" << queueThreadId << ",\"args\":{\"name\":\"Queue: "
                   << EscapeJson(record.name) << "\"}}";

            WriteSeparator();
            output << "    {\"name\":\"" << EscapeJson(record.name) << " queue wait\","
                   << "\"cat\":\"queue_wait\",\"ph\":\"X\",\"pid\":1,"
                   << "\"tid\":" << queueThreadId
                   << ",\"ts\":" << ToMicroseconds(graphStartedAt, readyAt)
                   << ",\"dur\":" << DurationMicroseconds(readyAt, record.startedAt)
                   << ",\"args\":{\"dependency_count\":" << record.dependencies.size() << "}}";

            WriteSeparator();
            output << "    {\"name\":\"" << EscapeJson(record.name) << "\","
                   << "\"cat\":\"job\",\"ph\":\"X\",\"pid\":1,"
                   << "\"tid\":" << record.threadId
                   << ",\"ts\":" << ToMicroseconds(graphStartedAt, record.startedAt)
                   << ",\"dur\":" << DurationMicroseconds(record.startedAt, record.finishedAt)
                   << ",\"args\":{\"status\":\""
                   << (record.succeeded ? "success" : "failed") << "\"}}";
        }

        uint64_t flowId = 1;
        for (JobId child = 0; child < records.size(); ++child)
        {
            const TraceRecord& childRecord = records[child];
            if (!childRecord.executed)
                continue;

            for (JobId dependency : childRecord.dependencies)
            {
                const TraceRecord& parentRecord = records[dependency];
                if (!parentRecord.executed)
                    continue;

                WriteSeparator();
                output << "    {\"name\":\"dependency\",\"cat\":\"flow\",\"ph\":\"s\","
                       << "\"pid\":1,\"tid\":" << parentRecord.threadId
                       << ",\"ts\":" << ToMicroseconds(graphStartedAt, parentRecord.finishedAt)
                       << ",\"id\":" << flowId << "}";

                WriteSeparator();
                output << "    {\"name\":\"dependency\",\"cat\":\"flow\",\"ph\":\"f\","
                       << "\"pid\":1,\"tid\":" << childRecord.threadId
                       << ",\"ts\":" << ToMicroseconds(graphStartedAt, childRecord.startedAt)
                       << ",\"id\":" << flowId << "}";
                ++flowId;
            }
        }

        output << "\n  ]\n}\n";
        return static_cast<bool>(output);
    }

private:
    static uint64_t ThreadId()
    {
        // Trace viewer가 JSON number를 안정적으로 처리하도록 작은 논리 id 범위에 둔다.
        return 1ULL +
            static_cast<uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()) % 900000ULL);
    }

    static uint64_t QueueThreadId(JobId id)
    {
        return 1000000ULL + static_cast<uint64_t>(id);
    }

    static int64_t ToMicroseconds(TimePoint origin, TimePoint point)
    {
        return std::chrono::duration_cast<std::chrono::microseconds>(point - origin).count();
    }

    static int64_t DurationMicroseconds(TimePoint start, TimePoint end)
    {
        return std::max<int64_t>(0, ToMicroseconds(start, end));
    }

    static TimePoint ReadyTime(
        const std::vector<TraceRecord>& records,
        const TraceRecord& record,
        TimePoint graphStartedAt)
    {
        TimePoint readyAt = graphStartedAt;
        for (JobId dependency : record.dependencies)
            readyAt = std::max(readyAt, records[dependency].finishedAt);
        return readyAt;
    }

    static std::string EscapeJson(const std::string& text)
    {
        std::string escaped;
        escaped.reserve(text.size());

        for (char ch : text)
        {
            switch (ch)
            {
            case '\\': escaped += "\\\\"; break;
            case '"':  escaped += "\\\""; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default:   escaped += ch; break;
            }
        }
        return escaped;
    }

    mutable std::mutex _mutex;
    TimePoint _graphStartedAt{};
    std::vector<TraceRecord> _records;
};

class TraceJobGraph
{
public:
    using JobId = size_t;

    JobId AddJob(std::string name, std::function<void()> job)
    {
        _nodes.push_back(Node{std::move(name), std::move(job), {}});
        return _nodes.size() - 1;
    }

    void AddDependency(JobId job, JobId dependency)
    {
        if (job >= _nodes.size() || dependency >= _nodes.size() || dependency >= job)
            throw std::invalid_argument("TraceJobGraph: dependency는 먼저 선언된 유효한 노드여야 합니다.");
        _nodes[job].dependencies.push_back(dependency);
    }

    void Run(ThreadPool& pool, ChromeTraceRecorder& recorder) const
    {
        std::vector<std::string> names;
        std::vector<std::vector<JobId>> dependencies;
        names.reserve(_nodes.size());
        dependencies.reserve(_nodes.size());

        for (const Node& node : _nodes)
        {
            names.push_back(node.name);
            dependencies.push_back(node.dependencies);
        }

        recorder.BeginGraph(Clock::now(), names, dependencies);
        std::vector<JobHandle> handles(_nodes.size());

        for (JobId id = 0; id < _nodes.size(); ++id)
        {
            const Node& node = _nodes[id];
            std::vector<JobHandle> dependencyHandles;
            dependencyHandles.reserve(node.dependencies.size());
            for (JobId dependency : node.dependencies)
                dependencyHandles.push_back(handles[dependency]);

            auto tracedJob = [&recorder, id, job = node.job]() mutable
            {
                const TimePoint startedAt = Clock::now();
                try
                {
                    job();
                    recorder.RecordExecution(id, startedAt, Clock::now(), true);
                }
                catch (...)
                {
                    recorder.RecordExecution(id, startedAt, Clock::now(), false);
                    throw;
                }
            };

            handles[id] = dependencyHandles.empty()
                ? pool.Submit(std::move(tracedJob))
                : pool.SubmitAfter(dependencyHandles, std::move(tracedJob));
        }

        for (JobHandle& handle : handles)
        {
            try
            {
                handle.Wait();
            }
            catch (...)
            {
                // 실패 event도 trace에 남기므로 전체 그래프 완료까지 계속 기다린다.
            }
        }
    }

private:
    struct Node
    {
        std::string name;
        std::function<void()> job;
        std::vector<JobId> dependencies;
    };

    std::vector<Node> _nodes;
};

int main(int argc, char* argv[])
{
    const std::string outputPath = argc > 1 ? argv[1] : "Day20_JobTrace.json";

    ThreadPool pool(3);
    TraceJobGraph graph;
    ChromeTraceRecorder recorder;

    const auto load = graph.AddJob("Load Assets", [] { SleepMs(20); });
    const auto parse = graph.AddJob("Parse Scene", [] { SleepMs(25); });
    const auto shaders = graph.AddJob("Compile Shaders", [] { SleepMs(90); });
    const auto meshes = graph.AddJob("Build Meshes", [] { SleepMs(45); });
    const auto audio = graph.AddJob("Bake Audio", [] { SleepMs(30); });
    const auto package = graph.AddJob("Package", [] { SleepMs(15); });
    const auto upload = graph.AddJob("Upload", [] { SleepMs(10); });

    graph.AddDependency(parse, load);
    graph.AddDependency(shaders, parse);
    graph.AddDependency(meshes, parse);
    graph.AddDependency(audio, parse);
    graph.AddDependency(package, shaders);
    graph.AddDependency(package, meshes);
    graph.AddDependency(package, audio);
    graph.AddDependency(upload, package);

    graph.Run(pool, recorder);

    if (!recorder.Export(outputPath))
    {
        std::cerr << "trace export 실패: " << outputPath << "\n";
        return 1;
    }

    std::cout << "Day 20 - Chrome Trace Export\n";
    std::cout << "trace file: " << outputPath << "\n";
    std::cout << "Perfetto UI 또는 chrome://tracing에서 파일을 열어 확인하세요.\n";
    std::cout << "job=worker 실행 구간, queue wait=실행 가능 후 대기, flow=dependency 연결\n";
    return 0;
}
