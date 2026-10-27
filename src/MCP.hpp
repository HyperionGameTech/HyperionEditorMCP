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

namespace Hyperion {
namespace MCP {

HYP_DECLARE_LOG_CHANNEL(MCP);

/*! \brief Maximum number of log lines kept in the MCP server's ring buffer. */
static constexpr uint32 MCPMaxLogLines = 1024;

/*! \brief Key a tool result object uses to carry an image: {"data": <base64>, "mimeType": "image/png"}.
 *  It is sent to the client as MCP image content rather than as part of the JSON text. */
static constexpr const char* MCPImageContentKey = "__mcpImage";

} // namespace MCP
} // namespace Hyperion
