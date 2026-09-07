/*!
 *  @author: The Hyperion Contributors
 *  @date 2016-2026
 *  @licence MIT
*/

#pragma once

#include "MCP.hpp"

namespace Hyperion {
namespace MCP {

bool HandleMessage(const JSON::Object& request, JSON::Object& outResponse);

} // namespace MCP
} // namespace Hyperion
