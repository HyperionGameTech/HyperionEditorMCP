/*!
 *  @author: The Hyperion Contributors
 *  @date 2016-2026
 *  @licence MIT
*/

#include "MCPProtocol.hpp"
#include "MCPRouter.hpp"
#include "MCPJsonConvert.hpp"

#include <Core/Utilities/StringUtil.hpp>

namespace Hyperion {
namespace MCP {

namespace {

constexpr const char* s_serverName = "hyperion-editor";
constexpr const char* s_serverVersion = "0.1.0";

JSON::Object MakeJsonRpcError(const JSON::Value& id, int code, const String& message)
{
    JSON::Object errorObject;
    errorObject.Set("code", JSON::Value(int64(code)));
    errorObject.Set("message", JSON::Value(message));

    JSON::Object response;
    response.Set("jsonrpc", JSON::Value("2.0"));
    response.Set("id", id);
    response.Set("error", JSON::Value(std::move(errorObject)));

    return response;
}

constexpr const char* ServerInstructions =
    "Hyperion editor bridge. Tips: "
    "Shaders (.hlsl/.hlsli under Source/Shaders) hot reload on save - the engine rechecks them every ~3 seconds, so no editor restart is needed; "
    "check get_logs with pattern 'Reloading' to confirm. "
    "World-level state (environment, exposure/tonemapping, fog, sky, clouds) lives on the World, not a scene node: "
    "use get_environment_settings / set_environment_settings, or pass target {\"world\": true} to get_field/set_field/invoke_method/get_object. "
    "Use capture_viewport to see the rendered result (optionally moving the camera first) and get_editor_camera to reproduce a view. "
    "Changes made here mark the level dirty; save with execute_editor_command SaveLevel.";

JSON::Object MakeToolTextContent(const String& text, bool isError)
{
    JSON::Object contentItem;
    contentItem.Set("type", JSON::Value("text"));
    contentItem.Set("text", JSON::Value(text));

    JSON::JArray contentArray;
    contentArray.PushBack(JSON::Value(std::move(contentItem)));

    JSON::Object result;
    result.Set("content", JSON::Value(std::move(contentArray)));

    if (isError)
    {
        result.Set("isError", JSON::Value(true));
    }

    return result;
}

// tools return images under MCPImageContentKey; they go out as MCP image content next to the remaining JSON as text
JSON::Object MakeToolResultContent(const JSON::Value& value)
{
    if (!value.IsObject() || !value.AsObject().Contains(String(MCPImageContentKey)))
    {
        return MakeToolTextContent(WriteJson(value), false);
    }

    JSON::Object textObject = value.AsObject();
    const JSON::Object imageObject = textObject.Find(String(MCPImageContentKey))->second.AsObject();
    textObject.Erase(String(MCPImageContentKey));

    JSON::Object textItem;
    textItem.Set("type", JSON::Value("text"));
    textItem.Set("text", JSON::Value(WriteJson(JSON::Value(std::move(textObject)))));

    JSON::Object imageItem;
    imageItem.Set("type", JSON::Value("image"));
    imageItem.Set("data", imageObject.Find(String("data"))->second);
    imageItem.Set("mimeType", imageObject.Find(String("mimeType"))->second);

    JSON::JArray contentArray;
    contentArray.PushBack(JSON::Value(std::move(textItem)));
    contentArray.PushBack(JSON::Value(std::move(imageItem)));

    JSON::Object result;
    result.Set("content", JSON::Value(std::move(contentArray)));

    return result;
}

JSON::Object HandleInitialize(const JSON::Value& id, const JSON::Object& params)
{
    // Echo back the client's requested protocol version if present, otherwise use ours.
    String protocolVersion = "2025-03-26";

    auto versionIt = params.Find("protocolVersion");
    if (versionIt != params.End() && versionIt->second.IsString())
    {
        protocolVersion = versionIt->second.AsString();
    }

    JSON::Object toolsCapability;
    toolsCapability.Set("listChanged", JSON::Value(false));

    JSON::Object capabilities;
    capabilities.Set("tools", JSON::Value(std::move(toolsCapability)));

    JSON::Object serverInfo;
    serverInfo.Set("name", JSON::Value(s_serverName));
    serverInfo.Set("version", JSON::Value(s_serverVersion));

    JSON::Object result;
    result.Set("protocolVersion", JSON::Value(protocolVersion));
    result.Set("capabilities", JSON::Value(std::move(capabilities)));
    result.Set("serverInfo", JSON::Value(std::move(serverInfo)));
    result.Set("instructions", JSON::Value(ServerInstructions));

    JSON::Object response;
    response.Set("jsonrpc", JSON::Value("2.0"));
    response.Set("id", id);
    response.Set("result", JSON::Value(std::move(result)));

    return response;
}

JSON::Object HandleToolsList(const JSON::Value& id)
{
    JSON::Object result;
    result.Set("tools", MCPRouter::GetInstance().MakeToolsListJson());

    JSON::Object response;
    response.Set("jsonrpc", JSON::Value("2.0"));
    response.Set("id", id);
    response.Set("result", JSON::Value(std::move(result)));

    return response;
}

JSON::Object HandleToolsCall(const JSON::Value& id, const JSON::Object& params)
{
    auto nameIt = params.Find("name");
    if (nameIt == params.End() || !nameIt->second.IsString())
    {
        return MakeJsonRpcError(id, -32602, "tools/call requires a string 'name' parameter");
    }

    String toolName = nameIt->second.AsString();

    static const JSON::Object s_emptyArguments;
    const JSON::Object* arguments = &s_emptyArguments;

    auto argsIt = params.Find("arguments");
    if (argsIt != params.End() && argsIt->second.IsObject())
    {
        arguments = &argsIt->second.AsObject();
    }

    TResult<JSON::Value> callResult = MCPRouter::GetInstance().Call(*toolName, *arguments);

    if (callResult.HasError())
    {
        // Tool-level failure: surface it as a tool error (not a protocol error) so agents can read the message.
        JSON::Object toolError;
        toolError.Set("ok", JSON::Value(false));
        toolError.Set("error", JSON::Value(String(callResult.GetError().GetMessage())));

        JSON::Value toolErrorValue(std::move(toolError));

        JSON::Object response;
        response.Set("jsonrpc", JSON::Value("2.0"));
        response.Set("id", id);
        response.Set("result", MakeToolTextContent(WriteJson(toolErrorValue), true));

        return response;
    }

    JSON::Object response;
    response.Set("jsonrpc", JSON::Value("2.0"));
    response.Set("id", id);
    response.Set("result", MakeToolResultContent(callResult.GetValue()));

    return response;
}

} // namespace

bool HandleMessage(const JSON::Object& request, JSON::Object& outResponse)
{
    auto methodIt = request.Find("method");
    if (methodIt == request.End() || !methodIt->second.IsString())
    {
        return false;
    }

    String method = methodIt->second.AsString();

    static const JSON::Value s_nullId = JSON::Value(JSON::JSNull {});
    JSON::Value id = s_nullId;

    auto idIt = request.Find("id");
    if (idIt != request.End() && !idIt->second.IsNullOrUndefined())
    {
        id = idIt->second;
    }

    const bool isNotification = (idIt == request.End() || idIt->second.IsNullOrUndefined());

    static const JSON::Object s_emptyParams;
    const JSON::Object* params = &s_emptyParams;

    auto paramsIt = request.Find("params");
    if (paramsIt != request.End() && paramsIt->second.IsObject())
    {
        params = &paramsIt->second.AsObject();
    }

    if (method == "initialize")
    {
        outResponse = HandleInitialize(id, *params);

        return true;
    }

    if (method == "notifications/initialized" || method == "notifications/cancelled")
    {
        return false;
    }

    if (method == "ping")
    {
        JSON::Object response;
        response.Set("jsonrpc", JSON::Value("2.0"));
        response.Set("id", id);
        response.Set("result", JSON::Object());

        return true;
    }

    if (method == "tools/list")
    {
        outResponse = HandleToolsList(id);

        return true;
    }

    if (method == "tools/call")
    {
        outResponse = HandleToolsCall(id, *params);

        return true;
    }

    if (isNotification)
    {
        return false;
    }

    outResponse = MakeJsonRpcError(id, -32601, String("Method not found: ") + method);

    return true;
}

} // namespace MCP
} // namespace Hyperion
