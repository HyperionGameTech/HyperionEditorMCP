/*!
 *  @author: The Hyperion Contributors
 *  @date 2016-2026
 *  @licence MIT
*/

#pragma once

#include "MCP.hpp"

#include <Core/Reflection/Handle.hpp>

namespace Hyperion {

class World;

namespace MCP {

/*! \brief The World the editor is showing: the active scene's world, falling back to the open project's.
 *  Sim thread only. */
Handle<World> GetActiveEditorWorld();

} // namespace MCP
} // namespace Hyperion
