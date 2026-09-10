/*!
 *  @author: The Hyperion Contributors
 *  @date 2016-2026
 *  @licence MIT
*/

#include "MCP.hpp"
#include "MCPRouter.hpp"
#include "MCPMarshal.hpp"

#include <Asset/Assets.hpp>
#include <Asset/AssetBatch.hpp>
#include <Asset/AssetObject.hpp>
#include <Asset/AssetPath.hpp>
#include <Asset/AssetRegistry.hpp>

#include <Editor/EditorState.hpp>
#include <Editor/EditorSubsystem.hpp>

#include <Framework/EngineGlobals.hpp>

#include <Core/Containers/Set.hpp>
#include <Core/Memory/SharedPtr.hpp>
#include <Core/Threading/ConditionVariable.hpp>
#include <Core/Threading/Mutex.hpp>

namespace Hyperion {
namespace MCP {

namespace {

static constexpr UTF8StringView SupportedImportExtensions[] {
    "obj", "fbx", "gltf", "glb", "mesh.xml", "skeleton.xml",
    "jpg", "jpeg", "png", "tga", "bmp", "psd", "gif", "hdr", "tif",
    "wav"
};

static constexpr uint32 ImportWaitPollMs = 250;
static constexpr uint64 ImportWaitTimeoutMs = 10ull * 60ull * 1000ull;

bool IsSupportedImportExtension(const FilePath& file)
{
    String extension = file.GetExtension().ToLower();

    if (extension == "xml")
    {
        String pathLower = String(file).ToLower();

        if (pathLower.EndsWith(".mesh.xml"))
        {
            extension = "mesh.xml";
        }
        else if (pathLower.EndsWith(".skeleton.xml"))
        {
            extension = "skeleton.xml";
        }
    }

    for (const UTF8StringView& supported : SupportedImportExtensions)
    {
        if (extension == supported)
        {
            return true;
        }
    }

    return false;
}

FilePath ResolveImportPath(const String& path)
{
    FilePath filePath(path);

    if (!filePath.IsAbsolute())
    {
        filePath = EngineGlobals::GetDataDirectory() / filePath;
    }

    return filePath.ToCanonical();
}

void CollectImportFilesFromDirectory(const FilePath& directory, bool recursive, Array<FilePath>& outFiles)
{
    for (DirectoryIterator it = directory.OpenDirectory(); it; it.Advance())
    {
        FilePath entry = it.Current();

        if (entry.IsDirectory())
        {
            if (recursive)
            {
                CollectImportFilesFromDirectory(entry, recursive, outFiles);
            }

            continue;
        }

        if (IsSupportedImportExtension(entry))
        {
            outFiles.PushBack(entry.ToCanonical());
        }
    }
}

String MakeDedupKey(const FilePath& path)
{
#if defined(HYP_WINDOWS)
    return String(path).ToLower();
#else
    return String(path);
#endif
}

struct ImportJobContext
{
    Mutex mutex;
    ConditionVariable cv;
    bool done = false;
    JSON::Object result;
};

void CompleteImportJob(const SharedPtr<ImportJobContext>& context, JSON::Object result)
{
    Mutex::Guard guard(context->mutex);

    context->result = std::move(result);
    context->done = true;

    context->cv.NotifyAll();
}

void HandleImportBatchResults(const SharedPtr<ImportJobContext>& context, AssetMap& results)
{
    JSON::JArray loadedArray;
    JSON::JArray failedArray;

    Set<uint32> changedBuckets;

    Handle<EditorSubsystem> subsystem = EditorState::GetInstance()->GetEditorSubsystem();

    for (auto& it : results)
    {
        const String& key = it.first;
        LoadedAsset& loadedAsset = it.second;

        if (!loadedAsset.IsValid())
        {
            HYP_LOG(MCP, Error, "Failed to import asset '{}': {}", key, loadedAsset.GetError().GetMessage());

            JSON::Object failedObject;
            failedObject.Set("file", JSON::Value(key));
            failedObject.Set("error", JSON::Value(String(loadedAsset.GetError().GetMessage())));

            failedArray.PushBack(JSON::Value(std::move(failedObject)));

            continue;
        }

        Handle<AssetObject> assetObject = loadedAsset.ExtractAs<AssetObject>();
        if (!assetObject.IsValid())
        {
            continue;
        }

        GetCurrentAssetRegistry()->PutAssetUnique(assetObject);

        const AssetPath& assetPath = assetObject->GetPath();

        if (assetPath.IsValid())
        {
            changedBuckets.Insert(assetPath.GetBucket().GetIndex());
        }

        JSON::Object loadedObject;
        loadedObject.Set("file", JSON::Value(key));
        loadedObject.Set("asset", JSON::Value(assetObject->GetName().ToString()));

        if (assetPath.IsValid())
        {
            loadedObject.Set("assetPath", JSON::Value(assetPath.ToString()));
        }

        loadedArray.PushBack(JSON::Value(std::move(loadedObject)));
    }

    for (uint32 bucketIndex : changedBuckets)
    {
        if (subsystem.IsValid())
        {
            subsystem->OnAssetsChanged(bucketIndex);
        }
    }

    const uint64 numLoaded = uint64(loadedArray.Size());
    const uint64 numFailed = uint64(failedArray.Size());

    JSON::Object result;
    result.Set("ok", JSON::Value(numFailed == 0));
    result.Set("numLoaded", JSON::Value(numLoaded));
    result.Set("numFailed", JSON::Value(numFailed));
    result.Set("loaded", JSON::Value(std::move(loadedArray)));
    result.Set("failed", JSON::Value(std::move(failedArray)));

    if (numLoaded == 0 && numFailed != 0)
    {
        result.Set("error", JSON::Value(HYP_FORMAT("All {} file(s) failed to import", numFailed)));
    }

    CompleteImportJob(context, std::move(result));
}

TResult<JSON::Value> HandleImportContent(const JSON::Object& args)
{
    auto pathsIt = args.Find("paths");
    if (pathsIt == args.End() || !pathsIt->second.IsArray())
    {
        return TResult<JSON::Value>(HYP_MAKE_ERROR(Error, "import_content requires a 'paths' array of file or directory paths"));
    }

    bool recursive = true;

    auto recursiveIt = args.Find("recursive");
    if (recursiveIt != args.End() && recursiveIt->second.IsBool())
    {
        recursive = recursiveIt->second.AsBool();
    }

    Array<FilePath> files;
    Array<String> missingList;
    Array<String> skippedList;
    Set<String> seenKeys;

    for (const JSON::Value& value : pathsIt->second.AsArray())
    {
        if (!value.IsString() || value.AsString().Empty())
        {
            return TResult<JSON::Value>(HYP_MAKE_ERROR(Error, "import_content 'paths' must contain non-empty strings"));
        }

        const FilePath resolved = ResolveImportPath(value.AsString());

        if (!resolved.Exists())
        {
            missingList.PushBack(String(resolved));

            continue;
        }

        if (resolved.IsDirectory())
        {
            const size_t numBefore = files.Size();

            CollectImportFilesFromDirectory(resolved, recursive, files);

            if (files.Size() == numBefore)
            {
                skippedList.PushBack(String(resolved) + " (directory contains no importable files)");
            }

            continue;
        }

        if (!IsSupportedImportExtension(resolved))
        {
            skippedList.PushBack(String(resolved) + " (unsupported format)");

            continue;
        }

        const String dedupKey = MakeDedupKey(resolved);
        if (seenKeys.Contains(dedupKey))
        {
            continue;
        }

        seenKeys.Insert(dedupKey);
        files.PushBack(resolved);
    }

    if (files.Empty())
    {
        String detail;

        for (const String& missing : missingList)
        {
            detail += HYP_FORMAT("\n  missing: {}", missing);
        }

        for (const String& skipped : skippedList)
        {
            detail += HYP_FORMAT("\n  skipped: {}", skipped);
        }

        return TResult<JSON::Value>(HYP_MAKE_ERROR(Error,
            "No importable files found ({} missing, {} skipped){}", missingList.Size(), skippedList.Size(), detail));
    }

    auto context = MakeShared<ImportJobContext>();

    // Batch identifier based on the folder of the first file (matches the
    // editor's dialog-based import).
    String identifier = files[0].BasePath().Basename();

    if (identifier.Empty())
    {
        identifier = "Unknown";
    }

    JSON::JArray filesArray;
    for (const FilePath& file : files)
    {
        filesArray.PushBack(JSON::Value(String(file)));
    }

    uint32 jobId = StartMCPJob(
        HYP_FORMAT("import_content: {} file(s)", files.Size()),
        [context, files, identifier]() -> JSON::Object
        {
            HYP_LOG(MCP, Info, "Importing {} file(s) into the project...", files.Size());

            TResult<bool> startResult = DispatchSimThread(
                [context, files, identifier]() -> bool
                {
                    if (!AssetManager::GetInstance().IsValid())
                    {
                        HYP_LOG(MCP, Error, "AssetManager is not available; cannot import content");

                        return false;
                    }

                    AssetBatch* batch = AssetManager::GetInstance()->CreateBatch(identifier);

                    for (const FilePath& file : files)
                    {
                        // Key by canonical path so same-named files in different
                        // folders don't collide in the batch results.
                        batch->Add(String(file), file.ToCanonical());
                    }

                    batch->OnComplete
                        .Bind([context](AssetMap& results) mutable
                              {
                                  HandleImportBatchResults(context, results);
                              })
                        .Detach();

                    batch->LoadAsync();

                    // The batch destroys itself via AssetManager::Update once complete.
                    return true;
                });

            if (startResult.HasError() || !startResult.GetValue())
            {
                String error = startResult.HasError()
                    ? String(startResult.GetError().GetMessage())
                    : String("AssetManager is not available; cannot import content");

                JSON::Object result;
                result.Set("ok", JSON::Value(false));
                result.Set("error", JSON::Value(std::move(error)));

                return result;
            }

            {
                Mutex::Guard guard(context->mutex);

                uint64 elapsedMs = 0;
                while (!context->done && elapsedMs < ImportWaitTimeoutMs)
                {
                    context->cv.WaitFor(context->mutex, ImportWaitPollMs);
                    elapsedMs += ImportWaitPollMs;
                }
            }

            if (!context->done)
            {
                JSON::Object result;
                result.Set("ok", JSON::Value(false));
                result.Set("error", JSON::Value(String("Timed out waiting for the import batch to complete")));

                return result;
            }

            Mutex::Guard guard(context->mutex);

            JSON::Object result = context->result;

            return result;
        });

    JSON::Object result;
    result.Set("ok", JSON::Value(true));
    result.Set("jobId", JSON::Value(uint64(jobId)));
    result.Set("numFiles", JSON::Value(uint64(files.Size())));
    result.Set("files", JSON::Value(std::move(filesArray)));

    if (missingList.Any())
    {
        JSON::JArray missingArray;
        for (const String& missing : missingList)
        {
            missingArray.PushBack(JSON::Value(missing));
        }

        result.Set("missing", JSON::Value(std::move(missingArray)));
    }

    if (skippedList.Any())
    {
        JSON::JArray skippedArray;
        for (const String& skipped : skippedList)
        {
            skippedArray.PushBack(JSON::Value(skipped));
        }

        result.Set("skipped", JSON::Value(std::move(skippedArray)));
    }

    return TResult<JSON::Value>(std::move(result));
}

} // namespace

void RegisterImportMCPTools(MCPRouter& router)
{
    router.Register(MCPTool {
        "import_content",
        "Import content files (models, textures, audio) into the open editor project without showing the file picker dialog - the path-based equivalent of the editor's Import button. Accepts files and/or directories; relative paths resolve against the project's data directory. Supported formats: obj, fbx, gltf, glb, mesh.xml, skeleton.xml, jpg, jpeg, png, tga, bmp, psd, gif, hdr, tif, wav. Runs async (returns a jobId); poll with job_status for the loaded/failed results.",
        MakeToolInputSchema({ { "paths", MakeArraySchemaProperty("File and/or directory paths to import", "string") },
                                { "recursive", MakeSchemaProperty("boolean", "When a path is a directory, include files from subdirectories (default true)") } },
            { "paths" }),
        [](const JSON::Object& args)
        {
            return HandleImportContent(args);
        }
    });
}

} // namespace MCP
} // namespace Hyperion
