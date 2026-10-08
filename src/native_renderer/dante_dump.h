// renderer=dante diagnostics: reads a texture back and writes it as a PNG
// (float formats normalized to their min..max range). Stalls the GPU; only
// used for the F8 traced frame.

#pragma once

#include <filesystem>

namespace Diligent {
struct IRenderDevice;
struct IDeviceContext;
struct ITexture;
}  // namespace Diligent

namespace native {

std::filesystem::path FrameDumpDirectory();

bool DumpTexturePng(Diligent::IRenderDevice* device, Diligent::IDeviceContext* context,
                    Diligent::ITexture* texture, const std::filesystem::path& path);

}  // namespace native
