/*!
 *  @author: The Hyperion Contributors
 *  @date 2016-2026
 *  @licence MIT
*/

#pragma once

#include "MCP.hpp"

#include <Core/Containers/Array.hpp>
#include <Core/Containers/String.hpp>

namespace Hyperion {
namespace MCP {

struct CapturedImage
{
    uint32 width = 0;
    uint32 height = 0;
    Array<ubyte> rgb; // tightly packed, top-down
};

/*! \brief Grab what is currently on screen inside a (possibly child) native window. Kept in its own
 *  translation unit so windows.h stays away from the engine headers. */
bool CaptureNativeWindow(void* nativeWindowHandle, CapturedImage& outImage, String& outError);

/*! \brief Box-filter the image down so it is at most \p maxWidth wide (no-op if already narrower). */
void DownscaleImage(CapturedImage& image, uint32 maxWidth);

bool EncodePng(const CapturedImage& image, Array<ubyte>& outPngBytes);

/*! \brief Standard base64. String::Base64Encode sign-extends bytes >= 0x80 and corrupts binary data. */
String EncodeBase64(const Array<ubyte>& bytes);

} // namespace MCP
} // namespace Hyperion
