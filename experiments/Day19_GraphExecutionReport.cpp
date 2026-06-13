// =============================================================================
// Day 19 - Graph Execution Report
//
// 목표:
//   그래프가 유효하게 실행되는지를 넘어, 어디서 기다리고 어디가 느린지 관찰한다.
//
// 측정 항목:
//   - ready -> start: queue wait
//   - start -> end: execution time
//   - node status / worker thread
//   - 측정된 실행 시간 기준 critical path
// =============================================================================

#include "ThreadPool.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
using Ms = std::chrono::milliseconds;

static void SleepMs(int ms) { std::this_thread::sleep_for(Ms(ms)); }

enum class DependencyPolicy
{
    Completion,
    AllSucceeded
};

enum class NodeStatus
{
    Pending,
    Running,
    Succeeded,
    Failed,
    Canceled
};

static const char* ToString(NodeStatus status)
{
    switch (status)
    {
    case NodeStatus::Pending:   return "pending";
    case NodeStatus::Running:   return "running";
    case NodeStatus::Succeeded: return "success";
    case NodeStatus::Failed:    return "failed";
    case NodeStatus::Canceled:  return "canceled";
    }
    return "unknown";
}

struct NodeExecutionRecord
{
    std::string name;
    std::vector<size_t> dependencies;
    NodeStatus status = NodeStatus::Pending;
    TimePoint readyAt{};
    TimePoint startedAt{};
    TimePoint finishedAt{};
    std::thread::id threadId{};
    std::string error;
};

static double ToMs(TimePoint from, TimePoint to)
{
    return std::chrono::duration<double, std::milli>(to - from).count();
}

class GraphExecutionReport
{
public:
    GraphExecutionReport(
        TimePoint graphStartedAt,
        TimePoint graphFinishedAt,
        std::vector<NodeExecutionRecord> records,
        std::vector<size_t> topologicalOrder)
        : _graphStartedAt(graphStartedAt),
          _graphFinishedAt(graphFinishedAt),
          _records(std::move(records)),
          _topologicalOrder(std::move(topologicalOrder))
    {
    }

    void Print() const
    {
        std::cout << "  node             status      wait(ms)  run(ms)   thread\n";
        std::cout << "  ---------------- ----------- --------- --------- ----------------\n";

        for (size_t id : _topologicalOrder)
        {
            const NodeExecutionRecord& record = _records[id];
            const bool executed = record.status == NodeStatus::Succeeded ||
                record.status == NodeStatus::Failed;

            std::ostringstream threadText;
            if (executed)
                threadText << record.threadId;
            else
                threadText << "-";

            const double waitMs = executed ? ToMs(record.readyAt, record.startedAt) : 0.0;
            const double runMs = executed ? ToMs(record.startedAt, record.finishedAt) : 0.0;

            std::cout << "  " << std::left << std::setw(16) << record.name
                      << " " << std::setw(11) << ToString(record.status)
                      << std::right << std::setw(9) << std::fixed << std::setprecision(2) << waitMs
                      << std::setw(10) << runMs << " " << threadText.str() << "\n";

            if (!record.error.empty())
                std::cout << "    error: " << record.error << "\n";
        }

        std::cout << "  graph elapsed: " << std::fixed << std::setprecision(2)
                  << ToMs(_graphStartedAt, _graphFinishedAt) << " ms\n";
    }

    void PrintCriticalPath() const
    {
        std::vector<double> longest(_records.size(), -1.0);
        std::vector<size_t> previous(_records.size(), _records.size());

        size_t endNode = _records.size();
        double longestDuration = -1.0;

        for (size_t id : _topologicalOrder)
        {
            const NodeExecutionRecord& record = _records[id];
            if (record.status != NodeStatus::Succeeded)
                continue;

            double dependencyDuration = 0.0;
            size_t predecessor = _records.size();
            for (size_t dependency : record.dependencies)
            {
                if (longest[dependency] > dependencyDuration)
                {
                    dependencyDuration = longest[dependency];
                    predecessor = dependency;
                }
            }

            const double runMs = ToMs(record.startedAt, record.finishedAt);
            longest[id] = dependencyDuration + runMs;
            previous[id] = predecessor;

            if (longest[id] > longestDuration)
            {
                longestDuration = longest[id];
                endNode = id;
            }
        }

        std::vector<size_t> path;
        while (endNode < _records.size())
        {
            path.push_back(endNode);
            endNode = previous[endNode];
        }
        std::reverse(path.begin(), path.end());

        std::cout << "  measured critical path: ";
        for (size_t i = 0; i < path.size(); ++i)
        {
            if (i > 0)
                std::cout << " -> ";
            std::cout << _records[path[i]].name;
        }
        std::cout << " (" << std::fixed << std::setprecision(2)
                  << std::max(0.0, longestDuration) << " ms)\n";
    }

private:
    TimePoint _graphStartedAt;
    TimePoint _graphFinishedAt;
    std::vector<NodeExecutionRecord> _records;
    std::vector<size_t> _topologicalOrder;
};

