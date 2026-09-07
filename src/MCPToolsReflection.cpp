/*!
 *  @author: The Hyperion Contributors
 *  @date 2016-2026
 *  @licence MIT
*/

#include "MCP.hpp"
#include "MCPRouter.hpp"
#include "MCPMarshal.hpp"
#include "MCPJsonConvert.hpp"

#include <Editor/EditorState.hpp>
#include <Editor/EditorSubsystem.hpp>

#include <Asset/SerializationUtils.hpp>

#include <Scene/Scene.hpp>
#include <Scene/Node.hpp>
#include <Scene/EntityManager.hpp>

#include <Core/Reflection/Class.hpp>
#include <Core/Reflection/ClassAttribute.hpp>
#include <Core/Reflection/ClassRegistry.hpp>
#include <Core/Reflection/Field.hpp>
#include <Core/Reflection/Member.hpp>
#include <Core/Reflection/Method.hpp>
#include <Core/Reflection/ObjectBase.hpp>
#include <Core/Reflection/StaticField.hpp>
#include <Core/Reflection/TypeInfo.hpp>

#include <Core/Math/MathUtil.hpp>

#include <Framework/EngineGlobals.hpp>

namespace Hyperion {
namespace MCP {

namespace {

constexpr uint32 MaxDescribedMembers = 512;
constexpr uint32 MaxSceneNodes = 2000;

bool IsScriptBound(const IMember* member)
{
    // Mirror script binding policy: members tagged NoScriptBindings are excluded.
    const ClassAttributeValue& attribute = member->GetAttribute("noscriptbindings"_sh);

    if (!attribute.IsValid() || attribute.GetType() != ClassAttributeType::BOOLEAN)
    {
        return true;
    }

    return !attribute.GetBool();
}

String GetTypeName(const TypeInfo& typeInfo)
{
    return String(TypeInfo_GetName(typeInfo).LookupString());
}

JSON::Value MakeTargetJson(const Handle<ObjectBase>& target)
{
    if (!target.IsValid())
    {
        return JSON::Value(JSON::JSNull {});
    }

    JSON::Object targetObject;
    targetObject.Set("class", JSON::Value(target->InstanceClass()->GetName().ToString()));

    // Include name + uuid when available (generic via reflection).
    if (const Method* getName = target->InstanceClass()->GetMethod("GetName"_sh, /* deep */ true))
    {
        if (getName->GetFlags() & MethodFlags::MEMBER)
        {
            BoxedValue targetBoxed(target);
            BoxedValue boxedName = getName->Invoke(Span<BoxedValue>(&targetBoxed, 1));

            if (boxedName.Is<Name>())
            {
                targetObject.Set("name", JSON::Value(boxedName.Get<Name>().ToString()));
            }
        }
    }

    if (const Method* getUuid = target->InstanceClass()->GetMethod("GetUUID"_sh, /* deep */ true))
    {
        if (getUuid->GetFlags() & MethodFlags::MEMBER)
        {
            BoxedValue targetBoxed(target);
            BoxedValue boxedUuid = getUuid->Invoke(Span<BoxedValue>(&targetBoxed, 1));

            if (boxedUuid.Is<UUID>())
            {
                targetObject.Set("uuid", JSON::Value(boxedUuid.Get<UUID>().ToString()));
            }
        }
    }

    return JSON::Value(std::move(targetObject));
}

JSON::Value CoerceStringifiedValue(const JSON::Value& value, const TypeInfo& typeInfo)
{
    if (!value.IsString() || typeInfo.IsStringType())
    {
        return value;
    }

    const String& text = value.AsString();

    JSON::ParseResult parseResult = JSON::Parse(UTF8StringView(text.Data(), text.Data() + text.Size()));
    if (parseResult.ok && !parseResult.value.IsNullOrUndefined())
    {
        return parseResult.value;
    }

    return value;
}

TResult<BoxedValue> ResolveTargetOnSimThread(const JSON::Value& targetJson);

bool TryResolveNodeHandleFromJson(const JSON::Value& json, const TypeInfo& typeInfo, BoxedValue& outBoxed)
{
    if (!typeInfo.IsHandleType() || !json.IsObject())
    {
        return false;
    }

    const Class* handleClass = typeInfo.GetClass();
    if (handleClass == nullptr || !IsA(Node::StaticClass(), handleClass))
    {
        return false;
    }

    TResult<BoxedValue> resolved = ResolveTargetOnSimThread(json);
    if (resolved.HasError())
    {
        return false;
    }

    outBoxed = resolved.GetValue();

    return true;
}

TResult<BoxedValue> ResolveTargetOnSimThread(const JSON::Value& targetJson)
{
    if (targetJson.IsNullOrUndefined())
    {
        return TResult<BoxedValue>(HYP_MAKE_ERROR(Error, "Missing target"));
    }

    JSON::Value resolvedJson = targetJson;

    if (resolvedJson.IsString())
    {
        const String& targetText = resolvedJson.AsString();

        JSON::ParseResult parseResult = JSON::Parse(UTF8StringView(targetText.Data(), targetText.Data() + targetText.Size()));
        if (!parseResult.ok || !parseResult.value.IsObject())
        {
            return TResult<BoxedValue>(HYP_MAKE_ERROR(Error, "Target must be an object with a 'uuid' field"));
        }

        resolvedJson = parseResult.value;
    }

    if (!resolvedJson.IsObject())
    {
        return TResult<BoxedValue>(HYP_MAKE_ERROR(Error, "Target must be an object with a 'uuid' field"));
    }

    const JSON::Object& targetObject = resolvedJson.AsObject();

    auto uuidIt = targetObject.Find("uuid");
    if (uuidIt != targetObject.End() && uuidIt->second.IsString())
    {
        TResult<UUID> uuidResult = JsonToUuid(uuidIt->second);
        if (uuidResult.HasError())
        {
            return TResult<BoxedValue>(uuidResult.GetError());
        }

        Handle<EditorSubsystem> subsystem = EditorState::GetInstance()->GetEditorSubsystem();
        if (!subsystem.IsValid())
        {
            return TResult<BoxedValue>(HYP_MAKE_ERROR(Error, "No active editor subsystem; cannot resolve target by UUID"));
        }

        Handle<Scene> activeScene = subsystem->GetActiveScene();
        if (!activeScene.IsValid())
        {
            return TResult<BoxedValue>(HYP_MAKE_ERROR(Error, "No active scene; cannot resolve target by UUID"));
        }

        Node* node = activeScene->FindNodeByUUID(uuidResult.GetValue());
        if (node == nullptr)
        {
            return TResult<BoxedValue>(HYP_MAKE_ERROR(Error, "No node found with UUID '{}' in the active scene", uuidIt->second.AsString()));
        }

        return TResult<BoxedValue>(BoxedValue(MakeStrongRef(node)));
    }

    return TResult<BoxedValue>(HYP_MAKE_ERROR(Error, "Target object must contain a 'uuid' field (nodes are resolved via query_scene)"));
}

TResult<JSON::Value> HandleListClasses(const JSON::Object& args)
{
    String filter;
    bool hasFilter = false;

    auto filterIt = args.Find("filter");
    if (filterIt != args.End() && filterIt->second.IsString())
    {
        filter = filterIt->second.AsString().ToLower();
        hasFilter = true;
    }

    const Class* baseClass = nullptr;

    auto baseIt = args.Find("base");
    if (baseIt != args.End() && baseIt->second.IsString())
    {
        baseClass = ClassRegistry::GetInstance().GetClass(baseIt->second.AsString().ToAnsi(), /* ignoreCase */ true);

        if (baseClass == nullptr)
        {
            return TResult<JSON::Value>(HYP_MAKE_ERROR(Error, "No class found with name '{}'", baseIt->second.AsString()));
        }
    }

    uint32 limit = 500;

    auto limitIt = args.Find("limit");
    if (limitIt != args.End() && limitIt->second.IsNumber())
    {
        limit = limitIt->second.ToUInt32(500);
    }

    JSON::JArray classesArray;
    uint32 count = 0;

    auto collectClass = [&baseClass, &classesArray, &count, &filter, hasFilter, limit](const Class* cls) -> IterationResult
    {
        if (count >= limit)
        {
            return IterationResult::STOP;
        }

        if (baseClass != nullptr && !cls->IsDerivedFrom(baseClass))
        {
            return IterationResult::CONTINUE;
        }

        String name = cls->GetName().ToString();

        if (hasFilter && !name.ToLower().Contains(filter))
        {
            return IterationResult::CONTINUE;
        }

        JSON::Object classObject;
        classObject.Set("name", JSON::Value(name));

        if (const Class* parent = cls->GetParent())
        {
            classObject.Set("base", JSON::Value(parent->GetName().ToString()));
        }

        classObject.Set("abstract", JSON::Value(cls->IsAbstract()));

        classesArray.PushBack(JSON::Value(std::move(classObject)));
        count++;

        return IterationResult::CONTINUE;
    };

    ClassRegistry::GetInstance().ForEachClass(collectClass);

    JSON::Object result;
    result.Set("classes", JSON::Value(std::move(classesArray)));

    return TResult<JSON::Value>(std::move(result));
}

TResult<JSON::Value> HandleDescribeClass(const JSON::Object& args)
{
    auto nameIt = args.Find("name");
    if (nameIt == args.End() || !nameIt->second.IsString())
    {
        return TResult<JSON::Value>(HYP_MAKE_ERROR(Error, "describe_class requires a string 'name' argument"));
    }

    const Class* cls = ClassRegistry::GetInstance().GetClass(nameIt->second.AsString().ToAnsi(), /* ignoreCase */ true);
    if (cls == nullptr)
    {
        return TResult<JSON::Value>(HYP_MAKE_ERROR(Error, "No class found with name '{}'", nameIt->second.AsString()));
    }

    JSON::Object classObject;
    classObject.Set("name", JSON::Value(cls->GetName().ToString()));

    if (const Class* parent = cls->GetParent())
    {
        classObject.Set("base", JSON::Value(parent->GetName().ToString()));
    }

    classObject.Set("abstract", JSON::Value(cls->IsAbstract()));

    JSON::JArray methodsArray;
    uint32 memberCount = 0;

    for (const Method* method : cls->GetMethodsInherited())
    {
        if (memberCount >= MaxDescribedMembers)
        {
            break;
        }

        if (!IsScriptBound(method))
        {
            continue;
        }

        JSON::Object methodObject;
        methodObject.Set("name", JSON::Value(method->GetName().ToString()));
        methodObject.Set("static", JSON::Value(bool(method->GetFlags() & MethodFlags::STATIC)));
        methodObject.Set("returnType", JSON::Value(GetTypeName(method->GetTypeInfo())));

        JSON::JArray paramsArray;

        const Array<MethodParameter>& params = method->GetParameters();
        const bool isMember = bool(method->GetFlags() & MethodFlags::MEMBER);

        // For member methods the target ("this") parameter is the LAST entry - report only the real args.
        const size_t numArgs = isMember && params.Size() > 0 ? params.Size() - 1 : params.Size();

        for (size_t i = 0; i < numArgs; i++)
        {
            JSON::Object paramObject;
            paramObject.Set("type", JSON::Value(GetTypeName(*params[i].typeInfo)));

            paramsArray.PushBack(JSON::Value(std::move(paramObject)));
        }

        methodObject.Set("params", JSON::Value(std::move(paramsArray)));

        methodsArray.PushBack(JSON::Value(std::move(methodObject)));
        memberCount++;
    }

    classObject.Set("methods", JSON::Value(std::move(methodsArray)));

    JSON::JArray fieldsArray;

    for (const Field* field : cls->GetFieldsInherited())
    {
        if (memberCount >= MaxDescribedMembers)
        {
            break;
        }

        if (!IsScriptBound(field))
        {
            continue;
        }

        JSON::Object fieldObject;
        fieldObject.Set("name", JSON::Value(field->GetName().ToString()));
        fieldObject.Set("type", JSON::Value(GetTypeName(field->GetTypeInfo())));

        fieldsArray.PushBack(JSON::Value(std::move(fieldObject)));
        memberCount++;
    }

    classObject.Set("fields", JSON::Value(std::move(fieldsArray)));

    JSON::JArray staticFieldsArray;

    for (const StaticField* staticField : cls->GetStaticFieldsInherited())
    {
        if (memberCount >= MaxDescribedMembers)
        {
            break;
        }

        JSON::Object staticFieldObject;
        staticFieldObject.Set("name", JSON::Value(staticField->GetName().ToString()));
        staticFieldObject.Set("type", JSON::Value(GetTypeName(staticField->GetTypeInfo())));

        staticFieldsArray.PushBack(JSON::Value(std::move(staticFieldObject)));
        memberCount++;
    }

    classObject.Set("staticFields", JSON::Value(std::move(staticFieldsArray)));

    return TResult<JSON::Value>(std::move(classObject));
}

TResult<JSON::Value> HandleInvokeMethod(const JSON::Object& args)
{
    auto classIt = args.Find("class");
    if (classIt == args.End() || !classIt->second.IsString())
    {
        return TResult<JSON::Value>(HYP_MAKE_ERROR(Error, "invoke_method requires a string 'class' argument"));
    }

    auto methodIt = args.Find("method");
    if (methodIt == args.End() || !methodIt->second.IsString())
    {
        return TResult<JSON::Value>(HYP_MAKE_ERROR(Error, "invoke_method requires a string 'method' argument"));
    }

    String className = classIt->second.AsString();
    String methodName = methodIt->second.AsString();

    const Class* cls = ClassRegistry::GetInstance().GetClass(className.ToAnsi(), /* ignoreCase */ true);
    if (cls == nullptr)
    {
        return TResult<JSON::Value>(HYP_MAKE_ERROR(Error, "No class found with name '{}'", className));
    }

    Method* method = cls->GetMethod(methodName, /* deep */ true);
    if (method == nullptr)
    {
        return TResult<JSON::Value>(HYP_MAKE_ERROR(Error, "Class '{}' has no method '{}'", className, methodName));
    }

    if (!IsScriptBound(method))
    {
        return TResult<JSON::Value>(HYP_MAKE_ERROR(Error, "Method '{}' is excluded from scripting (NoScriptBindings)", methodName));
    }

    const bool isMember = bool(method->GetFlags() & MethodFlags::MEMBER);

    JSON::JArray argsJson;
    auto argsIt = args.Find("args");
    if (argsIt != args.End() && argsIt->second.IsArray())
    {
        argsJson = argsIt->second.AsArray();
    }

    JSON::Value targetJson = JSON::Value(JSON::JSUndefined {});

    auto targetIt = args.Find("target");
    if (targetIt != args.End() && !targetIt->second.IsNullOrUndefined())
    {
        targetJson = targetIt->second;
    }

    if (isMember && targetJson.IsNullOrUndefined())
    {
        return TResult<JSON::Value>(HYP_MAKE_ERROR(Error,
            "Method '{}' is a member method; pass a 'target' with a node uuid (from query_scene) or use a static method",
            methodName));
    }

    if (!isMember && !targetJson.IsNullOrUndefined())
    {
        return TResult<JSON::Value>(HYP_MAKE_ERROR(Error, "Method '{}' is static; do not pass a target", methodName));
    }

    // Argument count check (member methods take target + real args).
    const Array<MethodParameter>& params = method->GetParameters();
    const size_t numExpectedArgs = isMember && params.Size() > 0 ? params.Size() - 1 : params.Size();

    if (size_t(argsJson.Size()) != numExpectedArgs)
    {
        return TResult<JSON::Value>(HYP_MAKE_ERROR(Error,
            "Method '{}' expects {} argument(s), got {}",
            methodName, uint32(numExpectedArgs), uint32(argsJson.Size())));
    }

    // Target resolution needs the sim thread; marshal the whole call there.
    TResult<JSON::Value> dispatchResult = DispatchSimThread(
        [method, isMember, targetJson, argsJson]() -> JSON::Value
        {
            Array<BoxedValue> invokeArgs;

            if (isMember)
            {
                TResult<BoxedValue> targetResult = ResolveTargetOnSimThread(targetJson);
                if (targetResult.HasError())
                {
                    JSON::Object result;
                    result.Set("ok", JSON::Value(false));
                    result.Set("error", JSON::Value(String(targetResult.GetError().GetMessage())));

                    return JSON::Value(std::move(result));
                }

                invokeArgs.PushBack(targetResult.GetValue());
            }

            const Array<MethodParameter>& methodParams = method->GetParameters();

            for (size_t i = 0; i < size_t(argsJson.Size()); i++)
            {
                BoxedValue boxed;

                const JSON::Value& coercedJson = CoerceStringifiedValue(argsJson[i], *methodParams[i].typeInfo);

                if (!TryResolveNodeHandleFromJson(coercedJson, *methodParams[i].typeInfo, boxed))
                {
                    if (Result convertResult = BoxedFromJSON(coercedJson, *methodParams[i].typeInfo, boxed); convertResult.HasError())
                    {
                        JSON::Object result;
                        result.Set("ok", JSON::Value(false));
                        result.Set("error", JSON::Value(
                            String("Failed to convert argument ") + String::ToString(uint32(i)) + ": " + String(convertResult.GetError().GetMessage())));

                        return JSON::Value(std::move(result));
                    }
                }

                invokeArgs.PushBack(std::move(boxed));
            }

            BoxedValue result = method->Invoke(Span<BoxedValue>(invokeArgs.Data(), invokeArgs.Size()));

            JSON::Object resultObject;
            resultObject.Set("ok", JSON::Value(true));

            if (result.IsValid())
            {
                JSON::Value resultJson;
                if (BoxedToJSON(result, resultJson, nullptr))
                {
                    resultObject.Set("result", std::move(resultJson));
                }
                else
                {
                    resultObject.Set("result", JSON::Value(String("<unserializable: ") + GetTypeName(*result.GetTypeInfo()) + ">"));
                }
            }

            return JSON::Value(std::move(resultObject));
        });

    return dispatchResult;
}

TResult<JSON::Value> HandleGetField(const JSON::Object& args)
{
    auto classIt = args.Find("class");
    auto fieldIt = args.Find("field");
    auto targetIt = args.Find("target");

    if (classIt == args.End() || !classIt->second.IsString() || fieldIt == args.End() || !fieldIt->second.IsString() || targetIt == args.End())
    {
        return TResult<JSON::Value>(HYP_MAKE_ERROR(Error, "get_field requires 'class' (string), 'field' (string) and 'target' arguments"));
    }

    const Class* cls = ClassRegistry::GetInstance().GetClass(classIt->second.AsString().ToAnsi(), /* ignoreCase */ true);
    if (cls == nullptr)
    {
        return TResult<JSON::Value>(HYP_MAKE_ERROR(Error, "No class found with name '{}'", classIt->second.AsString()));
    }

    Field* field = cls->GetField(fieldIt->second.AsString(), /* deep */ true);
    if (field == nullptr)
    {
        return TResult<JSON::Value>(HYP_MAKE_ERROR(Error, "Class '{}' has no field '{}'", classIt->second.AsString(), fieldIt->second.AsString()));
    }

    JSON::Value targetJson = targetIt->second;

    return DispatchSimThread(
        [field, targetJson]() -> JSON::Value
        {
            TResult<BoxedValue> targetResult = ResolveTargetOnSimThread(targetJson);
            if (targetResult.HasError())
            {
                JSON::Object result;
                result.Set("ok", JSON::Value(false));
                result.Set("error", JSON::Value(String(targetResult.GetError().GetMessage())));

                return JSON::Value(std::move(result));
            }

            BoxedValue value = field->Get(targetResult.GetValue());

            JSON::Object resultObject;
            resultObject.Set("ok", JSON::Value(true));

            if (value.IsValid())
            {
                JSON::Value valueJson;
                if (BoxedToJSON(value, valueJson, nullptr))
                {
                    resultObject.Set("value", std::move(valueJson));
                }
            }

            return JSON::Value(std::move(resultObject));
        });
}

TResult<JSON::Value> HandleSetField(const JSON::Object& args)
{
    auto classIt = args.Find("class");
    auto fieldIt = args.Find("field");
    auto targetIt = args.Find("target");
    auto valueIt = args.Find("value");

    if (classIt == args.End() || !classIt->second.IsString() || fieldIt == args.End() || !fieldIt->second.IsString()
        || targetIt == args.End() || valueIt == args.End())
    {
        return TResult<JSON::Value>(HYP_MAKE_ERROR(Error, "set_field requires 'class' (string), 'field' (string), 'target' and 'value' arguments"));
    }

    const Class* cls = ClassRegistry::GetInstance().GetClass(classIt->second.AsString().ToAnsi(), /* ignoreCase */ true);
    if (cls == nullptr)
    {
        return TResult<JSON::Value>(HYP_MAKE_ERROR(Error, "No class found with name '{}'", classIt->second.AsString()));
    }

    Field* field = cls->GetField(fieldIt->second.AsString(), /* deep */ true);
    if (field == nullptr)
    {
        return TResult<JSON::Value>(HYP_MAKE_ERROR(Error, "Class '{}' has no field '{}'", classIt->second.AsString(), fieldIt->second.AsString()));
    }

    if (!IsScriptBound(field))
    {
        return TResult<JSON::Value>(HYP_MAKE_ERROR(Error, "Field '{}' is excluded from scripting (NoScriptBindings)", fieldIt->second.AsString()));
    }

    JSON::Value targetJson = targetIt->second;
    JSON::Value valueJson = valueIt->second;
    const TypeInfo& fieldTypeInfo = field->GetTypeInfo();

    return DispatchSimThread(
        [field, targetJson, valueJson, &fieldTypeInfo]() -> JSON::Value
        {
            TResult<BoxedValue> targetResult = ResolveTargetOnSimThread(targetJson);
            if (targetResult.HasError())
            {
                JSON::Object result;
                result.Set("ok", JSON::Value(false));
                result.Set("error", JSON::Value(String(targetResult.GetError().GetMessage())));

                return JSON::Value(std::move(result));
            }

            BoxedValue value;

            const JSON::Value& coercedValueJson = CoerceStringifiedValue(valueJson, fieldTypeInfo);

            if (!TryResolveNodeHandleFromJson(coercedValueJson, fieldTypeInfo, value))
            {
                if (Result convertResult = BoxedFromJSON(coercedValueJson, fieldTypeInfo, value); convertResult.HasError())
                {
                    JSON::Object result;
                    result.Set("ok", JSON::Value(false));
                    result.Set("error", JSON::Value(String("Failed to convert value: ") + String(convertResult.GetError().GetMessage())));

                    return JSON::Value(std::move(result));
                }
            }

            BoxedValue target = targetResult.GetValue();

            field->Set(target, value);

            JSON::Object resultObject;
            resultObject.Set("ok", JSON::Value(true));

            return JSON::Value(std::move(resultObject));
        });
}

TResult<JSON::Value> HandleCreateInstance(const JSON::Object& args)
{
    auto classIt = args.Find("class");
    if (classIt == args.End() || !classIt->second.IsString())
    {
        return TResult<JSON::Value>(HYP_MAKE_ERROR(Error, "create_instance requires a string 'class' argument"));
    }

    const Class* cls = ClassRegistry::GetInstance().GetClass(classIt->second.AsString().ToAnsi(), /* ignoreCase */ true);
    if (cls == nullptr)
    {
        return TResult<JSON::Value>(HYP_MAKE_ERROR(Error, "No class found with name '{}'", classIt->second.AsString()));
    }

    if (cls->IsAbstract() || !cls->CanCreateInstance())
    {
        return TResult<JSON::Value>(HYP_MAKE_ERROR(Error, "Class '{}' cannot be instantiated (abstract or no default construction)", cls->GetName()));
    }

    return DispatchSimThread(
        [cls]() -> JSON::Value
        {
            BoxedValue boxed;

            if (!cls->CreateInstance(boxed))
            {
                JSON::Object result;
                result.Set("ok", JSON::Value(false));
                result.Set("error", JSON::Value(String("Failed to create instance of class ") + cls->GetName().ToString()));

                return JSON::Value(std::move(result));
            }

            Handle<ObjectBase> instance;

            if (boxed.Is<Handle<ObjectBase>>())
            {
                instance = boxed.Get<Handle<ObjectBase>>();
            }

            JSON::Object resultObject;
            resultObject.Set("ok", JSON::Value(true));
            resultObject.Set("instance", MakeTargetJson(instance));

            return JSON::Value(std::move(resultObject));
        });
}

TResult<JSON::Value> HandleQueryScene(const JSON::Object& args)
{
    uint32 limit = MaxSceneNodes;

    auto limitIt = args.Find("limit");
    if (limitIt != args.End() && limitIt->second.IsNumber())
    {
        limit = limitIt->second.ToUInt32(MaxSceneNodes);
        limit = MathUtil::Min(limit, MaxSceneNodes);
    }

    return DispatchSimThread(
        [limit]() -> JSON::Value
        {
            Handle<EditorSubsystem> subsystem = EditorState::GetInstance()->GetEditorSubsystem();
            if (!subsystem.IsValid())
            {
                return JSON::Value(JSON::JSNull {});
            }

            Handle<Scene> activeScene = subsystem->GetActiveScene();
            if (!activeScene.IsValid())
            {
                JSON::Object result;
                result.Set("ok", JSON::Value(false));
                result.Set("error", JSON::Value(String("No active scene")));

                return JSON::Value(std::move(result));
            }

            JSON::JArray nodesArray;
            uint32 count = 0;

            auto addNode = [&nodesArray, &count, limit](Node* node) -> bool
            {
                if (node == nullptr || count >= limit)
                {
                    return false;
                }

                JSON::Object nodeObject;
                nodeObject.Set("uuid", JSON::Value(node->GetUUID().ToString()));
                nodeObject.Set("name", JSON::Value(node->GetName().ToString()));
                nodeObject.Set("class", JSON::Value(node->InstanceClass()->GetName().ToString()));

                if (Node* parentNode = node->GetParent(); parentNode != nullptr)
                {
                    nodeObject.Set("parent", JSON::Value(parentNode->GetUUID().ToString()));
                }
                else
                {
                    nodeObject.Set("parent", JSON::Value(JSON::JSNull {}));
                }

                nodesArray.PushBack(JSON::Value(std::move(nodeObject)));
                count++;

                return true;
            };

            Handle<Node> root = activeScene->GetRoot();

            if (root.IsValid())
            {
                addNode(root.Get());

                for (Node* node : root->GetDescendantsArray())
                {
                    if (!addNode(node))
                    {
                        break;
                    }
                }
            }

            JSON::Object result;
            result.Set("ok", JSON::Value(true));
            result.Set("scene", JSON::Value(activeScene->GetName().ToString()));
            result.Set("nodes", JSON::Value(std::move(nodesArray)));

            return JSON::Value(std::move(result));
        });
}

TResult<JSON::Value> HandleCloneNode(const JSON::Object& args)
{
    auto targetIt = args.Find("target");
    if (targetIt == args.End())
    {
        return TResult<JSON::Value>(HYP_MAKE_ERROR(Error, "clone_node requires a 'target' argument with a node uuid (from query_scene)"));
    }

    JSON::Value targetJson = targetIt->second;

    JSON::Value parentJson = JSON::Value(JSON::JSUndefined {});

    auto parentIt = args.Find("parent");
    if (parentIt != args.End() && !parentIt->second.IsNullOrUndefined())
    {
        parentJson = parentIt->second;
    }

    String cloneNameOverride;
    bool hasNameOverride = false;

    auto nameIt = args.Find("name");
    if (nameIt != args.End() && nameIt->second.IsString())
    {
        cloneNameOverride = nameIt->second.AsString();
        hasNameOverride = true;
    }

    return DispatchSimThread(
        [targetJson, parentJson, cloneNameOverride, hasNameOverride]() -> JSON::Value
        {
            TResult<BoxedValue> targetResult = ResolveTargetOnSimThread(targetJson);
            if (targetResult.HasError())
            {
                JSON::Object result;
                result.Set("ok", JSON::Value(false));
                result.Set("error", JSON::Value(String(targetResult.GetError().GetMessage())));

                return JSON::Value(std::move(result));
            }

            Handle<Node> targetNode;

            if (targetResult.GetValue().Is<Handle<Node>>())
            {
                targetNode = targetResult.GetValue().Get<Handle<Node>>();
            }

            if (!targetNode.IsValid())
            {
                JSON::Object result;
                result.Set("ok", JSON::Value(false));
                result.Set("error", JSON::Value(String("Target is not a Node")));

                return JSON::Value(std::move(result));
            }

            Handle<EditorSubsystem> subsystem = EditorState::GetInstance()->GetEditorSubsystem();
            Handle<Scene> activeScene = subsystem.IsValid() ? subsystem->GetActiveScene() : Handle<Scene>();

            if (!activeScene.IsValid())
            {
                JSON::Object result;
                result.Set("ok", JSON::Value(false));
                result.Set("error", JSON::Value(String("No active scene")));

                return JSON::Value(std::move(result));
            }

            // Only the scene root has no parent - cloning it would duplicate the entire scene.
            if (targetNode->IsRoot())
            {
                JSON::Object result;
                result.Set("ok", JSON::Value(false));
                result.Set("error", JSON::Value(String("Cannot clone the scene root")));

                return JSON::Value(std::move(result));
            }

            Handle<Node> explicitParent;

            if (!parentJson.IsNullOrUndefined())
            {
                TResult<BoxedValue> parentResult = ResolveTargetOnSimThread(parentJson);
                if (parentResult.HasError())
                {
                    JSON::Object result;
                    result.Set("ok", JSON::Value(false));
                    result.Set("error", JSON::Value(String("Failed to resolve parent: ") + String(parentResult.GetError().GetMessage())));

                    return JSON::Value(std::move(result));
                }

                if (parentResult.GetValue().Is<Handle<Node>>())
                {
                    explicitParent = parentResult.GetValue().Get<Handle<Node>>();
                }
            }

            Handle<Node> clonedNode = targetNode->Clone();

            if (!clonedNode.IsValid())
            {
                JSON::Object result;
                result.Set("ok", JSON::Value(false));
                result.Set("error", JSON::Value(String("Failed to clone node")));

                return JSON::Value(std::move(result));
            }

            Scene* targetScene = nullptr;

            ANSIString newName = hasNameOverride
                ? cloneNameOverride.ToAnsi()
                : targetNode->GetName().LookupString() + ANSIString("Copy");

            if (explicitParent.IsValid())
            {
                targetScene = explicitParent->GetScene();
                clonedNode->SetName(targetScene->GetUniqueNodeName(newName));

                explicitParent->AddChild(clonedNode);
            }
            else if (Node* originalParent = targetNode->GetParent(); originalParent != nullptr)
            {
                targetScene = originalParent->GetScene();
                clonedNode->SetName(targetScene->GetUniqueNodeName(newName));

                originalParent->AddChild(clonedNode);
            }
            else
            {
                targetScene = activeScene;
                clonedNode->SetName(targetScene->GetUniqueNodeName(newName));

                activeScene->GetRoot()->AddChild(clonedNode);
            }

            JSON::Object nodeObject;
            nodeObject.Set("uuid", JSON::Value(clonedNode->GetUUID().ToString()));
            nodeObject.Set("name", JSON::Value(clonedNode->GetName().ToString()));
            nodeObject.Set("class", JSON::Value(clonedNode->InstanceClass()->GetName().ToString()));
            nodeObject.Set("parent", JSON::Value(clonedNode->GetParent() != nullptr ? clonedNode->GetParent()->GetUUID().ToString() : String()));

            JSON::Object result;
            result.Set("ok", JSON::Value(true));
            result.Set("node", JSON::Value(std::move(nodeObject)));

            return JSON::Value(std::move(result));
        });
}

TResult<JSON::Value> HandleGetObject(const JSON::Object& args)
{
    auto targetIt = args.Find("target");
    if (targetIt == args.End())
    {
        return TResult<JSON::Value>(HYP_MAKE_ERROR(Error, "get_object requires a 'target' argument with a node uuid (from query_scene)"));
    }

    JSON::Value targetJson = targetIt->second;

    return DispatchSimThread(
        [targetJson]() -> JSON::Value
        {
            TResult<BoxedValue> targetResult = ResolveTargetOnSimThread(targetJson);
            if (targetResult.HasError())
            {
                JSON::Object result;
                result.Set("ok", JSON::Value(false));
                result.Set("error", JSON::Value(String(targetResult.GetError().GetMessage())));

                return JSON::Value(std::move(result));
            }

            BoxedValue target = targetResult.GetValue();

            const Class* cls = nullptr;

            if (target.Is<Handle<ObjectBase>>())
            {
                Handle<ObjectBase> instance = target.Get<Handle<ObjectBase>>();

                if (instance.IsValid())
                {
                    cls = instance->InstanceClass();
                }
            }

            JSON::Object objectJson;

            if (cls == nullptr || ObjectToJSON(cls, target, objectJson, nullptr).HasError())
            {
                JSON::Object result;
                result.Set("ok", JSON::Value(false));
                result.Set("error", JSON::Value(String("Failed to serialize object to JSON")));

                return JSON::Value(std::move(result));
            }

            JSON::Object result;
            result.Set("ok", JSON::Value(true));
            result.Set("object", JSON::Value(std::move(objectJson)));

            return JSON::Value(std::move(result));
        });
}

TResult<JSON::Value> HandleAddComponent(const JSON::Object& args)
{
    auto classIt = args.Find("class");
    if (classIt == args.End() || !classIt->second.IsString())
    {
        return TResult<JSON::Value>(HYP_MAKE_ERROR(Error, "add_component requires a string 'class' argument (component class name)"));
    }

    auto targetIt = args.Find("target");
    if (targetIt == args.End())
    {
        return TResult<JSON::Value>(HYP_MAKE_ERROR(Error, "add_component requires a 'target' argument"));
    }

    JSON::Value targetJson = targetIt->second;
    String componentClassName = classIt->second.AsString();

    const Class* componentClass = ClassRegistry::GetInstance().GetClass(componentClassName.ToAnsi(), /* ignoreCase */ true);
    if (componentClass == nullptr)
    {
        return TResult<JSON::Value>(HYP_MAKE_ERROR(Error, "No class found with name '{}'", componentClassName));
    }

    JSON::Object componentArgs;
    auto argsIt = args.Find("args");
    if (argsIt != args.End() && argsIt->second.IsObject())
    {
        componentArgs = argsIt->second.AsObject();
    }

    return DispatchSimThread(
        [componentClass, targetJson, componentArgs]() -> JSON::Value
        {
            TResult<BoxedValue> targetResult = ResolveTargetOnSimThread(targetJson);
            if (targetResult.HasError())
            {
                JSON::Object result;
                result.Set("ok", JSON::Value(false));
                result.Set("error", JSON::Value(String(targetResult.GetError().GetMessage())));
                return JSON::Value(std::move(result));
            }

            BoxedValue targetBoxed = targetResult.GetValue();
            Handle<Node> node;

            if (targetBoxed.Is<Handle<Node>>())
            {
                node = targetBoxed.Get<Handle<Node>>();
            }

            if (!node || !node->IsA<Entity>())
            {
                JSON::Object result;
                result.Set("ok", JSON::Value(false));
                result.Set("error", JSON::Value(String("Target is not an Entity")));
                return JSON::Value(std::move(result));
            }

            Entity* entity = StaticCast<Entity>(node.Get());

            EntityManager* entityManager = entity->GetEntityManager();
            if (entityManager == nullptr)
            {
                JSON::Object result;
                result.Set("ok", JSON::Value(false));
                result.Set("error", JSON::Value(String("Entity has no EntityManager")));
                return JSON::Value(std::move(result));
            }

            BoxedValue componentBoxed;
            if (!componentClass->CreateInstance(componentBoxed))
            {
                JSON::Object result;
                result.Set("ok", JSON::Value(false));
                result.Set("error", JSON::Value(HYP_FORMAT("Failed to create instance of component class '{}'", componentClass->GetName())));
                return JSON::Value(std::move(result));
            }

            for (auto it = componentArgs.Begin(); it != componentArgs.End(); ++it)
            {
                const String& fieldName = it->first;
                const JSON::Value& fieldValue = it->second;

                Field* field = componentClass->GetField(StringHash(fieldName));
                if (field == nullptr)
                {
                    HYP_LOG(MCP, Warning, "Component class '{}' has no field '{}', skipping", componentClass->GetName(), fieldName);
                    continue;
                }

                const TypeInfo& fieldTypeInfo = field->GetTypeInfo();
                JSON::Value coerced = CoerceStringifiedValue(fieldValue, fieldTypeInfo);

                BoxedValue fieldBoxed;
                if (Result convertResult = BoxedFromJSON(coerced, fieldTypeInfo, fieldBoxed); convertResult.HasError())
                {
                    HYP_LOG(MCP, Warning, "Failed to convert field '{}': {}", fieldName, convertResult.GetError().GetMessage());
                    continue;
                }

                field->Set(componentBoxed, fieldBoxed);
            }

            entityManager->AddComponent(entity, std::move(componentBoxed));

            JSON::Object result;
            result.Set("ok", JSON::Value(true));
            return JSON::Value(std::move(result));
        });
}


} // namespace

void RegisterReflectionMCPTools(MCPRouter& router)
{
    router.Register(MCPTool {
        "list_classes",
        "List registered reflection classes, optionally filtered by name substring and/or a base class. Use base:'Node' to discover scene node types (Entity, Camera, Light, volumes, etc.), base:'Entity' for entity types with components, or base:'Component' for ECS component types.",
        MakeToolInputSchema({ { "filter", MakeStringSchemaProperty("Case-insensitive substring filter on class name") },
                                { "base", MakeStringSchemaProperty("Only return classes derived from this class name") },
                                { "limit", MakeSchemaProperty("number") } }, {}),
        [](const JSON::Object& args)
        {
            return HandleListClasses(args);
        }
    });

    router.Register(MCPTool {
        "describe_class",
        "Describe a reflected class: methods (with param types), fields, static fields. Script-excluded members are hidden. Call this before invoke_method/set_field to get exact member names, parameter counts and types. Key classes to explore: Node (scene graph node: transform, AddChild, Clone, Translate), Entity (Node with ECS components), Scene, Camera, Light, Transform, Vec3f.",
        MakeToolInputSchema({ { "name", MakeStringSchemaProperty("Class name, e.g. 'Node' or 'LightmapVolume'") } }, { "name" }),
        [](const JSON::Object& args)
        {
            return HandleDescribeClass(args);
        }
    });

    router.Register(MCPTool {
        "invoke_method",
        "Invoke a reflected method on a class. For member methods pass target: {\"uuid\": \"...\"} - a node in the active scene from query_scene (other object types cannot be targeted); omit target for static methods. Args are positional and converted using reflection type info: Vec3f as [x, y, z]; Quat4f as [x, y, z, w]; Transform as {\"translation\": [x, y, z], \"scale\": [x, y, z], \"rotation\": [x, y, z, w]}; Name as a string. Common methods on a Node target: SetWorldTranslation([x, y, z]), SetLocalTranslation([x, y, z]), Translate([x, y, z]) (offset in local space), SetLocalRotation([x, y, z, w]), SetLocalScale([x, y, z]), SetName(\"...\"), AddChild({\"uuid\": \"...\"}) to re-parent a node. World space is left-handed, Y-up (+X right, +Y up, +Z forward; cameras look down +Z) in float world units. Use describe_class to discover methods and exact signatures.",
        MakeToolInputSchema({ { "class", MakeStringSchemaProperty("Class name") },
                                { "method", MakeStringSchemaProperty("Method name") },
                                { "target", MakeSchemaProperty("object", "Target object: {\"uuid\": \"...\"} from query_scene (omit for static methods)") },
                                { "args", MakeAnyArraySchemaProperty("Positional arguments (any JSON values)") } }, { "class", "method" }),
        [](const JSON::Object& args)
        {
            return HandleInvokeMethod(args);
        }
    });

    router.Register(MCPTool {
        "get_field",
        "Get a reflected field value on a target object: target {\"uuid\": \"...\"} (a node in the active scene, from query_scene). Example: class=Node, field=LocalTransform returns {\"translation\": [x, y, z], \"scale\": [x, y, z], \"rotation\": [x, y, z, w]}. Use describe_class to discover field names.",
        MakeToolInputSchema({ { "class", MakeStringSchemaProperty("Class name") },
                                { "field", MakeStringSchemaProperty("Field name") },
                                { "target", MakeSchemaProperty("object") } }, { "class", "field", "target" }),
        [](const JSON::Object& args)
        {
            return HandleGetField(args);
        }
    });

    router.Register(MCPTool {
        "set_field",
        "Set a reflected field value on a target object: target {\"uuid\": \"...\"} (a node in the active scene, from query_scene). Value is converted using reflection type info: Vec3f as [x, y, z]; Quat4f as [x, y, z, w]; Transform as {\"translation\": [x, y, z], \"scale\": [x, y, z], \"rotation\": [x, y, z, w]} (e.g. class=Node, field=LocalTransform). World space is left-handed, Y-up (+X right, +Y up, +Z forward) in float world units. Prefer invoke_method with Node.SetWorldTranslation / SetLocalTranslation for positioning. Use describe_class to discover field names.",
        MakeToolInputSchema({ { "class", MakeStringSchemaProperty("Class name") },
                                { "field", MakeStringSchemaProperty("Field name") },
                                { "target", MakeSchemaProperty("object", "Target object: {\"uuid\": \"...\"} from query_scene") },
                                { "value", MakeAnySchemaProperty("New value (any JSON value)") } }, { "class", "field", "target", "value" }),
        [](const JSON::Object& args)
        {
            return HandleSetField(args);
        }
    });

    router.Register(MCPTool {
        "create_instance",
        "Create a new instance of a reflected class (must be non-abstract with default construction). Note: the instance is standalone - if it is a Node it is NOT added to the scene, so it cannot be targeted by uuid (scene lookups only find attached nodes). To create scene content prefer clone_node (duplicate an existing node) or execute_editor_command (e.g. AddCube).",
        MakeToolInputSchema({ { "class", MakeStringSchemaProperty("Class name") } }, { "class" }),
        [](const JSON::Object& args)
        {
            return HandleCreateInstance(args);
        }
    });

    router.Register(MCPTool {
        "query_scene",
        "List nodes in the active editor scene as a flat hierarchy list: uuid, name, class and parent (parent node uuid; null for the root - the first entry). Node uuids are the targets for invoke_method/get_field/set_field/get_object/clone_node. Typical scene-building workflow: query_scene to discover nodes -> get_object or describe_class for details -> clone_node or execute_editor_command (e.g. AddCube) to create content -> invoke_method Node.SetWorldTranslation([x, y, z]) to place it, AddChild({\"uuid\": \"...\"}) to re-parent. World space is left-handed, Y-up (+X right, +Y up, +Z forward) in float world units.",
        MakeToolInputSchema({ { "limit", MakeSchemaProperty("number") } }, {}),
        [](const JSON::Object& args)
        {
            return HandleQueryScene(args);
        }
    });

    router.Register(MCPTool {
        "clone_node",
        "Clone a node in the active scene (deep clone: children and local transforms are preserved) and add the clone to the scene. Pass target {\"uuid\": \"...\"} (from query_scene); optionally parent {\"uuid\": \"...\"} to attach the clone under a different node (default: the source node's parent, falling back to the scene root) and name to override the clone's name (default: '<name>Copy'). Returns the new node's uuid/name/class - use the returned uuid for follow-up calls. Afterwards place it with invoke_method Node.SetWorldTranslation([x, y, z]) (world space: left-handed, Y-up, +Z forward, float world units).",
        MakeToolInputSchema({ { "target", MakeSchemaProperty("object", "Node to clone: {\"uuid\": \"...\"} from query_scene") },
                                { "parent", MakeSchemaProperty("object", "Optional new parent for the clone: {\"uuid\": \"...\"} (default: source node's parent, falling back to scene root)") },
                                { "name", MakeStringSchemaProperty("Optional name for the clone (default: '<name>Copy')") } }, { "target" }),
        [](const JSON::Object& args)
        {
            return HandleCloneNode(args);
        }
    });

    router.Register(MCPTool {
        "get_object",
        "Serialize an object (resolved via target {\"uuid\": \"...\"} from query_scene) to JSON using the engine's reflection-based serializer: all reflected fields including nested values (e.g. Node.LocalTransform as {\"translation\": [x, y, z], \"scale\": [x, y, z], \"rotation\": [x, y, z, w]}, Entity components). Good for reading the full state of a node before modifying it.",
        MakeToolInputSchema({ { "target", MakeSchemaProperty("object") } }, { "target" }),
        [](const JSON::Object& args)
        {
            return HandleGetObject(args);
        }
    });

    router.Register(MCPTool {
        "add_component",
        "Add a reflected component to an entity. Pass target {\"uuid\": \"...\"} and the component class name. Optional 'args' object sets component fields.",
        MakeToolInputSchema({ { "class", MakeStringSchemaProperty("Component class name, e.g. 'RigidBodyComponent'") },
                                { "target", MakeSchemaProperty("object", "Target entity: {\"uuid\": \"...\"} from query_scene") },
                                { "args", MakeSchemaProperty("object", "Optional field values for the component") } }, { "class", "target" }),
        [](const JSON::Object& args)
        {
            return HandleAddComponent(args);
        }
    });
}

} // namespace MCP
} // namespace Hyperion
