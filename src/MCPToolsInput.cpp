/*!
 *  @author: The Hyperion Contributors
 *  @date 2016-2026
 *  @licence MIT
*/

#include "MCP.hpp"
#include "MCPRouter.hpp"

#include <Engine/Framework/EngineGlobals.hpp>
#include <Engine/System/AppContext.hpp>

#include <Engine/Input/InputManager.hpp>
#include <Engine/Input/Event.hpp>
#include <Engine/Input/Keyboard.hpp>
#include <Engine/Input/Mouse.hpp>

#include <Core/Reflection/Enum.hpp>

#include <Core/Threading/Threads.hpp>
#include <Core/Threading/Scheduler.hpp>

#include <Core/Math/MathUtil.hpp>

namespace Hyperion {
namespace MCP {

namespace {

constexpr uint32 MaxHoldMs = 10000;

bool ParseKeyCode(const String& keyName, KeyCode& outKeyCode)
{
    const String upper = keyName.ToUpper();
    const String prefixed = upper.StartsWith("KEY_") ? upper : String("KEY_") + upper;

    bool found = false;

    ForEachEnumMember<KeyCode>([&](Name name, KeyCode value, bool* stopIteration)
        {
            if (String(name.LookupString()).ToUpper() == prefixed)
            {
                outKeyCode = value;
                found = true;
                *stopIteration = true;
            }
        });

    return found;
}

bool ParseMouseButton(const String& buttonName, EnumFlags<MouseButtonState>& outButtons)
{
    const String lower = buttonName.ToLower();

    if (lower == "left")
    {
        outButtons |= MouseButtonState::LEFT;
    }
    else if (lower == "right")
    {
        outButtons |= MouseButtonState::RIGHT;
    }
    else if (lower == "middle")
    {
        outButtons |= MouseButtonState::MIDDLE;
    }
    else
    {
        return false;
    }

    return true;
}

struct InjectedInput
{
    Array<KeyCode> keys;
    EnumFlags<MouseButtonState> buttons = MouseButtonState::NONE;
    Vec2f mouseDelta = Vec2f::Zero();
};

Result PostInput(const InjectedInput& input, bool pressed, bool includeMotion)
{
    ThreadBase* mainThread = GetThreadById(g_mainThread);

    if (mainThread == nullptr)
    {
        return HYP_MAKE_ERROR(Error, "Main thread is not available");
    }

    mainThread->GetScheduler().Enqueue(
        [input, pressed, includeMotion]()
        {
            ApplicationWindow* window = g_appContext.IsValid() ? g_appContext->GetMainWindow() : nullptr;

            if (window == nullptr || !window->GetInputManager().IsValid())
            {
                HYP_LOG(MCP, Warning, "send_input: no main window to send input to");
                return;
            }

            InputManager* inputManager = window->GetInputManager().Get();

            for (KeyCode key : input.keys)
            {
                Event event(pressed ? EventType::KEYDOWN : EventType::KEYUP, window, PlatformEvent {});
                event.GetEventData().Set(key);

                inputManager->ProcessEvent(std::move(event));
            }

            if (input.buttons != MouseButtonState::NONE)
            {
                Event event(pressed ? EventType::MOUSEBUTTON_DOWN : EventType::MOUSEBUTTON_UP, window, PlatformEvent {});
                event.GetEventData().Set(input.buttons);

                inputManager->ProcessEvent(std::move(event));
            }

            if (includeMotion && input.mouseDelta != Vec2f::Zero())
            {
                Event event(EventType::MOUSEMOTION, window, PlatformEvent {});
                event.GetEventData().Set(MotionData { Vec2f::Zero(), input.mouseDelta, /* isAbsolute */ false });

                inputManager->ProcessEvent(std::move(event));
            }
        },
        TaskEnqueueFlags::FIRE_AND_FORGET);

    return {};
}

TResult<JSON::Value> HandleSendInput(const JSON::Object& args)
{
    InjectedInput input;
    JSON::JArray resolvedKeys;

    if (auto keysIt = args.Find("keys"); keysIt != args.End() && keysIt->second.IsArray())
    {
        for (const JSON::Value& keyJson : keysIt->second.AsArray())
        {
            const String keyName = keyJson.ToString(false);

            KeyCode keyCode = KeyCode::KEY_UNKNOWN;

            if (!ParseKeyCode(keyName, keyCode))
            {
                return TResult<JSON::Value>(HYP_MAKE_ERROR(Error, "Unknown key '{}' (use KeyCode names, e.g. W, SPACE, LEFT, LSHIFT, ESCAPE)", keyName));
            }

            input.keys.PushBack(keyCode);
            resolvedKeys.PushBack(JSON::Value(EnumToString(keyCode)));
        }
    }

    if (auto buttonsIt = args.Find("mouseButtons"); buttonsIt != args.End() && buttonsIt->second.IsArray())
    {
        for (const JSON::Value& buttonJson : buttonsIt->second.AsArray())
        {
            if (!ParseMouseButton(buttonJson.ToString(false), input.buttons))
            {
                return TResult<JSON::Value>(HYP_MAKE_ERROR(Error, "Unknown mouse button '{}' (left, right or middle)", buttonJson.ToString(false)));
            }
        }
    }

    if (auto deltaIt = args.Find("mouseDelta"); deltaIt != args.End() && deltaIt->second.IsArray() && deltaIt->second.AsArray().Size() == 2)
    {
        const JSON::JArray& delta = deltaIt->second.AsArray();
        input.mouseDelta = Vec2f(delta[0].ToFloat(), delta[1].ToFloat());
    }

    if (input.keys.Empty() && input.buttons == MouseButtonState::NONE && input.mouseDelta == Vec2f::Zero())
    {
        return TResult<JSON::Value>(HYP_MAKE_ERROR(Error, "send_input needs at least one of 'keys', 'mouseButtons' or 'mouseDelta'"));
    }

    String action = "tap";

    if (auto actionIt = args.Find("action"); actionIt != args.End() && actionIt->second.IsString())
    {
        action = actionIt->second.AsString().ToLower();
    }

    uint32 durationMs = action == "hold" ? 500 : 50;

    if (auto durationIt = args.Find("durationMs"); durationIt != args.End() && durationIt->second.IsNumber())
    {
        durationMs = MathUtil::Min(durationIt->second.ToUInt32(durationMs), MaxHoldMs);
    }

    Result postResult;

    if (action == "down")
    {
        postResult = PostInput(input, true, true);
    }
    else if (action == "up")
    {
        postResult = PostInput(input, false, false);
    }
    else if (action == "tap" || action == "hold")
    {
        postResult = PostInput(input, true, true);

        if (!postResult.HasError())
        {
            ThreadSleep(durationMs);

            postResult = PostInput(input, false, false);
        }
    }
    else
    {
        return TResult<JSON::Value>(HYP_MAKE_ERROR(Error, "Unknown action '{}' (tap, hold, down or up)", action));
    }

    if (postResult.HasError())
    {
        return TResult<JSON::Value>(postResult.GetError());
    }

    JSON::Object result;
    result.Set("ok", JSON::Value(true));
    result.Set("action", JSON::Value(action));
    result.Set("keys", JSON::Value(std::move(resolvedKeys)));

    if (action == "tap" || action == "hold")
    {
        result.Set("durationMs", JSON::Value(durationMs));
    }

    return TResult<JSON::Value>(JSON::Value(std::move(result)));
}

} // namespace

void RegisterInputMCPTools(MCPRouter& router)
{
    router.Register(MCPTool {
        "send_input",
        "Inject keyboard / mouse input into the editor's main window, through the same InputManager path as real input: key state (InputManager::IsKeyDown) "
        "and the event queue the running game's OnInputEvent reads. Use it to drive a game while simulating (start with the Play editor command). "
        "action: 'tap' (default; down, wait durationMs (50), up), 'hold' (same, durationMs default 500, max 10000; the call blocks until released), "
        "'down' or 'up' (leave keys held across calls - always release them). Keys are KeyCode names with or without the KEY_ prefix: W, A, S, D, SPACE, "
        "LEFT, RIGHT, UP, DOWN, LSHIFT, LCTRL, RETURN, ESCAPE, 0-9. mouseDelta [dx, dy] sends one relative mouse move with the press.",
        MakeToolInputSchema({ { "keys", MakeArraySchemaProperty("Key names, e.g. [\"W\", \"LEFT\"]", "string") },
                                { "mouseButtons", MakeArraySchemaProperty("Mouse buttons: left, right, middle", "string") },
                                { "mouseDelta", MakeArraySchemaProperty("Relative mouse motion [dx, dy] in pixels", "number") },
                                { "action", MakeStringSchemaProperty("tap (default), hold, down or up") },
                                { "durationMs", MakeSchemaProperty("number", "How long tap/hold keeps the input pressed") } }),
        [](const JSON::Object& args)
        {
            return HandleSendInput(args);
        }
    });
}

} // namespace MCP
} // namespace Hyperion
