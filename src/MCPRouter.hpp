/*!
 *  @author: The Hyperion Contributors
 *  @date 2016-2026
 *  @licence MIT
*/

#pragma once

#include "MCP.hpp"

#include <Core/Utilities/Result.hpp>

#include <Core/Functional/Proc.hpp>

namespace Hyperion {
namespace MCP {

class MCPRouter;

/*! \brief A callable MCP tool: a name, human-readable description, a JSON-schema-ish
 *  input schema, and a handler taking the tool's `arguments` object. */
struct MCPTool
{
    String name;
    String description;
    JSON::Object inputSchema;
    Proc<TResult<JSON::Value>(const JSON::Object&)> handler;
};

/*! \brief Registry of tools + dispatch. Tool handlers are called on the MCP server
 *  thread; anything touching editor/engine state must marshal to the appropriate
 *  engine thread (see DispatchSimThread / StartMCPJob). */
class MCPRouter
{
public:
    static MCPRouter& GetInstance();

    void Register(MCPTool&& tool);

    const MCPTool* Find(UTF8StringView name) const;

    /*! \brief Serialize all registered tools for the MCP `tools/list` response. */
    JSON::Value MakeToolsListJson() const;

    /*! \brief Invoke a tool by name with the given arguments object. */
    TResult<JSON::Value> Call(UTF8StringView name, const JSON::Object& arguments) const;

private:
    Array<MCPTool> m_tools;
};

#pragma region Schema helpers

inline JSON::Value MakeSchemaProperty(const char* type)
{
    JSON::Object property;
    property.Set("type", JSON::Value(type));

    return JSON::Value(std::move(property));
}

inline JSON::Value MakeSchemaProperty(const char* type, const char* description)
{
    JSON::Object property;
    property.Set("type", JSON::Value(type));
    property.Set("description", JSON::Value(description));

    return JSON::Value(std::move(property));
}

inline JSON::Value MakeStringSchemaProperty(const char* description)
{
    JSON::Object property;
    property.Set("type", JSON::Value("string"));
    property.Set("description", JSON::Value(description));

    return JSON::Value(std::move(property));
}

inline JSON::Value MakeAnySchemaProperty(const char* description)
{
    // An empty schema is the JSON-Schema way of expressing "any value" -
    // "type": "any" is not valid JSON Schema and strict providers reject it.
    JSON::Object property;
    property.Set("description", JSON::Value(description));

    return JSON::Value(std::move(property));
}

inline JSON::Value MakeArraySchemaProperty(const char* description, const char* itemType)
{
    JSON::Object items;
    items.Set("type", JSON::Value(itemType));

    JSON::Object property;
    property.Set("type", JSON::Value("array"));
    property.Set("description", JSON::Value(description));
    property.Set("items", JSON::Value(std::move(items)));

    return JSON::Value(std::move(property));
}

inline JSON::Value MakeAnyArraySchemaProperty(const char* description)
{
    // Array with unconstrained items (any JSON value per element).
    JSON::Object items;

    JSON::Object property;
    property.Set("type", JSON::Value("array"));
    property.Set("description", JSON::Value(description));
    property.Set("items", JSON::Value(std::move(items)));

    return JSON::Value(std::move(property));
}

inline JSON::Object MakeToolInputSchema(std::initializer_list<Pair<const char*, JSON::Value>> properties, std::initializer_list<const char*> required = {})
{
    JSON::Object schema;
    schema.Set("type", JSON::Value("object"));

    JSON::Object propertiesObject;
    for (const Pair<const char*, JSON::Value>& property : properties)
    {
        propertiesObject.Set(property.first, property.second);
    }

    schema.Set("properties", JSON::Value(std::move(propertiesObject)));

    if (required.size() != 0)
    {
        JSON::JArray requiredArray;
        for (const char* name : required)
        {
            requiredArray.PushBack(JSON::Value(name));
        }

        schema.Set("required", JSON::Value(std::move(requiredArray)));
    }

    return schema;
}

#pragma endregion Schema helpers

} // namespace MCP
} // namespace Hyperion
