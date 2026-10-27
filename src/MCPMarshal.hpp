/*!
 *  @author: The Hyperion Contributors
 *  @date 2016-2026
 *  @licence MIT
*/

#pragma once

#include "MCP.hpp"

#include <Core/Utilities/Result.hpp>

#include <Core/Functional/Proc.hpp>

#include <Core/Memory/SharedPtr.hpp>

#include <Core/Threading/ConditionVariable.hpp>
#include <Core/Threading/Mutex.hpp>
#include <Core/Threading/Scheduler.hpp>
#include <Core/Threading/Task.hpp>
#include <Core/Threading/TaskSystem.hpp>
#include <Core/Threading/Threads.hpp>

#include <chrono>
#include <type_traits>

namespace Hyperion {
namespace MCP {

/*! \brief How long a request thread waits for the simulation thread before giving up. */
static constexpr uint32 SimThreadDispatchTimeoutMs = 30 * 1000;

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

    // Shared with the sim thread's closure, which may outlive this call if we time out.
    struct DispatchState
    {
        Mutex mutex;
        ConditionVariable cv;
        bool done = false;
        bool abandoned = false;
        ResultType result { HYP_MAKE_ERROR(Error, "Simulation thread call did not complete") };
    };

    SharedPtr<DispatchState> state = MakeShared<DispatchState>();

    thread->GetScheduler().Enqueue(
        [fn, state]() mutable
        {
            {
                // timed out before it started: don't run a call nobody is waiting for
                Mutex::Guard guard(state->mutex);

                if (state->abandoned)
                {
                    return;
                }
            }

            ResultType result = [&fn]() -> ResultType
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
            }();

            Mutex::Guard guard(state->mutex);

            state->result = std::move(result);
            state->done = true;

            state->cv.NotifyAll();
        },
        TaskEnqueueFlags::FIRE_AND_FORGET);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(SimThreadDispatchTimeoutMs);

    Mutex::Guard guard(state->mutex);

    while (!state->done)
    {
        if (std::chrono::steady_clock::now() >= deadline)
        {
            state->abandoned = true;

            return ResultType(HYP_MAKE_ERROR(Error, "Timed out waiting for the simulation thread (it may be busy or shutting down)"));
        }

        state->cv.WaitFor(state->mutex, 100);
    }

    return std::move(state->result);
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
