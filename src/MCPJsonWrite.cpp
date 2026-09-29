/*!
 *  @author: The Hyperion Contributors
 *  @date 2016-2026
 *  @licence MIT
*/

#include "MCPJsonConvert.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace Hyperion {
namespace MCP {

namespace {

void AppendJsonString(String& out, const String& text)
{
    static constexpr char hexDigits[] = "0123456789abcdef";

    out.Append('"');

    // raw UTF-8 bytes; only quotes, backslashes and control characters need escaping
    const char* chars = text.Data();

    for (size_t i = 0; i < text.Size(); i++)
    {
        const unsigned char ch = static_cast<unsigned char>(chars[i]);

        switch (ch)
        {
        case '"':
            out.Append("\\\"");
            break;
        case '\\':
            out.Append("\\\\");
            break;
        case '\b':
            out.Append("\\b");
            break;
        case '\f':
            out.Append("\\f");
            break;
        case '\n':
            out.Append("\\n");
            break;
        case '\r':
            out.Append("\\r");
            break;
        case '\t':
            out.Append("\\t");
            break;
        default:
            if (ch < 0x20)
            {
                out.Append("\\u00");
                out.Append(hexDigits[ch >> 4]);
                out.Append(hexDigits[ch & 0xF]);
            }
            else
            {
                out.Append(static_cast<char>(ch));
            }

            break;
        }
    }

    out.Append('"');
}

void AppendJsonNumber(String& out, double number)
{
    if (!std::isfinite(number))
    {
        out.Append("null");

        return;
    }

    char buffer[64];

    if (std::trunc(number) == number && std::fabs(number) < 9007199254740992.0)
    {
        std::snprintf(buffer, sizeof(buffer), "%lld", static_cast<long long>(number));
        out.Append(buffer);

        return;
    }

    // shortest form that reads back to the same value. Most engine values are floats widened to double,
    // so round-trip those at float precision (0.12f prints as 0.12, not 0.119999997)
    const bool isFloatValue = double(float(number)) == number;

    for (int precision = 1; precision <= 17; precision++)
    {
        std::snprintf(buffer, sizeof(buffer), "%.*g", precision, number);

        const double parsed = std::strtod(buffer, nullptr);

        if (isFloatValue ? float(parsed) == float(number) : parsed == number)
        {
            break;
        }
    }

    out.Append(buffer);
}

void AppendJson(String& out, const JSON::Value& value)
{
    if (value.IsString())
    {
        AppendJsonString(out, value.AsString());
    }
    else if (value.IsNumber())
    {
        AppendJsonNumber(out, value.AsNumber());
    }
    else if (value.IsBool())
    {
        out.Append(value.AsBool() ? "true" : "false");
    }
    else if (value.IsArray())
    {
        out.Append('[');

        bool first = true;

        for (const JSON::Value& element : value.AsArray())
        {
            if (!first)
            {
                out.Append(',');
            }

            first = false;

            AppendJson(out, element);
        }

        out.Append(']');
    }
    else if (value.IsObject())
    {
        out.Append('{');

        bool first = true;

        for (const auto& member : value.AsObject())
        {
            if (!first)
            {
                out.Append(',');
            }

            first = false;

            AppendJsonString(out, member.first);
            out.Append(':');
            AppendJson(out, member.second);
        }

        out.Append('}');
    }
    else
    {
        out.Append("null");
    }
}

} // namespace

String WriteJson(const JSON::Value& value)
{
    String out;
    AppendJson(out, value);

    return out;
}

} // namespace MCP
} // namespace Hyperion
