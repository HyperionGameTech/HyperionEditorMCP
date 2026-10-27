/*!
 *  @author: The Hyperion Contributors
 *  @date 2016-2026
 *  @licence MIT
*/

#include "MCP.hpp"
#include "MCPRouter.hpp"
#include "MCPServer.hpp"
#include "MCPEditorAccess.hpp"

#include <Engine/Plugins/PluginModule.hpp>
#include <Engine/Plugins/PluginContext.hpp>

#include <Engine/Framework/EngineGlobals.hpp>

namespace Hyperion {
namespace MCP {

class MCPBridgeModule final : public IPluginModule
{
public:
    void OnLoad(PluginContext& context) override
    {
        // channels defined in plugins load after the engine's LogChannelRegistrar::RegisterAll, so no redirect
        // (get_logs included) would see them; register ours dynamically instead
        m_logChannelHandle = MakeUnique<DynamicLogChannelHandle>(Logger::GetInstance().CreateDynamicLogChannel(g_logChannel_MCP));

        // Touch the router so all tools register (and log) at load time
        // rather than on the first MCP request.
        MCPRouter::GetInstance();

        HYP_LOG(MCP, Info, "MCP bridge loaded (engine {}.{}.{})", HYP_VERSION_MAJOR, HYP_VERSION_MINOR, HYP_VERSION_PATCH);

        // headless editor hosts (tests, commandlets) would otherwise compete with a running editor for the port
        if (EngineGlobals::IsHeadless())
        {
            return;
        }

        MCPServer::GetInstance().AutoStart();
    }

    void OnUnload() override
    {
        MCPServer::GetInstance().Stop();

        // no request can touch the registry now; release instances before the engine tears down
        ClearRegisteredInstances();

        m_logChannelHandle.Reset();
    }

private:
    UniquePtr<DynamicLogChannelHandle> m_logChannelHandle;
};

} // namespace MCP
} // namespace Hyperion

HYP_DEFINE_PLUGIN_MODULE(MCPBridge, MCPBridgeEditor, ::Hyperion::MCP::MCPBridgeModule)

static const bool g_pluginModuleInitializerAdded = (::Hyperion::AddPluginModuleInitializer(&HypRegisterPluginModule_MCPBridgeEditor), true);
