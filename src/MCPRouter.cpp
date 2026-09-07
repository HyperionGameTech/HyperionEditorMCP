/*!
 *  @author: The Hyperion Contributors
 *  @date 2016-2026
 *  @licence MIT
*/

#include "MCPRouter.hpp"

namespace Hyperion {
namespace MCP {

// Implemented in the tool translation units.
void RegisterSystemMCPTools(MCPRouter& router);
void RegisterCommandMCPTools(MCPRouter& router);
void RegisterReflectionMCPTools(MCPRouter& router);

MCPRouter& MCPRouter::GetInstance()
{
    static MCPRouter s_instance;

    static bool s_initialized = false;
    if (!s_initialized)
    {
        s_initialized = true;

        RegisterSystemMCPTools(s_instance);
        RegisterCommandMCPTools(s_instance);
        RegisterReflectionMCPTools(s_instance);
    }

    return s_instance;
}

void MCPRouter::Register(MCPTool&& tool)
{
    for (const MCPTool& existing : m_tools)
    {
        if (existing.name == tool.name)
        {
            HYP_LOG(MCP, Warning, "Tool with name '{}' is already registered; ignoring duplicate", tool.name);

            return;
        }
    }

    HYP_LOG(MCP, Info, "Registered MCP tool '{}'", tool.name);

    m_tools.PushBack(std::move(tool));
}

const MCPTool* MCPRouter::Find(UTF8StringView name) const
{
    for (const MCPTool& tool : m_tools)
    {
        if (tool.name == String(name))
        {
            return &tool;
        }
    }

    return nullptr;
}

JSON::Value MCPRouter::MakeToolsListJson() const
{
    JSON::JArray toolsArray;

    for (const MCPTool& tool : m_tools)
    {
        JSON::Object toolObject;
        toolObject.Set("name", JSON::Value(tool.name));
        toolObject.Set("description", JSON::Value(tool.description));
        toolObject.Set("inputSchema", JSON::Value(tool.inputSchema));

        toolsArray.PushBack(JSON::Value(std::move(toolObject)));
    }

    return JSON::Value(std::move(toolsArray));
}

TResult<JSON::Value> MCPRouter::Call(UTF8StringView name, const JSON::Object& arguments) const
{
    const MCPTool* tool = Find(name);
    if (tool == nullptr)
    {
        return TResult<JSON::Value>(HYP_MAKE_ERROR(Error, "Unknown tool: {}", String(name)));
    }

    return tool->handler(arguments);
}

} // namespace MCP
} // namespace Hyperion
