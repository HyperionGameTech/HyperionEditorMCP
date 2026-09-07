/*!
 *  @author: The Hyperion Contributors
 *  @date 2016-2026
 *  @licence MIT
*/

#pragma once

#include <Core/Defines.hpp>

#include <Core/Types.hpp>

#include <Core/Logging/Logger.hpp>

#include <Core/DataProcessing/JSON/JSON.hpp>

#include <Engine/Plugins/PluginAPI.hpp>

namespace Hyperion {
namespace MCP {

/*! \brief The MCP log channel lives inside this plugin DLL; exported so the
 *  plugin's own translation units (and any tool plugins) can log to it. */
HYP_PLUGIN_API HYP_DECLARE_LOG_CHANNEL(MCP);

/*! \brief Maximum number of log lines kept in the MCP server's ring buffer. */
static constexpr uint32 MCPMaxLogLines = 1024;

} // namespace MCP
} // namespace Hyperion
