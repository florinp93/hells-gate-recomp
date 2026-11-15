#include "overlay_style.h"
#include "path_util.h"

#include <rex/logging/macros.h>
#include <rex/ui/window.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

namespace overlay {

namespace {

struct FontSpec {
  float size;  // pixels at 1080p
  int weight;  // 0 regular, 1 semibold, 2 bold
};

constexpr FontSpec kSpecs[size_t(Font::kCount)] = {
    {17.0f, 1}, {21.0f, 0}, {21.0f, 1}, {30.0f, 2}, {42.0f, 2},
};

std::array<ImFont*, size_t(Font::kCount)> g_fonts{};

// Basic Latin, Latin-1, Latin Extended-A, general punctuation, euro, trade mark.
constexpr ImWchar kGlyphRanges[] = {
    0x0020, 0x017F, 0x2010, 0x205E, 0x20AC, 0x20AC, 0x2122, 0x2122, 0,
};

std::filesystem::path FindFontFile(int weight) {
  std::vector<std::filesystem::path> candidates;
#if defined(_WIN32)
  const char* windir = std::getenv("WINDIR");
  std::filesystem::path fonts = std::filesystem::path(windir ? windir : "C:\\Windows") / "Fonts";
  static constexpr const char* kWindows[3][2] = {
      {"segoeui.ttf", "arial.ttf"},
      {"seguisb.ttf", "segoeui.ttf"},
      {"segoeuib.ttf", "arialbd.ttf"},
  };
  for (const char* name : kWindows[weight]) candidates.push_back(fonts / name);
#else
  static constexpr const char* kLinux[3][4] = {
      {"/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
       "/usr/share/fonts/noto/NotoSans-Regular.ttf",
       "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf"},
      {"/usr/share/fonts/truetype/noto/NotoSans-SemiBold.ttf",
       "/usr/share/fonts/noto/NotoSans-SemiBold.ttf",
       "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
       "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf"},
      {"/usr/share/fonts/truetype/noto/NotoSans-Bold.ttf",
       "/usr/share/fonts/noto/NotoSans-Bold.ttf",
       "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
       "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf"},
  };
  for (const char* name : kLinux[weight]) candidates.push_back(name);
#endif
  for (const auto& path : candidates) {
    std::error_code ec;
    if (std::filesystem::is_regular_file(path, ec)) return path;
  }
  return {};
}

}  // namespace

void LoadFonts(ImFontAtlas* atlas, rex::ui::Window* window) {
  // The atlas is baked once (the SDK's ImGui backends have no dynamic font
  // textures); other window sizes scale the baked glyphs.
  float physical_height = window ? float(window->GetActualPhysicalHeight()) : 0.0f;
  if (physical_height < 360.0f) physical_height = 1080.0f;
  const float density =
      window ? std::max(1.0f, float(window->GetDpi()) / float(window->GetMediumDpi())) : 1.0f;
  const float bake_scale = std::clamp(physical_height / 1080.0f, 0.6f, 3.0f);

  for (size_t i = 0; i < g_fonts.size(); ++i) {
    const FontSpec& spec = kSpecs[i];
    ImFontConfig config;
    config.SizePixels = std::round(spec.size * bake_scale) / density;
    config.RasterizerDensity = density;
    config.OversampleH = 2;
    config.OversampleV = 1;
    std::snprintf(config.Name, sizeof(config.Name), "overlay-%zu", i);
    ImFont* font = nullptr;
    std::filesystem::path path = FindFontFile(spec.weight);
    if (!path.empty()) {
      font = atlas->AddFontFromFileTTF(path_util::Utf8(path).c_str(), config.SizePixels, &config,
                                       kGlyphRanges);
    }
    if (!font) {
      REXLOG_WARN("Overlay: no system font for weight {}, using the built-in font", spec.weight);
      font = atlas->AddFontDefault(&config);
    }
    g_fonts[i] = font;
  }
  REXLOG_INFO("Overlay: fonts baked for {:.0f}-pixel display (scale {:.2f}, density {:.2f})",
              physical_height, bake_scale, density);
}

float UiScale(const ImGuiIO& io) { return std::max(io.DisplaySize.y / 1080.0f, 0.5f); }

ImFont* GetFont(Font font) {
  ImFont* f = g_fonts[size_t(font)];
  return f ? f : ImGui::GetFont();
}

float FontSize(Font font, float scale) { return std::round(kSpecs[size_t(font)].size * scale); }

ImVec2 MeasureText(Font font, float scale, std::string_view text) {
  return GetFont(font)->CalcTextSizeA(FontSize(font, scale), FLT_MAX, 0.0f, text.data(),
                                      text.data() + text.size());
}

void DrawText(ImDrawList* list, Font font, float scale, ImVec2 pos, ImU32 color,
              std::string_view text) {
  pos.x = std::round(pos.x);
  pos.y = std::round(pos.y);
  list->AddText(GetFont(font), FontSize(font, scale), pos, color, text.data(),
                text.data() + text.size());
}

std::string Ellipsize(Font font, float scale, std::string_view text, float max_width) {
  if (MeasureText(font, scale, text).x <= max_width) return std::string(text);
  static constexpr std::string_view kEllipsis = "\xE2\x80\xA6";  // U+2026
  size_t length = text.size();
  while (length > 0) {
    --length;
    // Don't cut a UTF-8 sequence.
    while (length > 0 && (uint8_t(text[length]) & 0xC0) == 0x80) --length;
    std::string candidate(text.substr(0, length));
    while (!candidate.empty() && candidate.back() == ' ') candidate.pop_back();
    candidate += kEllipsis;
    if (MeasureText(font, scale, candidate).x <= max_width) return candidate;
  }
  return std::string(kEllipsis);
}

std::string WrapLines(Font font, float scale, std::string_view text, float max_width,
                      int max_lines) {
  std::string result;
  int lines = 0;
  while (!text.empty() && lines < max_lines) {
    while (!text.empty() && text.front() == ' ') text.remove_prefix(1);
    if (text.empty()) break;
    if (lines == max_lines - 1) {
      if (!result.empty()) result += '\n';
      result += Ellipsize(font, scale, text, max_width);
      break;
    }
    const char* end = GetFont(font)->CalcWordWrapPosition(FontSize(font, scale), text.data(),
                                                          text.data() + text.size(), max_width);
    size_t length = std::max<size_t>(1, size_t(end - text.data()));
    if (!result.empty()) result += '\n';
    std::string_view line = text.substr(0, length);
    while (!line.empty() && (line.back() == ' ' || line.back() == '\n')) line.remove_suffix(1);
    result += line;
    text.remove_prefix(length);
    ++lines;
  }
  return result;
}

void DrawShadow(ImDrawList* list, ImVec2 min, ImVec2 max, float rounding, float size, float alpha) {
  constexpr int kSteps = 8;
  for (int i = kSteps; i >= 1; --i) {
    float spread = size * float(i) / kSteps;
    float a = alpha * 0.11f * (1.0f - float(i - 1) / kSteps);
    list->AddRectFilled(ImVec2(min.x - spread, min.y - spread * 0.6f),
                        ImVec2(max.x + spread, max.y + spread * 1.4f),
                        IM_COL32(0, 0, 0, int(std::clamp(a, 0.0f, 1.0f) * 255.0f)),
                        rounding + spread);
  }
}

ImU32 WithAlpha(ImU32 color, float alpha) {
  uint32_t a = (color >> IM_COL32_A_SHIFT) & 0xFF;
  a = uint32_t(std::clamp(float(a) * alpha, 0.0f, 255.0f));
  return (color & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
}

}  // namespace overlay
