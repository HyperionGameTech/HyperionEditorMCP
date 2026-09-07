/*!
 *  @author: The Hyperion Contributors
 *  @date 2016-2026
 *  @licence MIT
*/

#include "MCPMarshal.hpp"
#include "MCP.hpp"

#include <Core/Threading/TaskSystem.hpp>
#include <Core/Threading/Threads.hpp>
#include <Core/Threading/Scheduler.hpp>

#include <Core/Memory/SharedPtr.hpp>

namespace Hyperion {
namespace MCP {

namespace detail {

struct MCPJobRegistry
{
    Array<SharedPtr<MCPJob>> jobs;
    Mutex mutex;
    uint32 nextId = 1;
};

static MCPJobRegistry& GetJobRegistry()
{
    static MCPJobRegistry s_registry;

    return s_registry;
}

} // namespace detail

uint32 StartMCPJob(const String& name, Proc<JSON::Object()>&& fn)
{
    using namespace detail;

    MCPJobRegistry& registry = GetJobRegistry();

    SharedPtr<MCPJob> job = MakeShared<MCPJob>();
    job->name = name;
    job->status = "running";

    {
        Mutex::Guard guard(registry.mutex);
        job->id = registry.nextId++;
        registry.jobs.PushBack(job);

        // Keep the registry bounded - drop completed jobs beyond the most recent 64.
        while (registry.jobs.Size() > 64)
        {
            auto it = registry.jobs.Begin();
            while (it != registry.jobs.End() && (*it)->status == "running")
            {
                ++it;
            }

            if (it == registry.jobs.End())
            {
                break;
            }

            registry.jobs.Erase(it);
        }
    }

    TaskSystem::GetInstance().Enqueue(
        [job, fn = std::move(fn)]() mutable
        {
            JSON::Object result = fn();

            Mutex::Guard guard(job->mutex);
            job->result = std::move(result);

            if (result.Contains("error"))
            {
                job->status = "error";
            }
            else
            {
                job->status = "complete";
            }
        },
        TaskThreadPoolName::THREAD_POOL_BACKGROUND,
        TaskEnqueueFlags::FIRE_AND_FORGET);

    HYP_LOG(MCP, Info, "Started job #{}: {}", job->id, name);

    return job->id;
}

JSON::Value GetMCPJobsJson(uint32 jobId)
{
    using namespace detail;

    MCPJobRegistry& registry = GetJobRegistry();

    JSON::JArray outArray;

    Mutex::Guard guard(registry.mutex);

    for (const SharedPtr<MCPJob>& job : registry.jobs)
    {
        if (jobId != ~0u && job->id != jobId)
        {
            continue;
        }

        JSON::Object jobObject;
        jobObject.Set("id", JSON::Value(uint64(job->id)));
        jobObject.Set("name", JSON::Value(job->name));

        {
            Mutex::Guard jobGuard(job->mutex);
            jobObject.Set("status", JSON::Value(job->status));

            JSON::Object resultCopy = job->result;
            jobObject.Set("result", JSON::Value(std::move(resultCopy)));
        }

        outArray.PushBack(JSON::Value(std::move(jobObject)));
    }

    return JSON::Value(std::move(outArray));
}

} // namespace MCP
} // namespace Hyperion
