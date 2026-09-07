/*!
 *  @author: The Hyperion Contributors
 *  @date 2016-2026
 *  @licence MIT
*/

#include "MCP.hpp"
#include "MCPRouter.hpp"
#include "MCPServer.hpp"

#include <Engine/Plugins/PluginAPI.hpp>

namespace Hyperion {
namespace MCP {

namespace {

class MCPPlugin final : public IPlugin
{
public:
    void OnInitialize(const IPluginHost* host) override
    {
        m_host = host;

        // Touch the router so all tools register (and log) at load time
        // rather than on the first MCP request.
        MCPRouter::GetInstance();

        HYP_LOG(MCP, Info, "MCP plugin initialized (engine {}.{}.{}, plugin ABI {})",
            host->GetEngineVersionMajor(),
            host->GetEngineVersionMinor(),
            host->GetEngineVersionPatch(),
            host->GetABIVersion());
    }

    void OnEditorLaunch() override
    {
        MCPServer::GetInstance().AutoStart();
    }

    void OnEditorShutdown() override
    {
        MCPServer::GetInstance().Stop();
    }

    void OnShutdown() override
    {
        // Defensive: make sure the bridge is not serving while the host tears down.
        MCPServer::GetInstance().Stop();
    }

private:
    const IPluginHost* m_host = nullptr;
};

MCPPlugin s_plugin;

} // namespace

} // namespace MCP
} // namespace Hyperion

extern "C" HYP_PLUGIN_API Hyperion::HypPluginDescriptor* HypPluginQuery()
{
    static Hyperion::HypPluginDescriptor descriptor;
    descriptor.name = "MCPBridge";
    descriptor.version = "1.0.0";
    descriptor.abiVersion = HYP_PLUGIN_ABI_VERSION;
    descriptor.hostFlags = Hyperion::HypPluginHostFlags::Editor;

    return &descriptor;
}

extern "C" HYP_PLUGIN_API Hyperion::IPlugin* HypPluginLoad(const Hyperion::IPluginHost* host)
{
    return &Hyperion::MCP::s_plugin;
}

extern "C" HYP_PLUGIN_API void HypPluginUnload(Hyperion::IPlugin* plugin)
{
    // The plugin instance has static storage duration inside this DLL;
    // nothing to free here.
}
