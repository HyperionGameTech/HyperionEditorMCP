/*!
 *  @author: The Hyperion Contributors
 *  @date 2016-2026
 *  @licence MIT
*/

#include "MCP.hpp"
#include "MCPRouter.hpp"
#include "MCPMarshal.hpp"
#include "MCPJsonConvert.hpp"

#include <Editor/EditorCommand.hpp>
#include <Editor/EditorSubsystem.hpp>
#include <Editor/EditorState.hpp>

#include <Scene/Scene.hpp>

#include <Core/Reflection/Class.hpp>
#include <Core/Reflection/ClassRegistry.hpp>
#include <Core/Reflection/Method.hpp>

#include <Core/CLI/CommandLine.hpp>

#include <Engine/Framework/CVarManager.hpp>
#include <Framework/EngineGlobals.hpp>
#include <System/AppContext.hpp>

namespace Hyperion {
namespace MCP {

namespace {

// these commands show modal windows and should not be allowed to be called from the server
static constexpr UTF8StringView DeniedEditorCommands[] {
    "OpenProject",
    "NewProject",
    "SaveProject",
    "SaveProjectAs",
    "CloseProject",
    "ImportContent"
};

const Class* FindEditorCommandClass(UTF8StringView name)
{
    ANSIString className = ANSIString("EditorCommand") + ANSIString(name);

    return ClassRegistry::GetInstance().GetClass(className, /* ignoreCase */ true);
}

const Class* FindEditorCommandBaseClass()
{
    return ClassRegistry::GetInstance().GetClass("EditorCommandBase"_sh);
}

TResult<JSON::Value> ExecuteEditorCommandInternal(const String& name, const Array<String>& args)
{
    for (const UTF8StringView& denied : DeniedEditorCommands)
    {
        if (name == denied)
        {
            return TResult<JSON::Value>(HYP_MAKE_ERROR(Error,
                "Editor command '{}' is denied for server usage as it shows a modal window or is a destructive action.", name));
        }
    }

    const Class* commandClass = FindEditorCommandClass(name);
    if (commandClass == nullptr)
    {
        return TResult<JSON::Value>(HYP_MAKE_ERROR(Error, "No editor command found with name '{}'", name));
    }

    BoxedValue boxed;
    if (!commandClass->CreateInstance(boxed))
    {
        return TResult<JSON::Value>(HYP_MAKE_ERROR(Error, "Failed to create instance of editor command '{}'", name));
    }

    if (!boxed.Is<Handle<EditorCommandBase>>())
    {
        return TResult<JSON::Value>(HYP_MAKE_ERROR(Error, "'{}' is not an instance of EditorCommandBase", name));
    }

    Handle<EditorCommandBase> command = boxed.Get<Handle<EditorCommandBase>>();
    command->SetArguments(args);

    TResult<JSON::Value> dispatchResult = DispatchSimThread(
        [command]() -> JSON::Value
        {
            Handle<EditorSubsystem> subsystem = EditorState::GetInstance()->GetEditorSubsystem();
            if (!subsystem.IsValid())
            {
                JSON::Object result;
                result.Set("ok", JSON::Value(false));
                result.Set("error", JSON::Value(String("No active editor subsystem; cannot execute editor command")));

                return result;
            }

            command->Execute(subsystem);

            JSON::Object result;
            result.Set("ok", JSON::Value(true));
            result.Set("executed", JSON::Value(command->InstanceClass()->GetName().LookupString()));

            return result;
        });

    if (dispatchResult.HasError())
    {
        return dispatchResult;
    }

    return TResult<JSON::Value>(dispatchResult.GetValue());
}

JSON::Value MakeCommandletJson(const Class* commandletClass)
{
    String name = commandletClass->GetName().ToString();
    if (name.EndsWith("Commandlet"))
    {
        name = name.Substr(0, name.Length() - String("Commandlet").Length());
    }

    JSON::Object commandletObject;
    commandletObject.Set("name", JSON::Value(name));

    JSON::JArray argsArray;

    if (const Method* method = commandletClass->GetMethod("GetArgumentDefinitions"_sh))
    {
        Span<BoxedValue*> invokeArgs = {};
        BoxedValue boxed = method->Invoke(invokeArgs);

        if (boxed.Is<CommandLineArgumentDefinitions>())
        {
            const CommandLineArgumentDefinitions& definitions = boxed.Get<CommandLineArgumentDefinitions>();

            for (const CommandLineArgumentDefinition& definition : definitions.GetDefinitions())
            {
                JSON::Object argObject;
                argObject.Set("name", JSON::Value(definition.name));
                argObject.Set("type", JSON::Value(uint32(definition.type)));

                String typeName;
                switch (definition.type)
                {
                case CommandLineArgumentType::STRING: typeName = "string"; break;
                case CommandLineArgumentType::INTEGER: typeName = "integer"; break;
                case CommandLineArgumentType::FLOAT: typeName = "float"; break;
                case CommandLineArgumentType::BOOLEAN: typeName = "boolean"; break;
                case CommandLineArgumentType::ENUM: typeName = "enum"; break;
                }

                argObject.Set("typeName", JSON::Value(typeName));

                if (definition.shorthand.HasValue())
                {
                    argObject.Set("shorthand", JSON::Value(definition.shorthand.Get()));
                }

                if (definition.description.HasValue())
                {
                    argObject.Set("description", JSON::Value(definition.description.Get()));
                }

                argObject.Set("required", JSON::Value(definition.flags & CommandLineArgumentFlags::REQUIRED));
                argObject.Set("allowMultiple", JSON::Value(definition.flags & CommandLineArgumentFlags::ALLOW_MULTIPLE));

                if (definition.defaultValue.HasValue() && !definition.defaultValue.Get().IsNullOrUndefined())
                {
                    argObject.Set("default", definition.defaultValue.Get());
                }

                if (definition.enumValues.HasValue())
                {
                    JSON::JArray enumValuesArray;
                    for (const String& enumValue : definition.enumValues.Get())
                    {
                        enumValuesArray.PushBack(JSON::Value(enumValue));
                    }

                    argObject.Set("enumValues", JSON::Value(std::move(enumValuesArray)));
                }

                argsArray.PushBack(JSON::Value(std::move(argObject)));
            }
        }
    }

    commandletObject.Set("arguments", JSON::Value(std::move(argsArray)));

    return JSON::Value(std::move(commandletObject));
}

TResult<JSON::Value> HandleListEditorCommands(const JSON::Object&)
{
    const Class* baseClass = FindEditorCommandBaseClass();
    if (baseClass == nullptr)
    {
        return TResult<JSON::Value>(HYP_MAKE_ERROR(Error, "EditorCommandBase class is not registered"));
    }

    JSON::JArray commandsArray;

    auto collectCommand = [&baseClass, &commandsArray](const Class* cls) -> IterationResult
    {
        if (!cls->IsDerivedFrom(baseClass) || cls->IsAbstract())
        {
            return IterationResult::CONTINUE;
        }

        String name = cls->GetName().ToString();
        if (!name.StartsWith("EditorCommand"))
        {
            return IterationResult::CONTINUE;
        }

        name = name.Substr(String("EditorCommand").Length());

        JSON::Object commandObject;
        commandObject.Set("name", JSON::Value(name));

        commandsArray.PushBack(JSON::Value(std::move(commandObject)));

        return IterationResult::CONTINUE;
    };

    ClassRegistry::GetInstance().ForEachClass(collectCommand);

    JSON::Object result;
    result.Set("commands", JSON::Value(std::move(commandsArray)));

    return TResult<JSON::Value>(std::move(result));
}

TResult<JSON::Value> HandleExecuteEditorCommand(const JSON::Object& args)
{
    auto nameIt = args.Find("name");
    if (nameIt == args.End() || !nameIt->second.IsString())
    {
        return TResult<JSON::Value>(HYP_MAKE_ERROR(Error, "execute_editor_command requires a string 'name' argument"));
    }

    Array<String> commandArgs;

    auto argsIt = args.Find("args");
    if (argsIt != args.End() && argsIt->second.IsArray())
    {
        for (const JSON::Value& value : argsIt->second.AsArray())
        {
            commandArgs.PushBack(JsonValueToString(value));
        }
    }

    return ExecuteEditorCommandInternal(nameIt->second.AsString(), commandArgs);
}

TResult<JSON::Value> HandleConsoleExec(const JSON::Object& args)
{
    auto commandIt = args.Find("command");
    if (commandIt == args.End() || !commandIt->second.IsString())
    {
        return TResult<JSON::Value>(HYP_MAKE_ERROR(Error, "console_exec requires a string 'command' argument"));
    }

    Array<String> tokens = commandIt->second.AsString().Split(' ');
    Array<String> filteredTokens;
    for (const String& token : tokens)
    {
        if (token.Empty())
        {
            continue;
        }

        filteredTokens.PushBack(token.Trimmed());
    }

    if (filteredTokens.Empty())
    {
        return TResult<JSON::Value>(HYP_MAKE_ERROR(Error, "console_exec requires a non-empty 'command' argument"));
    }

    const String& commandName = filteredTokens[0];

    Array<String> commandArgs;
    for (size_t i = 1; i < filteredTokens.Size(); i++)
    {
        commandArgs.PushBack(filteredTokens[i]);
    }

    // 1) CVar?
    CVarBase* cvar = CVarManager::GetInstance().FindVar(commandName.ToAnsi());
    if (cvar != nullptr)
    {
        String valueString;
        for (size_t i = 0; i < commandArgs.Size(); i++)
        {
            if (i != 0)
            {
                valueString += ' ';
            }

            valueString += commandArgs[i];
        }

        if (!cvar->SetFromString(valueString))
        {
            return TResult<JSON::Value>(HYP_MAKE_ERROR(Error, "Failed to set CVar '{}' - value '{}' is not valid", commandName, valueString));
        }

        JSON::Object result;
        result.Set("ok", JSON::Value(true));
        result.Set("type", JSON::Value("cvar"));
        result.Set("name", JSON::Value(commandName));
        result.Set("value", JSON::Value(valueString));

        return TResult<JSON::Value>(std::move(result));
    }

    // 2) Commandlet?
    if (g_appContext.IsValid())
    {
        const Class* commandletClass = g_appContext->FindCommandletClass(commandName.ToAnsi());

        if (commandletClass != nullptr)
        {
            JSON::Value commandletJson = MakeCommandletJson(commandletClass);
            String canonicalName = commandletClass->GetName().ToString();

            uint32 jobId = StartMCPJob(
                String("console_exec: ") + commandName,
                [canonicalName, commandletJson, commandArgs]() -> JSON::Object
                {
                    // Re-fetch the class on the job thread (registry is thread-safe for lookups).
                    const Class* cls = ClassRegistry::GetInstance().GetClass(canonicalName.ToAnsi(), /* ignoreCase */ true);
                    if (cls == nullptr || !g_appContext.IsValid())
                    {
                        JSON::Object result;
                        result.Set("ok", JSON::Value(false));
                        result.Set("error", JSON::Value(String("Commandlet class disappeared before execution")));

                        return result;
                    }

                    CommandLineArgumentDefinitions definitions;
                    if (const Method* method = cls->GetMethod("GetArgumentDefinitions"_sh))
                    {
                        Span<BoxedValue*> invokeArgs = {};
                        BoxedValue boxed = method->Invoke(invokeArgs);

                        if (boxed.Is<CommandLineArgumentDefinitions>())
                        {
                            definitions = boxed.Get<CommandLineArgumentDefinitions>();
                        }
                    }

                    CommandLineArguments parsedArgs { *cls->GetName() };

                    // Tokens arrive as a flat array; accept "--key value" pairs (also "--key=value" as a single token).
                    for (size_t i = 0; i < commandArgs.Size(); i++)
                    {
                        const String& arg = commandArgs[i];

                        if (!arg.StartsWith("--"))
                        {
                            continue;
                        }

                        String key = arg.Substr(2);

                        if (key.Contains('='))
                        {
                            Array<String> parts = key.Split('=');

                            // Rejoin beyond the first '=' so values containing '=' survive.
                            String value = parts[1];
                            for (size_t partIndex = 2; partIndex < parts.Size(); partIndex++)
                            {
                                value += '=';
                                value += parts[partIndex];
                            }

                            parsedArgs.Set(parts[0], JSON::Value(std::move(value)));
                        }
                        else if (i + 1 < commandArgs.Size())
                        {
                            parsedArgs.Set(key, JSON::Value(commandArgs[i + 1]));
                            i++;
                        }
                    }

                    CommandLineParser parser { &definitions };
                    parser.ApplyDefaults(parsedArgs);

                    Result runResult = g_appContext->RunCommandlet(*cls->GetName(), parsedArgs);

                    JSON::Object result;
                    result.Set("ok", JSON::Value(!runResult.HasError()));

                    if (runResult.HasError())
                    {
                        result.Set("error", JSON::Value(String(runResult.GetError().GetMessage())));
                    }

                    return result;
                });

            JSON::Object result;
            result.Set("ok", JSON::Value(true));
            result.Set("type", JSON::Value("commandlet"));
            result.Set("name", JSON::Value(commandletJson["name"].AsString()));
            result.Set("jobId", JSON::Value(uint64(jobId)));

            return TResult<JSON::Value>(std::move(result));
        }
    }

    // 3) Editor command?
    if (FindEditorCommandClass(commandName) != nullptr)
    {
        TResult<JSON::Value> executeResult = ExecuteEditorCommandInternal(commandName, commandArgs);

        if (executeResult.HasError())
        {
            return executeResult;
        }

        JSON::Object result = executeResult.GetValue().ToObject();
        result.Set("type", JSON::Value("editorCommand"));

        return TResult<JSON::Value>(std::move(result));
    }

    return TResult<JSON::Value>(HYP_MAKE_ERROR(Error, "No CVar, commandlet, or editor command found with name '{}'", commandName));
}

TResult<JSON::Value> HandleListCommandlets(const JSON::Object&)
{
    const Class* baseClass = ClassRegistry::GetInstance().GetClass("CommandletBase"_sh);
    if (baseClass == nullptr)
    {
        return TResult<JSON::Value>(HYP_MAKE_ERROR(Error, "CommandletBase class is not registered"));
    }

    JSON::JArray commandletsArray;

    auto collectCommandlet = [&baseClass, &commandletsArray](const Class* cls) -> IterationResult
    {
        if (!cls->IsDerivedFrom(baseClass) || cls->IsAbstract())
        {
            return IterationResult::CONTINUE;
        }

        commandletsArray.PushBack(MakeCommandletJson(cls));

        return IterationResult::CONTINUE;
    };

    ClassRegistry::GetInstance().ForEachClass(collectCommandlet);

    JSON::Object result;
    result.Set("commandlets", JSON::Value(std::move(commandletsArray)));

    return TResult<JSON::Value>(std::move(result));
}

TResult<JSON::Value> HandleRunCommandlet(const JSON::Object& args)
{
    auto nameIt = args.Find("name");
    if (nameIt == args.End() || !nameIt->second.IsString())
    {
        return TResult<JSON::Value>(HYP_MAKE_ERROR(Error, "run_commandlet requires a string 'name' argument"));
    }

    if (!g_appContext.IsValid())
    {
        return TResult<JSON::Value>(HYP_MAKE_ERROR(Error, "App context is not available"));
    }

    String name = nameIt->second.AsString();

    const Class* commandletClass = g_appContext->FindCommandletClass(name.ToAnsi());
    if (commandletClass == nullptr)
    {
        return TResult<JSON::Value>(HYP_MAKE_ERROR(Error, "No commandlet found with name '{}'", name));
    }

    JSON::Value commandletJson = MakeCommandletJson(commandletClass);
    String canonicalName = commandletClass->GetName().ToString();

    JSON::Object argsObject;
    auto argsIt = args.Find("args");
    if (argsIt != args.End() && argsIt->second.IsObject())
    {
        argsObject = argsIt->second.AsObject();
    }

    uint32 jobId = StartMCPJob(
        String("run_commandlet: ") + name,
        [canonicalName, argsObject]() -> JSON::Object
        {
            const Class* cls = ClassRegistry::GetInstance().GetClass(canonicalName.ToAnsi(), /* ignoreCase */ true);
            if (cls == nullptr || !g_appContext.IsValid())
            {
                JSON::Object result;
                result.Set("ok", JSON::Value(false));
                result.Set("error", JSON::Value(String("Commandlet class disappeared before execution")));

                return result;
            }

            CommandLineArgumentDefinitions definitions;
            if (const Method* method = cls->GetMethod("GetArgumentDefinitions"_sh))
            {
                Span<BoxedValue*> invokeArgs = {};
                BoxedValue boxed = method->Invoke(invokeArgs);

                if (boxed.Is<CommandLineArgumentDefinitions>())
                {
                    definitions = boxed.Get<CommandLineArgumentDefinitions>();
                }
            }

            CommandLineArguments parsedArgs { *cls->GetName() };

            for (const auto& pair : argsObject)
            {
                JSON::Value valueCopy = pair.second;
                parsedArgs.Set(pair.first, std::move(valueCopy));
            }

            CommandLineParser parser { &definitions };
            parser.ApplyDefaults(parsedArgs);

            Result runResult = g_appContext->RunCommandlet(*cls->GetName(), parsedArgs);

            JSON::Object result;
            result.Set("ok", JSON::Value(!runResult.HasError()));

            if (runResult.HasError())
            {
                result.Set("error", JSON::Value(String(runResult.GetError().GetMessage())));
            }

            return result;
        });

    JSON::Object result;
    result.Set("ok", JSON::Value(true));
    result.Set("jobId", JSON::Value(uint64(jobId)));
    result.Set("commandlet", std::move(commandletJson));

    return TResult<JSON::Value>(std::move(result));
}

TResult<JSON::Value> HandleJobStatus(const JSON::Object& args)
{
    uint32 jobId = ~0u;

    auto idIt = args.Find("jobId");
    if (idIt != args.End() && idIt->second.IsNumber())
    {
        jobId = idIt->second.ToUInt32();
    }

    JSON::Value jobsJson = GetMCPJobsJson(jobId);

    JSON::Object result;
    result.Set("ok", JSON::Value(true));
    result.Set("jobs", std::move(jobsJson));

    return TResult<JSON::Value>(std::move(result));
}

} // namespace

void RegisterCommandMCPTools(MCPRouter& router)
{
    router.Register(MCPTool {
        "list_editor_commands",
        "List all editor commands available in the running editor (these are the same commands as the editor's command palette/console).",
        MakeToolInputSchema({}, {}),
        [](const JSON::Object& args)
        {
            return HandleListEditorCommands(args);
        }
    });

    router.Register(MCPTool {
        "execute_editor_command",
        "Execute an editor command by name in the running editor. Runs on the simulation thread, same as the editor console.",
        MakeToolInputSchema({ { "name", MakeStringSchemaProperty("Editor command name, e.g. 'BuildLightmaps', 'AddCube', 'CookGameContent'") },
                                { "args", MakeArraySchemaProperty("Optional command arguments", "string") } }, { "name" }),
        [](const JSON::Object& args)
        {
            return HandleExecuteEditorCommand(args);
        }
    });

    router.Register(MCPTool {
        "console_exec",
        "Execute a string via the engine console pipeline: sets a CVar, runs a commandlet, or executes an editor command - resolved in that order. Commandlets run async (returns a jobId).",
        MakeToolInputSchema({ { "command", MakeStringSchemaProperty("Command line, e.g. 'ssgi 1' or 'PrecompileShaders'") } }, { "command" }),
        [](const JSON::Object& args)
        {
            return HandleConsoleExec(args);
        }
    });

    router.Register(MCPTool {
        "list_commandlets",
        "List all commandlets (headless engine tasks) with their argument definitions.",
        MakeToolInputSchema({}, {}),
        [](const JSON::Object& args)
        {
            return HandleListCommandlets(args);
        }
    });

    router.Register(MCPTool {
        "run_commandlet",
        "Run a commandlet in-process on a background thread. Returns a jobId immediately; poll with job_status. Pass args as a JSON object keyed by argument name.",
        MakeToolInputSchema({ { "name", MakeStringSchemaProperty("Commandlet name, e.g. 'PrecompileShaders'") },
                                { "args", MakeSchemaProperty("object") } }, { "name" }),
        [](const JSON::Object& args)
        {
            return HandleRunCommandlet(args);
        }
    });

    router.Register(MCPTool {
        "job_status",
        "Get status/result of MCP background jobs (commandlets etc). Pass jobId for a single job, or omit for all recent jobs.",
        MakeToolInputSchema({ { "jobId", MakeSchemaProperty("number") } }, {}),
        [](const JSON::Object& args)
        {
            return HandleJobStatus(args);
        }
    });
}

} // namespace MCP
} // namespace Hyperion