class TimedJobGraph
{
public:
    using JobId = size_t;

    JobId AddJob(
        std::string name,
        std::function<void()> job,
        DependencyPolicy policy = DependencyPolicy::Completion)
    {
        _nodes.push_back(Node{std::move(name), std::move(job), policy});
        return _nodes.size() - 1;
    }

    void AddDependency(JobId job, JobId dependency)
    {
        if (job >= _nodes.size() || dependency >= _nodes.size())
            throw std::invalid_argument("TimedJobGraph: invalid JobId");
        if (job == dependency)
            throw std::invalid_argument("TimedJobGraph: self dependency");

        auto& dependencies = _nodes[job].dependencies;
        if (std::find(dependencies.begin(), dependencies.end(), dependency) != dependencies.end())
            throw std::invalid_argument("TimedJobGraph: duplicate dependency");

        dependencies.push_back(dependency);
    }

    GraphExecutionReport Run(ThreadPool& pool) const
    {
        const std::vector<JobId> order = BuildTopologicalOrder();
        const TimePoint graphStartedAt = Clock::now();
        auto state = std::make_shared<ReportState>();
        state->graphStartedAt = graphStartedAt;
        state->records.resize(_nodes.size());

        for (JobId id = 0; id < _nodes.size(); ++id)
        {
            state->records[id].name = _nodes[id].name;
            state->records[id].dependencies = _nodes[id].dependencies;
        }

        std::vector<JobHandle> handles(_nodes.size());

        for (JobId id : order)
        {
            const Node& node = _nodes[id];
            std::vector<JobHandle> dependencyHandles;
            dependencyHandles.reserve(node.dependencies.size());
            for (JobId dependency : node.dependencies)
                dependencyHandles.push_back(handles[dependency]);

            auto measuredJob = [state, id, dependencies = node.dependencies, job = node.job]() mutable
            {
                const TimePoint startedAt = Clock::now();
                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    NodeExecutionRecord& record = state->records[id];
                    record.readyAt = state->graphStartedAt;
                    for (size_t dependency : dependencies)
                        record.readyAt = std::max(record.readyAt, state->records[dependency].finishedAt);
                    record.startedAt = startedAt;
                    record.threadId = std::this_thread::get_id();
                    record.status = NodeStatus::Running;
                }

                try
                {
                    job();
                }
                catch (const std::exception& e)
                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    NodeExecutionRecord& record = state->records[id];
                    record.finishedAt = Clock::now();
                    record.status = NodeStatus::Failed;
                    record.error = e.what();
                    throw;
                }
                catch (...)
                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    NodeExecutionRecord& record = state->records[id];
                    record.finishedAt = Clock::now();
                    record.status = NodeStatus::Failed;
                    record.error = "unknown exception";
                    throw;
                }

                std::lock_guard<std::mutex> lock(state->mutex);
                NodeExecutionRecord& record = state->records[id];
                record.finishedAt = Clock::now();
                record.status = NodeStatus::Succeeded;
            };

            if (dependencyHandles.empty())
            {
                handles[id] = pool.Submit(std::move(measuredJob));
            }
            else if (node.policy == DependencyPolicy::AllSucceeded)
            {
                handles[id] = pool.SubmitAfterAllSucceeded(
                    dependencyHandles,
                    std::move(measuredJob));
            }
            else
            {
                handles[id] = pool.SubmitAfter(dependencyHandles, std::move(measuredJob));
            }
        }

        for (JobId id : order)
        {
            try
            {
                handles[id].Wait();
            }
            catch (const JobCanceledException& e)
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                NodeExecutionRecord& record = state->records[id];
                if (record.status == NodeStatus::Pending)
                {
                    record.status = NodeStatus::Canceled;
                    record.readyAt = LatestDependencyFinish(*state, record.dependencies);
                    record.finishedAt = record.readyAt;
                    record.error = e.what();
                }
            }
            catch (...)
            {
                // measuredJob이 실패 상태와 메시지를 이미 기록했다.
            }
        }

        const TimePoint graphFinishedAt = Clock::now();
        std::vector<NodeExecutionRecord> records;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            records = state->records;
        }

        return GraphExecutionReport(
            graphStartedAt,
            graphFinishedAt,
            std::move(records),
            order);
    }

