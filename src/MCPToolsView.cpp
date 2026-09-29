/*!
 *  @author: The Hyperion Contributors
 *  @date 2016-2026
 *  @licence MIT
*/

#include "MCP.hpp"
#include "MCPRouter.hpp"
#include "MCPMarshal.hpp"
#include "MCPJsonConvert.hpp"
#include "MCPEditorAccess.hpp"
#include "MCPViewportCapture.hpp"

#include <Editor/EditorState.hpp>
#include <Editor/EditorSubsystem.hpp>
#include <Editor/EditorViewport.hpp>

#include <Asset/SerializationUtils.hpp>

#include <Scene/World.hpp>
#include <Scene/EnvironmentSettings.hpp>
#include <Scene/Camera/Camera.hpp>

#include <System/AppContext.hpp>

#include <Core/Math/MathUtil.hpp>

#include <Framework/EngineGlobals.hpp>

#include <cstdio>

namespace Hyperion {
namespace MCP {

namespace {

constexpr uint32 DefaultCaptureMaxWidth = 1280;
constexpr uint32 DefaultSettleMsAfterCameraMove = 1000;
constexpr uint32 MaxSettleMs = 10000;

JSON::Value MakeErrorJson(const String& message)
{
    JSON::Object result;
    result.Set("ok", JSON::Value(false));
    result.Set("error", JSON::Value(message));

    return JSON::Value(std::move(result));
}

JSON::Value Vec3ToJson(const Vec3f& vector)
{
    JSON::JArray components;
    components.PushBack(JSON::Value(double(vector.x)));
    components.PushBack(JSON::Value(double(vector.y)));
    components.PushBack(JSON::Value(double(vector.z)));

    return JSON::Value(std::move(components));
}

bool JsonToVec3(const JSON::Value& json, Vec3f& outVector)
{
    if (!json.IsArray() || json.AsArray().Size() != 3)
    {
        return false;
    }

    for (uint32 i = 0; i < 3; i++)
    {
        if (!json.AsArray()[i].IsNumber())
        {
            return false;
        }

        outVector[i] = float(json.AsArray()[i].AsNumber());
    }

    return true;
}

// clients sometimes send nested objects stringified
JSON::Value ParseIfString(const JSON::Value& value)
{
    if (!value.IsString())
    {
        return value;
    }

    const String& text = value.AsString();
    JSON::ParseResult parseResult = JSON::Parse(UTF8StringView(text.Data(), text.Data() + text.Size()));

    return parseResult.ok ? parseResult.value : value;
}

#pragma region Environment settings

TResult<JSON::Value> EnvironmentSettingsToJson(const EnvironmentSettings& settings)
{
    JSON::Value settingsJson;

    if (Result result = BoxedToJSON(BoxedValue(EnvironmentSettings(settings)), settingsJson, nullptr); result.HasError())
    {
        return TResult<JSON::Value>(result.GetError());
    }

    return TResult<JSON::Value>(std::move(settingsJson));
}

JSON::Object::Iterator FindKeyIgnoreCase(JSON::Object& object, const String& key)
{
    const String lowerKey = key.ToLower();

    for (auto it = object.Begin(); it != object.End(); ++it)
    {
        if (it->first.ToLower() == lowerKey)
        {
            return it;
        }
    }

    return object.End();
}

String JoinKeys(const JSON::Object& object)
{
    String keys;

    for (const auto& member : object)
    {
        if (!keys.Empty())
        {
            keys += ", ";
        }

        keys += member.first;
    }

    return keys;
}

// merges patch into base, matching keys case-insensitively; "Exposure.Saturation" style keys address nested members.
// Unknown keys are an error rather than silently ignored, so typos don't look like they worked
bool MergeSettingsPatch(JSON::Object& base, const JSON::Object& patch, const String& path, String& outError)
{
    for (const auto& member : patch)
    {
        String key = member.first;
        JSON::Value value = member.second;

        if (key.Contains('.'))
        {
            Array<String> parts = key.Split('.');

            for (size_t i = parts.Size() - 1; i > 0; i--)
            {
                JSON::Object nested;
                nested.Set(parts[i], std::move(value));
                value = JSON::Value(std::move(nested));
            }

            key = parts[0];
        }

        auto it = FindKeyIgnoreCase(base, key);

        if (it == base.End())
        {
            outError = HYP_FORMAT("Unknown setting '{}{}'. Valid keys here: {}", path, key, JoinKeys(base));

            return false;
        }

        if (value.IsObject() && it->second.IsObject())
        {
            if (!MergeSettingsPatch(it->second.AsObject(), value.AsObject(), path + it->first + ".", outError))
            {
                return false;
            }

            continue;
        }

        it->second = std::move(value);
    }

    return true;
}

TResult<JSON::Value> HandleGetEnvironmentSettings(const JSON::Object&)
{
    return DispatchSimThread(
        []() -> JSON::Value
        {
            Handle<World> world = GetActiveEditorWorld();
            if (!world.IsValid())
            {
                return MakeErrorJson("No world is open in the editor");
            }

            TResult<JSON::Value> settingsResult = EnvironmentSettingsToJson(world->GetEnvironmentSettings());
            if (settingsResult.HasError())
            {
                return MakeErrorJson(String("Failed to serialize environment settings: ") + String(settingsResult.GetError().GetMessage()));
            }

            JSON::Object result;
            result.Set("ok", JSON::Value(true));
            result.Set("world", JSON::Value(world->GetName().ToString()));
            result.Set("settings", std::move(settingsResult.GetValue()));

            return JSON::Value(std::move(result));
        });
}

TResult<JSON::Value> HandleSetEnvironmentSettings(const JSON::Object& args)
{
    auto settingsIt = args.Find("settings");
    if (settingsIt == args.End())
    {
        return TResult<JSON::Value>(HYP_MAKE_ERROR(Error, "set_environment_settings requires a 'settings' object"));
    }

    JSON::Value patchJson = ParseIfString(settingsIt->second);
    if (!patchJson.IsObject())
    {
        return TResult<JSON::Value>(HYP_MAKE_ERROR(Error, "{}", String("'settings' must be an object, e.g. {\"Exposure\": {\"Saturation\": 1.1}}")));
    }

    return DispatchSimThread(
        [patchJson]() -> JSON::Value
        {
            Handle<World> world = GetActiveEditorWorld();
            if (!world.IsValid())
            {
                return MakeErrorJson("No world is open in the editor");
            }

            const BoxedValue currentBoxed(EnvironmentSettings(world->GetEnvironmentSettings()));

            JSON::Value mergedJson;
            if (Result result = BoxedToJSON(currentBoxed, mergedJson, nullptr); result.HasError() || !mergedJson.IsObject())
            {
                return MakeErrorJson("Failed to serialize the current environment settings");
            }

            String mergeError;
            if (!MergeSettingsPatch(mergedJson.AsObject(), patchJson.AsObject(), String::empty, mergeError))
            {
                return MakeErrorJson(mergeError);
            }

            BoxedValue newBoxed;
            if (Result result = BoxedFromJSON(mergedJson, *currentBoxed.GetTypeInfo(), newBoxed); result.HasError())
            {
                return MakeErrorJson(String("Failed to apply settings: ") + String(result.GetError().GetMessage()));
            }

            if (!newBoxed.Is<EnvironmentSettings>())
            {
                return MakeErrorJson("Settings did not convert to EnvironmentSettings");
            }

            world->SetEnvironmentSettings(newBoxed.Get<EnvironmentSettings>());
            world->MarkDirty();

            TResult<JSON::Value> appliedResult = EnvironmentSettingsToJson(world->GetEnvironmentSettings());

            JSON::Object result;
            result.Set("ok", JSON::Value(true));

            if (!appliedResult.HasError())
            {
                result.Set("settings", std::move(appliedResult.GetValue()));
            }

            return JSON::Value(std::move(result));
        });
}

#pragma endregion Environment settings

#pragma region Camera and capture

Camera* GetEditorCameraOnSimThread()
{
    Handle<EditorSubsystem> subsystem = EditorState::GetInstance()->GetEditorSubsystem();

    if (subsystem.IsValid())
    {
        if (EditorViewport* viewport = subsystem->GetActiveViewport())
        {
            if (Camera* camera = viewport->GetCamera())
            {
                return camera;
            }
        }
    }

    const Handle<EditorState>& editorState = EditorState::GetInstance();

    return editorState.IsValid() ? editorState->GetEditorCamera() : nullptr;
}

JSON::Object MakeCameraJson(const Camera* camera)
{
    JSON::Object cameraObject;
    cameraObject.Set("position", Vec3ToJson(camera->GetWorldTranslation()));
    cameraObject.Set("direction", Vec3ToJson(camera->GetDirection()));
    cameraObject.Set("fov", JSON::Value(double(camera->GetFOV())));

    return cameraObject;
}

TResult<JSON::Value> HandleGetEditorCamera(const JSON::Object&)
{
    return DispatchSimThread(
        []() -> JSON::Value
        {
            Camera* camera = GetEditorCameraOnSimThread();
            if (camera == nullptr)
            {
                return MakeErrorJson("No editor camera");
            }

            JSON::Object result;
            result.Set("ok", JSON::Value(true));
            result.Set("camera", JSON::Value(MakeCameraJson(camera)));

            return JSON::Value(std::move(result));
        });
}

JSON::Value StatValue(double value)
{
    return JSON::Value(MathUtil::Round(value * 10000.0) / 10000.0);
}

JSON::Object ComputeImageStats(const CapturedImage& image)
{
    // display-referred (sRGB-encoded) values, the same thing an image editor's histogram shows
    uint32 luminanceHistogram[256] = {};
    double sums[3] = { 0.0, 0.0, 0.0 };
    double saturationSum = 0.0;

    const size_t numPixels = size_t(image.width) * image.height;
    const ubyte* pixel = image.rgb.Data();

    for (size_t i = 0; i < numPixels; i++, pixel += 3)
    {
        const uint32 maxChannel = MathUtil::Max(pixel[0], MathUtil::Max(pixel[1], pixel[2]));
        const uint32 minChannel = MathUtil::Min(pixel[0], MathUtil::Min(pixel[1], pixel[2]));

        const uint32 luminance = MathUtil::Min(255u, uint32(0.2126f * pixel[0] + 0.7152f * pixel[1] + 0.0722f * pixel[2] + 0.5f));
        luminanceHistogram[luminance]++;

        sums[0] += pixel[0];
        sums[1] += pixel[1];
        sums[2] += pixel[2];

        saturationSum += maxChannel > 0 ? double(maxChannel - minChannel) / maxChannel : 0.0;
    }

    auto percentile = [&luminanceHistogram, numPixels](double fraction) -> double
    {
        const size_t target = size_t(fraction * double(numPixels));
        size_t accumulated = 0;

        for (uint32 bin = 0; bin < 256; bin++)
        {
            accumulated += luminanceHistogram[bin];

            if (accumulated > target)
            {
                return bin / 255.0;
            }
        }

        return 1.0;
    };

    double luminanceSum = 0.0;

    for (uint32 bin = 0; bin < 256; bin++)
    {
        luminanceSum += double(bin) * luminanceHistogram[bin];
    }

    const double pixelScale = numPixels > 0 ? 1.0 / (double(numPixels) * 255.0) : 0.0;

    JSON::JArray meanRgb;
    meanRgb.PushBack(StatValue(sums[0] * pixelScale));
    meanRgb.PushBack(StatValue(sums[1] * pixelScale));
    meanRgb.PushBack(StatValue(sums[2] * pixelScale));

    JSON::Object stats;
    stats.Set("meanLuminance", StatValue(luminanceSum * pixelScale));
    stats.Set("p5Luminance", StatValue(percentile(0.05)));
    stats.Set("p50Luminance", StatValue(percentile(0.5)));
    stats.Set("p95Luminance", StatValue(percentile(0.95)));
    stats.Set("meanRgb", JSON::Value(std::move(meanRgb)));
    stats.Set("meanSaturation", StatValue(numPixels > 0 ? saturationSum / double(numPixels) : 0.0));

    return stats;
}

TResult<JSON::Value> HandleCaptureViewport(const JSON::Object& args)
{
    Vec3f position;
    Vec3f direction;
    bool hasPosition = false;
    bool hasDirection = false;

    if (auto it = args.Find("position"); it != args.End() && !it->second.IsNullOrUndefined())
    {
        if (!JsonToVec3(ParseIfString(it->second), position))
        {
            return TResult<JSON::Value>(HYP_MAKE_ERROR(Error, "'position' must be [x, y, z]"));
        }

        hasPosition = true;
    }

    if (auto it = args.Find("direction"); it != args.End() && !it->second.IsNullOrUndefined())
    {
        if (!JsonToVec3(ParseIfString(it->second), direction) || direction.LengthSquared() <= MathUtil::epsilonF)
        {
            return TResult<JSON::Value>(HYP_MAKE_ERROR(Error, "'direction' must be a non-zero [x, y, z]"));
        }

        direction.Normalize();
        hasDirection = true;
    }

    uint32 settleMs = (hasPosition || hasDirection) ? DefaultSettleMsAfterCameraMove : 0;

    if (auto it = args.Find("settleMs"); it != args.End() && it->second.IsNumber())
    {
        settleMs = MathUtil::Min(it->second.ToUInt32(settleMs), MaxSettleMs);
    }

    uint32 maxWidth = DefaultCaptureMaxWidth;

    if (auto it = args.Find("maxWidth"); it != args.End() && it->second.IsNumber())
    {
        maxWidth = it->second.ToUInt32(maxWidth);
    }

    String savePath;

    if (auto it = args.Find("savePath"); it != args.End() && it->second.IsString())
    {
        savePath = it->second.AsString();
    }

    bool includeImage = true;

    if (auto it = args.Find("includeImage"); it != args.End() && it->second.IsBool())
    {
        includeImage = it->second.AsBool();
    }

    struct ViewportInfo
    {
        void* hwnd = nullptr;
        JSON::Object camera;
        String error;
    };

    TResult<ViewportInfo> viewportResult = DispatchSimThread(
        [hasPosition, position, hasDirection, direction]() -> ViewportInfo
        {
            ViewportInfo info;

            Handle<EditorSubsystem> subsystem = EditorState::GetInstance()->GetEditorSubsystem();
            EditorViewport* viewport = subsystem.IsValid() ? subsystem->GetActiveViewport() : nullptr;

            if (viewport == nullptr || viewport->GetWindow() == nullptr)
            {
                info.error = "No active editor viewport window";

                return info;
            }

            Camera* camera = viewport->GetCamera();

            if (camera != nullptr)
            {
                if (hasPosition)
                {
                    camera->SetWorldTranslation(position);
                }

                if (hasDirection)
                {
                    camera->SetDirection(direction);
                }

                info.camera = MakeCameraJson(camera);
            }

            info.hwnd = viewport->GetWindow()->GetHWND();

            return info;
        });

    if (viewportResult.HasError())
    {
        return TResult<JSON::Value>(viewportResult.GetError());
    }

    ViewportInfo viewportInfo = viewportResult.GetValue();

    if (!viewportInfo.error.Empty())
    {
        return TResult<JSON::Value>(HYP_MAKE_ERROR(Error, "{}", viewportInfo.error));
    }

    if (settleMs != 0)
    {
        // lets the moved camera render and TAA converge before grabbing the frame
        ThreadSleep(settleMs);
    }

    CapturedImage image;
    String captureError;

    if (!CaptureNativeWindow(viewportInfo.hwnd, image, captureError))
    {
        return TResult<JSON::Value>(HYP_MAKE_ERROR(Error, "Failed to capture viewport: {}", captureError));
    }

    const uint32 capturedWidth = image.width;
    const uint32 capturedHeight = image.height;

    DownscaleImage(image, maxWidth);

    Array<ubyte> pngBytes;

    if (!EncodePng(image, pngBytes))
    {
        return TResult<JSON::Value>(HYP_MAKE_ERROR(Error, "Failed to encode PNG"));
    }

    JSON::Object result;
    result.Set("ok", JSON::Value(true));
    result.Set("width", JSON::Value(uint64(image.width)));
    result.Set("height", JSON::Value(uint64(image.height)));
    result.Set("capturedWidth", JSON::Value(uint64(capturedWidth)));
    result.Set("capturedHeight", JSON::Value(uint64(capturedHeight)));
    result.Set("camera", JSON::Value(std::move(viewportInfo.camera)));
    result.Set("stats", JSON::Value(ComputeImageStats(image)));

    if (!savePath.Empty())
    {
        FILE* file = std::fopen(savePath.Data(), "wb");

        if (file == nullptr || std::fwrite(pngBytes.Data(), 1, pngBytes.Size(), file) != pngBytes.Size())
        {
            result.Set("saveError", JSON::Value(HYP_FORMAT("Could not write '{}'", savePath)));
        }
        else
        {
            result.Set("savedTo", JSON::Value(savePath));
        }

        if (file != nullptr)
        {
            std::fclose(file);
        }
    }

    if (includeImage)
    {
        JSON::Object imageObject;
        imageObject.Set("data", JSON::Value(EncodeBase64(pngBytes)));
        imageObject.Set("mimeType", JSON::Value("image/png"));

        result.Set(MCPImageContentKey, JSON::Value(std::move(imageObject)));
    }

    return TResult<JSON::Value>(JSON::Value(std::move(result)));
}

#pragma endregion Camera and capture

} // namespace

void RegisterViewMCPTools(MCPRouter& router)
{
    router.Register(MCPTool {
        "get_environment_settings",
        "Get the open World's environment settings: Sky (Tint, Intensity), SkyLight, Exposure (ExposureCompensation, TonemapOperator, "
        "WhiteBalanceTemperature/Tint, Saturation, Contrast, Vignette), HeightFog, Clouds, Wind, GlobalIllumination. "
        "Exposure.TonemapOperator is numeric: 0 AgX, 1 AgXPunchy, 2 ACES, 3 PBRNeutral, 4 Reinhard. "
        "These drive tonemapping, color grading, fog and sky; they live on the World, not on a scene node.",
        MakeToolInputSchema({}, {}),
        [](const JSON::Object& args)
        {
            return HandleGetEnvironmentSettings(args);
        }
    });

    router.Register(MCPTool {
        "set_environment_settings",
        "Change the open World's environment settings live (no restart needed). 'settings' is a partial patch merged into the current values: "
        "nested objects ({\"Exposure\": {\"Saturation\": 1.1}}) or dotted keys ({\"Exposure.Saturation\": 1.1}); keys are case-insensitive, "
        "unknown keys are rejected. Use the same value formats get_environment_settings returns. Marks the level dirty - save with execute_editor_command SaveLevel.",
        MakeToolInputSchema({ { "settings", MakeSchemaProperty("object", "Partial EnvironmentSettings, e.g. {\"Exposure\": {\"ExposureCompensation\": 0.3}, \"HeightFog.Density\": 0.002}") } }, { "settings" }),
        [](const JSON::Object& args)
        {
            return HandleSetEnvironmentSettings(args);
        }
    });

    router.Register(MCPTool {
        "get_editor_camera",
        "Get the active editor viewport camera: world position [x, y, z], normalized view direction [x, y, z] and FOV. "
        "Pass the position/direction back to capture_viewport to reproduce a view.",
        MakeToolInputSchema({}, {}),
        [](const JSON::Object& args)
        {
            return HandleGetEditorCamera(args);
        }
    });

    router.Register(MCPTool {
        "capture_viewport",
        "Screenshot the editor viewport as it is displayed (after tonemapping, bloom and grading) and return it as a PNG image, "
        "plus display-referred stats (mean/p5/p50/p95 luminance, mean RGB, mean saturation, all 0-1). "
        "Optionally move the camera first with position [x, y, z] and/or direction [x, y, z] (world space: left-handed, Y-up); "
        "the capture then waits settleMs (default 1000 after a move, else 0) for the frame and TAA to settle. "
        "Editor overlays (grid, stats bar, probe warnings) are included - set_cvar Editor.ShowGrid false for cleaner shots. The viewport must be visible on screen. Windows only.",
        MakeToolInputSchema({ { "position", MakeArraySchemaProperty("Optional camera world position [x, y, z]", "number") },
                                { "direction", MakeArraySchemaProperty("Optional camera view direction [x, y, z]", "number") },
                                { "settleMs", MakeSchemaProperty("number", "Milliseconds to wait before capturing (max 10000)") },
                                { "maxWidth", MakeSchemaProperty("number", "Downscale to at most this width (default 1280, 0 = full size)") },
                                { "savePath", MakeStringSchemaProperty("Optional absolute path to also write the PNG to") },
                                { "includeImage", MakeSchemaProperty("boolean", "Return the image inline (default true); false returns only stats/savedTo") } }, {}),
        [](const JSON::Object& args)
        {
            return HandleCaptureViewport(args);
        }
    });
}

} // namespace MCP
} // namespace Hyperion
