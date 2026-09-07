/*!
 *  @author: The Hyperion Contributors
 *  @date 2016-2026
 *  @licence MIT
*/

#pragma once

#include "MCP.hpp"

#include <Core/Utilities/Result.hpp>
#include <Core/Utilities/Uuid.hpp>

#include <Core/Reflection/BoxedValue.hpp>

namespace Hyperion {
namespace MCP {

/*! \brief Render a JSON value as a plain string (used for CVar SetFromString,
 *  commandlet argument building, etc). Strings are returned unquoted. */
inline String JsonValueToString(const JSON::Value& value)
{
    if (value.IsString())
    {
        return value.AsString();
    }

    if (value.IsBool())
    {
        return value.AsBool() ? "true" : "false";
    }

    if (value.IsNullOrUndefined())
    {
        return String::empty;
    }

    return value.ToString();
}

/*! \brief Parse a UUID from a JSON value (expected to be a string). */
inline TResult<UUID> JsonToUuid(const JSON::Value& value)
{
    if (!value.IsString())
    {
        return TResult<UUID>(HYP_MAKE_ERROR(Error, "Expected a UUID string"));
    }

    UUID uuid(value.AsString().Data());
    if (uuid == UUID::Invalid())
    {
        return TResult<UUID>(HYP_MAKE_ERROR(Error, "Invalid UUID '{}'", value.AsString()));
    }

    return TResult<UUID>(uuid);
}

} // namespace MCP
} // namespace Hyperion
