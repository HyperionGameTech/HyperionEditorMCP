/*!
 *  @author: The Hyperion Contributors
 *  @date 2016-2026
 *  @licence MIT
*/

#pragma once

#include "MCP.hpp"

#include <Core/Utilities/Result.hpp>

#include <Core/Functional/Proc.hpp>

#include <Core/Threading/Mutex.hpp>
#include <Core/Threading/Scheduler.hpp>
#include <Core/Threading/Task.hpp>
#include <Core/Threading/TaskSystem.hpp>
#include <Core/Threading/Threads.hpp>

#include <type_traits>

namespace Hyperion {
namespace MCP {

/*! \brief Run \p fn on the simulation thread (where all editor state lives) and block
 *  on the calling thread until it completes.
 *
 *  Uses the engine's TaskPromise idiom for cross-thread fulfillment. If the calling
 *  thread IS the simulation thread, \p fn is executed inline.
 */
template <class Func>
auto DispatchSimThread(Func&& fn) -> TResult<decltype(fn())>
{
    using ReturnType = decltype(fn());
    using ResultType = TResult<ReturnType>;

    ThreadBase* thread = GetThreadById(g_simThread);
    if (thread == nullptr)
    {
        return ResultType(HYP_MAKE_ERROR(Error, "Simulation thread is not available"));
    }

    if (IsOnThread(g_simThread))
    {
        if constexpr (std::is_void_v<ReturnType>)
        {
            fn();

            return ResultType();
        }
        else
        {
            return ResultType(fn());
        }
    }

    // Manually-fulfilled task: the sim thread fulfills the promise, we block on Await().
    Task<ResultType> task;
    TaskPromise<ResultType>* promise = task.Promise();

    thread->GetScheduler().Enqueue(
        [fn, promise]() mutable
        {
            if constexpr (std::is_void_v<ReturnType>)
            {
                fn();

                promise->Fulfill(ResultType());
            }
            else
            {
                promise->Fulfill(ResultType(fn()));
            }
        },
        TaskEnqueueFlags::FIRE_AND_FORGET);

    return task.Await();
}

/*! \brief A background job tracked by the MCP server (used for commandlets and other
 *  long-running work). Jobs are executed on the engine's background thread pool. */
struct MCPJob
{
    uint32 id;
    String name;
    String status; // "running", "complete", "error"
    JSON::Object result;
    Mutex mutex;
};

/*! \brief Start a job on the background thread pool. Returns the job id. */
uint32 StartMCPJob(const String& name, Proc<JSON::Object()>&& fn);

/*! \brief Serialize jobs (all, or just \p jobId if provided) to a JSON value. */
JSON::Value GetMCPJobsJson(uint32 jobId = ~0u);

} // namespace MCP
} // namespace Hyperion
