/*!
 *  @author: The Hyperion Contributors
 *  @date 2016-2026
 *  @licence MIT
*/

#include "MCPProtocol.hpp"
#include "MCPRouter.hpp"

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
        response.Set("result", MakeToolTextContent(toolErrorValue.ToString(), true));

        return response;
    }

    JSON::Object response;
    response.Set("jsonrpc", JSON::Value("2.0"));
    response.Set("id", id);
    response.Set("result", MakeToolTextContent(callResult.GetValue().ToString(), false));

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
