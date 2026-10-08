#include "dante_dump.h"

#include "Common/interface/RefCntAutoPtr.hpp"
#include "Graphics/GraphicsEngine/interface/DeviceContext.h"
#include "Graphics/GraphicsEngine/interface/RenderDevice.h"
#include "Graphics/GraphicsEngine/interface/Texture.h"

#include <rex/logging/macros.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../../thirdparty/diligent-core/ThirdParty/stb/stb_image_write.h"

namespace native {
namespace {

namespace dl = Diligent;

float HalfToFloat(uint16_t h) {
  uint32_t sign = uint32_t(h >> 15) << 31;
  uint32_t exponent = (h >> 10) & 0x1F;
  uint32_t mantissa = h & 0x3FF;
  uint32_t bits;
  if (!exponent) {
    if (!mantissa) {
      bits = sign;
    } else {
      float f = std::ldexp(float(mantissa), -24);
      return sign ? -f : f;
    }
  } else if (exponent == 31) {
    bits = sign | 0x7F800000u | (mantissa << 13);
  } else {
    bits = sign | ((exponent + 112) << 23) | (mantissa << 13);
  }
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

}  // namespace

std::filesystem::path FrameDumpDirectory() {
#ifdef _WIN32
  wchar_t module_path[MAX_PATH];
  if (GetModuleFileNameW(nullptr, module_path, MAX_PATH)) {
    return std::filesystem::path(module_path).parent_path() / "frame_dump";
  }
#endif
  return std::filesystem::current_path() / "frame_dump";
}

bool DumpTexturePng(dl::IRenderDevice* device, dl::IDeviceContext* context, dl::ITexture* texture,
                    const std::filesystem::path& path) {
  if (!texture) return false;
  const dl::TextureDesc& src = texture->GetDesc();
  dl::TextureDesc desc;
  desc.Name = "DumpStaging";
  desc.Type = dl::RESOURCE_DIM_TEX_2D;
  desc.Width = src.Width;
  desc.Height = src.Height;
  desc.Format = src.Format;
  desc.Usage = dl::USAGE_STAGING;
  desc.CPUAccessFlags = dl::CPU_ACCESS_READ;
  dl::RefCntAutoPtr<dl::ITexture> staging;
  device->CreateTexture(desc, nullptr, &staging);
  if (!staging) return false;
  dl::CopyTextureAttribs copy(texture, dl::RESOURCE_STATE_TRANSITION_MODE_TRANSITION, staging,
                              dl::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  context->CopyTexture(copy);
  context->WaitForIdle();
  dl::MappedTextureSubresource mapped;
  context->MapTextureSubresource(staging, 0, 0, dl::MAP_READ, dl::MAP_FLAG_DO_NOT_WAIT, nullptr,
                                 mapped);
  if (!mapped.pData) return false;

  const uint32_t w = src.Width, h = src.Height;
  std::vector<float> rgba(size_t(w) * h * 4, 1.0f);
  const auto* bytes = static_cast<const uint8_t*>(mapped.pData);
  bool known = true;
  for (uint32_t y = 0; y < h; ++y) {
    const uint8_t* row = bytes + size_t(y) * mapped.Stride;
    for (uint32_t x = 0; x < w; ++x) {
      float* out = &rgba[(size_t(y) * w + x) * 4];
      switch (src.Format) {
        case dl::TEX_FORMAT_RGBA8_UNORM:
          for (int c = 0; c < 4; ++c) out[c] = row[x * 4 + c] / 255.0f;
          break;
        case dl::TEX_FORMAT_RGB10A2_UNORM: {
          uint32_t v;
          std::memcpy(&v, row + x * 4, 4);
          out[0] = (v & 1023) / 1023.0f;
          out[1] = ((v >> 10) & 1023) / 1023.0f;
          out[2] = ((v >> 20) & 1023) / 1023.0f;
          out[3] = (v >> 30) / 3.0f;
          break;
        }
        case dl::TEX_FORMAT_RGBA16_FLOAT:
        case dl::TEX_FORMAT_RG16_FLOAT: {
          uint32_t channels = src.Format == dl::TEX_FORMAT_RGBA16_FLOAT ? 4 : 2;
          for (uint32_t c = 0; c < channels; ++c) {
            uint16_t v;
            std::memcpy(&v, row + (x * channels + c) * 2, 2);
            out[c] = HalfToFloat(v);
          }
          if (channels == 2) out[2] = 0.0f;
          break;
        }
        case dl::TEX_FORMAT_R32_FLOAT: {
          float v;
          std::memcpy(&v, row + x * 4, 4);
          out[0] = out[1] = out[2] = v;
          break;
        }
        default:
          known = false;
          break;
      }
    }
  }
  context->UnmapTextureSubresource(staging, 0, 0);
  if (!known) {
    REXLOG_INFO("NATIVE-DUMP skipped {} (format {})", path.filename().string(), int(src.Format));
    return false;
  }

  // Float formats: normalize RGB to the observed range so HDR / depth are visible.
  bool is_float = src.Format != dl::TEX_FORMAT_RGBA8_UNORM &&
                  src.Format != dl::TEX_FORMAT_RGB10A2_UNORM;
  float lo = FLT_MAX, hi = -FLT_MAX;
  for (size_t i = 0; i < rgba.size(); i += 4) {
    for (int c = 0; c < 3; ++c) {
      if (std::isfinite(rgba[i + c])) {
        lo = std::min(lo, rgba[i + c]);
        hi = std::max(hi, rgba[i + c]);
      }
    }
  }
  float scale = (is_float && hi > lo) ? 1.0f / (hi - lo) : 1.0f;
  float bias = is_float && hi > lo ? lo : 0.0f;
  std::vector<uint8_t> png(size_t(w) * h * 4);
  for (size_t i = 0; i < size_t(w) * h; ++i) {
    for (int c = 0; c < 3; ++c) {
      float v = (rgba[i * 4 + c] - bias) * scale;
      png[i * 4 + c] = uint8_t(std::clamp(std::isfinite(v) ? v : 0.0f, 0.0f, 1.0f) * 255.0f + 0.5f);
    }
    png[i * 4 + 3] = 255;
  }
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  bool ok = stbi_write_png(path.string().c_str(), int(w), int(h), 4, png.data(), int(w) * 4) != 0;
  REXLOG_INFO("NATIVE-DUMP {} {}x{} fmt={} range={:.4f}..{:.4f} {}", path.filename().string(), w,
              h, int(src.Format), lo, hi, ok ? "written" : "FAILED");
  return ok;
}

}  // namespace native
