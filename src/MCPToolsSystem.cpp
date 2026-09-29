/*!
 *  @author: The Hyperion Contributors
 *  @date 2016-2026
 *  @licence MIT
*/

#include "MCP.hpp"
#include "MCPRouter.hpp"
#include "MCPServer.hpp"

#include <Core/Reflection/BoxedValue.hpp>

#include <Engine/Framework/CVarManager.hpp>

#include <Core/Math/MathUtil.hpp>
#include <Core/Utilities/StringUtil.hpp>

namespace Hyperion {
namespace MCP {

namespace {

double SnapshotValueToNumber(const CVarSnapshotValue& value)
{
    double number = 0.0;

    if (value.Is<float>())
    {
        number = double(value.GetUnchecked<float>());
    }
    else if (value.Is<double>())
    {
        number = value.GetUnchecked<double>();
    }
    else if (value.Is<int8>())
    {
        number = double(value.GetUnchecked<int8>());
    }
    else if (value.Is<int16>())
    {
        number = double(value.GetUnchecked<int16>());
    }
    else if (value.Is<int32>())
    {
        number = double(value.GetUnchecked<int32>());
    }
    else if (value.Is<int64>())
    {
        number = double(value.GetUnchecked<int64>());
    }
    else if (value.Is<uint8>())
    {
        number = double(value.GetUnchecked<uint8>());
    }
    else if (value.Is<uint16>())
    {
        number = double(value.GetUnchecked<uint16>());
    }
    else if (value.Is<uint32>())
    {
        number = double(value.GetUnchecked<uint32>());
    }
    else if (value.Is<uint64>())
    {
        number = double(value.GetUnchecked<uint64>());
    }

    return number;
}

JSON::Value SnapshotValueToJson(const CVarSnapshotValue& value)
{
    if (value.Is<bool>())
    {
        return JSON::Value(value.GetUnchecked<bool>());
    }

    if (value.Is<CVarString>())
    {
        const CVarString chars = value.GetUnchecked<CVarString>();

        return JSON::Value(chars != nullptr ? String(chars) : String::empty);
    }

    return JSON::Value(SnapshotValueToNumber(value));
}

// WriteToSnapshot is the only way to read a CVar's value without knowing its type. Reading it live
// (rather than from the published snapshot) also reflects a set_cvar immediately
struct CVarValueReader : CVarBase
{
    static void Read(const CVarBase* cvar, CVarSnapshotValue& outValue)
    {
        (cvar->*(&CVarValueReader::WriteToSnapshot))(outValue);
    }
};

const char* GetSnapshotValueTypeName(const CVarSnapshotValue& value)
{
    if (value.Is<bool>())
    {
        return "bool";
    }

    if (value.Is<CVarString>())
    {
        return "string";
    }

    if (value.Is<float>() || value.Is<double>())
    {
        return "float";
    }

    return value.IsValid() ? "int" : "unknown";
}

void LogSnapshotDiagnosticsOnce()
{
    static bool s_logged = false;

    if (s_logged)
    {
        return;
    }

    s_logged = true;

    const CVarManager& manager = CVarManager::GetInstance();
    const CVarSnapshot& snapshot = manager.GetCurrentSnapshot();

    int maxId = -1;

    for (CVarBase* cvar : manager.cvars)
    {
        if (cvar != nullptr)
        {
            maxId = MathUtil::Max(maxId, cvar->id);
        }
    }

    HYP_LOG(MCP, Info, "CVar snapshot diagnostics: numVars={} version={} registered={} maxId={} sizeof(CVarSnapshotValue)={} sizeof(CVarManager)={} snapshotOffset={}",
        snapshot.numVars, snapshot.version, manager.cvars.Size(), maxId,
        sizeof(CVarSnapshotValue), sizeof(CVarManager),
        uintptr_t(&snapshot) - uintptr_t(&manager));
}

JSON::Value MakeCvarJson(CVarBase* cvar)
{
    LogSnapshotDiagnosticsOnce();

    CVarSnapshotValue value;
    CVarValueReader::Read(cvar, value);

    JSON::Object cvarObject;
    cvarObject.Set("name", JSON::Value(String(cvar->name.LookupString())));
    cvarObject.Set("value", value.IsValid() ? SnapshotValueToJson(value) : JSON::Value(JSON::JSNull {}));
    cvarObject.Set("type", JSON::Value(GetSnapshotValueTypeName(value)));

    return JSON::Value(std::move(cvarObject));
}

TResult<JSON::Value> HandleListCvars(const JSON::Object&)
{
    JSON::JArray cvarsArray;

    for (CVarBase* cvar : CVarManager::GetInstance().cvars)
    {
        if (cvar == nullptr)
        {
            continue;
        }

        cvarsArray.PushBack(MakeCvarJson(cvar));
    }

    JSON::Object result;
    result.Set("cvars", JSON::Value(std::move(cvarsArray)));

    return TResult<JSON::Value>(std::move(result));
}

TResult<JSON::Value> HandleGetCvar(const JSON::Object& args)
{
    auto nameIt = args.Find("name");
    if (nameIt == args.End() || !nameIt->second.IsString())
    {
        return TResult<JSON::Value>(HYP_MAKE_ERROR(Error, "get_cvar requires a string 'name' argument"));
    }

    ANSIString name = nameIt->second.AsString().ToAnsi();

    CVarBase* cvar = CVarManager::GetInstance().FindVar(name);
    if (cvar == nullptr)
    {
        return TResult<JSON::Value>(HYP_MAKE_ERROR(Error, "No CVar found with name '{}'", name));
    }

    JSON::Object result;
    result.Set("cvar", MakeCvarJson(cvar));

    return TResult<JSON::Value>(std::move(result));
}

TResult<JSON::Value> HandleSetCvar(const JSON::Object& args)
{
    auto nameIt = args.Find("name");
    if (nameIt == args.End() || !nameIt->second.IsString())
    {
        return TResult<JSON::Value>(HYP_MAKE_ERROR(Error, "set_cvar requires a string 'name' argument"));
    }

    auto valueIt = args.Find("value");
    if (valueIt == args.End() || valueIt->second.IsNullOrUndefined())
    {
        return TResult<JSON::Value>(HYP_MAKE_ERROR(Error, "set_cvar requires a 'value' argument"));
    }

    ANSIString name = nameIt->second.AsString().ToAnsi();

    CVarBase* cvar = CVarManager::GetInstance().FindVar(name);
    if (cvar == nullptr)
    {
        return TResult<JSON::Value>(HYP_MAKE_ERROR(Error, "No CVar found with name '{}'", name));
    }

    if (!cvar->SetFromString(valueIt->second.ToString()))
    {
        return TResult<JSON::Value>(HYP_MAKE_ERROR(Error, "Failed to set CVar '{}' - value '{}' is not valid for its type", name, valueIt->second.ToString()));
    }

    JSON::Object result;
    result.Set("ok", JSON::Value(true));
    result.Set("cvar", MakeCvarJson(cvar));

    return TResult<JSON::Value>(std::move(result));
}

TResult<JSON::Value> HandleGetLogs(const JSON::Object& args)
{
    uint32 limit = 200;
    int minLevel = int(LogLevel::Info);

    auto limitIt = args.Find("limit");
    if (limitIt != args.End() && limitIt->second.IsNumber())
    {
        limit = limitIt->second.ToUInt32(200);
        limit = MathUtil::Clamp(limit, 1u, MCPMaxLogLines);
    }

    auto levelIt = args.Find("level");
    if (levelIt != args.End() && levelIt->second.IsString())
    {
        const String level = levelIt->second.AsString().ToLower();

        if (level == "verbose")
        {
            minLevel = int(LogLevel::Verbose);
        }
        else if (level == "debug")
        {
            minLevel = int(LogLevel::Debug);
        }
        else if (level == "info")
        {
            minLevel = int(LogLevel::Info);
        }
        else if (level == "warning" || level == "warn")
        {
            minLevel = int(LogLevel::Warning);
        }
        else if (level == "error")
        {
            minLevel = int(LogLevel::Error);
        }
        else if (level == "fatal")
        {
            minLevel = int(LogLevel::Fatal);
        }
    }

    String pattern;
    bool hasPattern = false;

    auto patternIt = args.Find("pattern");
    if (patternIt != args.End() && patternIt->second.IsString())
    {
        pattern = patternIt->second.AsString().ToLower();
        hasPattern = true;
    }

    Array<MCPLogLine> lines = MCPServer::GetInstance().GetLogLines(limit, minLevel);

    JSON::JArray linesArray;
    for (const MCPLogLine& line : lines)
    {
        if (hasPattern && !line.text.ToLower().Contains(pattern))
        {
            continue;
        }

        JSON::Object lineObject;
        lineObject.Set("timestamp", JSON::Value(line.timestampMs));
        lineObject.Set("channel", JSON::Value(String(line.channel)));
        lineObject.Set("text", JSON::Value(line.text));

        linesArray.PushBack(JSON::Value(std::move(lineObject)));
    }

    JSON::Object result;
    result.Set("lines", JSON::Value(std::move(linesArray)));

    return TResult<JSON::Value>(std::move(result));
}

} // namespace

void RegisterSystemMCPTools(MCPRouter& router)
{
    router.Register(MCPTool {
        "list_cvars",
        "List all registered console variables (CVars) with their current values and types.",
        MakeToolInputSchema({}, {}),
        [](const JSON::Object& args)
        {
            return HandleListCvars(args);
        }
    });

    router.Register(MCPTool {
        "get_cvar",
        "Get the current value of a single CVar by name (case-insensitive).",
        MakeToolInputSchema({ { "name", MakeStringSchemaProperty("The name of the CVar (e.g. 'Rendering.SSGI' or just 'SSGI')") } }, { "name" }),
        [](const JSON::Object& args)
        {
            return HandleGetCvar(args);
        }
    });

    router.Register(MCPTool {
        "set_cvar",
        "Set a CVar by name. Values are parsed as strings ('true'/'1', numbers, etc).",
        MakeToolInputSchema({ { "name", MakeStringSchemaProperty("The name of the CVar") },
                                { "value", MakeStringSchemaProperty("The value to set, as a string") } }, { "name", "value" }),
        [](const JSON::Object& args)
        {
            return HandleSetCvar(args);
        }
    });

    router.Register(MCPTool {
        "get_logs",
        "Get recent engine log lines captured by the MCP bridge. Optionally filter by level and a case-insensitive substring pattern.",
        MakeToolInputSchema({ { "limit", MakeSchemaProperty("number") },
                                { "level", MakeStringSchemaProperty("Minimum level: fatal | error | warning | info | verbose | debug") },
                                { "pattern", MakeStringSchemaProperty("Case-insensitive substring filter applied to the log text") } }, {}),
        [](const JSON::Object& args)
        {
            return HandleGetLogs(args);
        }
    });
}

} // namespace MCP
} // namespace Hyperion