private:
    struct Node
    {
        std::string name;
        std::function<void()> job;
        DependencyPolicy policy;
        std::vector<JobId> dependencies;
    };

    struct ReportState
    {
        std::mutex mutex;
        TimePoint graphStartedAt{};
        std::vector<NodeExecutionRecord> records;
    };

    static TimePoint LatestDependencyFinish(
        const ReportState& state,
        const std::vector<JobId>& dependencies)
    {
        TimePoint latest = state.graphStartedAt;
        for (JobId dependency : dependencies)
            latest = std::max(latest, state.records[dependency].finishedAt);
        return latest;
    }

    std::vector<JobId> BuildTopologicalOrder() const
    {
        enum class VisitState { Unvisited, Visiting, Visited };

        std::vector<VisitState> states(_nodes.size(), VisitState::Unvisited);
        std::vector<JobId> order;
        order.reserve(_nodes.size());

        std::function<void(JobId)> visit = [&](JobId id)
        {
            if (states[id] == VisitState::Visiting)
                throw std::runtime_error("TimedJobGraph: cycle detected");
            if (states[id] == VisitState::Visited)
                return;

            states[id] = VisitState::Visiting;
            for (JobId dependency : _nodes[id].dependencies)
                visit(dependency);
            states[id] = VisitState::Visited;
            order.push_back(id);
        };

        for (JobId id = 0; id < _nodes.size(); ++id)
            visit(id);

        return order;
    }

    std::vector<Node> _nodes;
};

static void Experiment1_QueueWait()
{
    std::cout << "\n[실험 1] worker 부족으로 생기는 queue wait\n";
    std::cout << "------------------------------------------------------------\n";

    ThreadPool pool(2);
    TimedJobGraph graph;
    std::vector<TimedJobGraph::JobId> roots;

    for (int i = 0; i < 6; ++i)
    {
        roots.push_back(graph.AddJob("Root" + std::to_string(i), []
        {
            SleepMs(30);
        }));
    }

    const auto merge = graph.AddJob("Merge", [] { SleepMs(5); });
    for (auto root : roots)
        graph.AddDependency(merge, root);

    const GraphExecutionReport report = graph.Run(pool);
    report.Print();
    report.PrintCriticalPath();
    std::cout << "  핵심: 독립 작업이어도 worker 수를 넘으면 뒤쪽 작업의 queue wait가 증가한다.\n";
}

static void Experiment2_CriticalPath()
{
    std::cout << "\n[실험 2] fan-out / fan-in critical path\n";
    std::cout << "------------------------------------------------------------\n";

    ThreadPool pool(4);
    TimedJobGraph graph;

    const auto load = graph.AddJob("Load", [] { SleepMs(20); });
    const auto parse = graph.AddJob("Parse", [] { SleepMs(25); });
    const auto shaders = graph.AddJob("Shaders", [] { SleepMs(90); });
    const auto meshes = graph.AddJob("Meshes", [] { SleepMs(45); });
    const auto audio = graph.AddJob("Audio", [] { SleepMs(30); });
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

    const GraphExecutionReport report = graph.Run(pool);
    report.Print();
    report.PrintCriticalPath();
    std::cout << "  핵심: 가장 긴 노드 하나가 아니라 dependency를 따라 누적된 경로가 병목이다.\n";
}

static void Experiment3_FailureStatus()
{
    std::cout << "\n[실험 3] 실패와 취소 상태 기록\n";
    std::cout << "------------------------------------------------------------\n";

    ThreadPool pool(2);
    TimedJobGraph graph;

    const auto fetch = graph.AddJob("Fetch", []
    {
        SleepMs(10);
        throw std::runtime_error("network error");
    });
    const auto cleanup = graph.AddJob("Cleanup", [] { SleepMs(5); });
    const auto build = graph.AddJob(
        "Build",
        [] { SleepMs(5); },
        DependencyPolicy::AllSucceeded);

    graph.AddDependency(cleanup, fetch);
    graph.AddDependency(build, fetch);

    const GraphExecutionReport report = graph.Run(pool);
    report.Print();
    std::cout << "  핵심: 완료 기반 Cleanup은 실행되고, 성공 기반 Build는 canceled로 기록된다.\n";
}

int main()
{
    std::cout << "============================================================\n";
    std::cout << "  Day 19 - Graph Execution Report\n";
    std::cout << "============================================================\n";

    Experiment1_QueueWait();
    Experiment2_CriticalPath();
    Experiment3_FailureStatus();

    std::cout << "\n오늘의 핵심\n";
    std::cout << "  queue wait  : 실행 가능해진 뒤 worker를 얻기까지의 시간\n";
    std::cout << "  run time    : 실제 job 함수가 실행된 시간\n";
    std::cout << "  critical path: dependency를 따라 누적 실행 시간이 가장 긴 경로\n";
    return 0;
}
