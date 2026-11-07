#include "button_glyphs.h"

#include "Common/interface/RefCntAutoPtr.hpp"
#include "Graphics/GraphicsEngine/interface/RenderDevice.h"
#include "Graphics/GraphicsEngine/interface/Texture.h"

#include <rex/graphics/xenos.h>
#include <rex/logging/macros.h>

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "../../thirdparty/diligent-core/ThirdParty/stb/stb_image.h"

namespace renderer {

struct GlyphIconFile {
  const char* name;
  const unsigned char* data;
  size_t size;
};
extern const GlyphIconFile kGlyphIconFiles[];
extern const size_t kGlyphIconFileCount;

namespace {

namespace dl = Diligent;

// RGBA image, components 0..1.
struct Image {
  int w = 0, h = 0;
  std::vector<float> px;
  Image() = default;
  Image(int width, int height) : w(width), h(height), px(size_t(width) * height * 4, 0.0f) {}
  float* at(int x, int y) { return &px[(size_t(y) * w + x) * 4]; }
  const float* at(int x, int y) const { return &px[(size_t(y) * w + x) * 4]; }
};

// Single-channel mask / coverage.
struct Mask {
  int w = 0, h = 0;
  std::vector<float> v;
  Mask() = default;
  Mask(int width, int height) : w(width), h(height), v(size_t(width) * height, 0.0f) {}
  float& at(int x, int y) { return v[size_t(y) * w + x]; }
  float at(int x, int y) const { return v[size_t(y) * w + x]; }
};

// A rectangle of an image, edited in place.
struct Cell {
  Image& image;
  int x0, y0, w, h;
  float* at(int x, int y) { return image.at(x0 + x, y0 + y); }
};

float Luminance(const float* p) { return p[0] * 0.299f + p[1] * 0.587f + p[2] * 0.114f; }
float Saturation(const float* p) {
  return std::max({p[0], p[1], p[2]}) - std::min({p[0], p[1], p[2]});
}

// BC3 (DXT5) decoding

void Decode565(uint16_t c, float out[3]) {
  out[0] = float((c >> 11) & 31) / 31.0f;
  out[1] = float((c >> 5) & 63) / 63.0f;
  out[2] = float(c & 31) / 31.0f;
}

Image DecodeBc3(const uint8_t* blocks, int width, int height) {
  Image image(width, height);
  int bx_count = (width + 3) / 4, by_count = (height + 3) / 4;
  for (int by = 0; by < by_count; ++by) {
    for (int bx = 0; bx < bx_count; ++bx) {
      const uint8_t* b = blocks + (size_t(by) * bx_count + bx) * 16;
      float alpha[8];
      alpha[0] = b[0] / 255.0f;
      alpha[1] = b[1] / 255.0f;
      if (b[0] > b[1]) {
        for (int i = 1; i < 7; ++i) alpha[i + 1] = ((7 - i) * alpha[0] + i * alpha[1]) / 7.0f;
      } else {
        for (int i = 1; i < 5; ++i) alpha[i + 1] = ((5 - i) * alpha[0] + i * alpha[1]) / 5.0f;
        alpha[6] = 0.0f;
        alpha[7] = 1.0f;
      }
      uint64_t alpha_bits = 0;
      for (int i = 0; i < 6; ++i) alpha_bits |= uint64_t(b[2 + i]) << (8 * i);
      uint16_t c0 = uint16_t(b[8] | (b[9] << 8)), c1 = uint16_t(b[10] | (b[11] << 8));
      float color[4][3];
      Decode565(c0, color[0]);
      Decode565(c1, color[1]);
      for (int c = 0; c < 3; ++c) {
        color[2][c] = (2 * color[0][c] + color[1][c]) / 3.0f;
        color[3][c] = (color[0][c] + 2 * color[1][c]) / 3.0f;
      }
      uint32_t color_bits = uint32_t(b[12]) | (uint32_t(b[13]) << 8) | (uint32_t(b[14]) << 16) |
                            (uint32_t(b[15]) << 24);
      for (int i = 0; i < 16; ++i) {
        int x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
        if (x >= width || y >= height) continue;
        float* p = image.at(x, y);
        const float* c = color[(color_bits >> (2 * i)) & 3];
        p[0] = c[0];
        p[1] = c[1];
        p[2] = c[2];
        p[3] = alpha[(alpha_bits >> (3 * i)) & 7];
      }
    }
  }
  return image;
}

Image DecodeBc1(const uint8_t* blocks, int width, int height) {
  Image image(width, height);
  int bx_count = (width + 3) / 4, by_count = (height + 3) / 4;
  for (int by = 0; by < by_count; ++by) {
    for (int bx = 0; bx < bx_count; ++bx) {
      const uint8_t* b = blocks + (size_t(by) * bx_count + bx) * 8;
      uint16_t c0 = uint16_t(b[0] | (b[1] << 8)), c1 = uint16_t(b[2] | (b[3] << 8));
      float color[4][4] = {};
      Decode565(c0, color[0]);
      Decode565(c1, color[1]);
      color[0][3] = color[1][3] = color[2][3] = color[3][3] = 1.0f;
      for (int c = 0; c < 3; ++c) {
        if (c0 > c1) {
          color[2][c] = (2 * color[0][c] + color[1][c]) / 3.0f;
          color[3][c] = (color[0][c] + 2 * color[1][c]) / 3.0f;
        } else {
          color[2][c] = (color[0][c] + color[1][c]) / 2.0f;
        }
      }
      if (c0 <= c1) color[3][3] = 0.0f;
      uint32_t bits =
          uint32_t(b[4]) | (uint32_t(b[5]) << 8) | (uint32_t(b[6]) << 16) | (uint32_t(b[7]) << 24);
      for (int i = 0; i < 16; ++i) {
        int x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
        if (x >= width || y >= height) continue;
        std::memcpy(image.at(x, y), color[(bits >> (2 * i)) & 3], sizeof(float) * 4);
      }
    }
  }
  return image;
}

// Masks

// Dilation (grow) or erosion of a binary mask with a k x k square, as a row
// pass and a column pass (pixels past the edge repeat the border).
std::vector<bool> Morph(const std::vector<bool>& in, int w, int h, int k, bool dilate) {
  int r = k / 2;
  auto pass = [&](const std::vector<bool>& src, bool rows) {
    std::vector<bool> out(src.size());
    int lines = rows ? h : w, length = rows ? w : h;
    std::vector<int> prefix(size_t(length) + 1);
    for (int line = 0; line < lines; ++line) {
      auto at = [&](int i) { return rows ? size_t(line) * w + i : size_t(i) * w + line; };
      // Pixels equal to `dilate` in [0, i).
      for (int i = 0; i < length; ++i)
        prefix[size_t(i) + 1] = prefix[size_t(i)] + (src[at(i)] == dilate);
      for (int i = 0; i < length; ++i) {
        int a = i - r, b = i + r;
        int count = prefix[size_t(std::min(b, length - 1)) + 1] - prefix[size_t(std::max(a, 0))];
        if (a < 0) count += (src[at(0)] == dilate) ? -a : 0;
        if (b >= length) count += (src[at(length - 1)] == dilate) ? b - length + 1 : 0;
        out[at(i)] = count > 0 ? dilate : !dilate;
      }
    }
    return out;
  };
  return pass(pass(in, true), false);
}

// Fills masked pixels from their unmasked neighbours (diffusion), RGB only.
void Inpaint(Cell cell, std::vector<bool> unknown, int iterations = 60) {
  std::vector<float> fill(size_t(cell.w) * cell.h * 3);
  std::vector<bool> filled(unknown.size());
  for (int it = 0; it < iterations; ++it) {
    bool any = false;
    std::fill(filled.begin(), filled.end(), false);
    for (int y = 0; y < cell.h; ++y) {
      for (int x = 0; x < cell.w; ++x) {
        size_t i = size_t(y) * cell.w + x;
        if (!unknown[i]) continue;
        float acc[3] = {};
        int count = 0;
        for (int dy = -1; dy <= 1; ++dy) {
          for (int dx = -1; dx <= 1; ++dx) {
            if (!dx && !dy) continue;
            int sx = x + dx, sy = y + dy;
            if (sx < 0 || sy < 0 || sx >= cell.w || sy >= cell.h) continue;
            if (unknown[size_t(sy) * cell.w + sx]) continue;
            const float* p = cell.at(sx, sy);
            acc[0] += p[0];
            acc[1] += p[1];
            acc[2] += p[2];
            ++count;
          }
        }
        if (!count) continue;
        for (int c = 0; c < 3; ++c) fill[i * 3 + c] = acc[c] / float(count);
        filled[i] = true;
        any = true;
      }
    }
    if (!any) break;
    for (int y = 0; y < cell.h; ++y) {
      for (int x = 0; x < cell.w; ++x) {
        size_t i = size_t(y) * cell.w + x;
        if (!filled[i]) continue;
        float* p = cell.at(x, y);
        p[0] = fill[i * 3];
        p[1] = fill[i * 3 + 1];
        p[2] = fill[i * 3 + 2];
        unknown[i] = false;
      }
    }
  }
}

// Source over destination; destination alpha = max.
void Over(float* dst, const float rgb[3], float a) {
  if (a <= 0.0f) return;
  for (int c = 0; c < 3; ++c) dst[c] = rgb[c] * a + dst[c] * (1.0f - a);
  dst[3] = std::max(dst[3], a);
}

// Text (GDI, bold, drawn at 4x): Palatino Linotype on the game's stone
// buttons, Bahnschrift on Kenney's keys.

constexpr int kTextScale = 4;
constexpr const wchar_t* kSerif = L"Palatino Linotype";
constexpr const wchar_t* kKeyFont = L"Bahnschrift";

class GdiCanvas {
 public:
  GdiCanvas(int w, int h) : w_(w * kTextScale), h_(h * kTextScale) {
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w_;
    bi.bmiHeader.biHeight = -h_;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    dc_ = CreateCompatibleDC(nullptr);
    bitmap_ =
        CreateDIBSection(dc_, &bi, DIB_RGB_COLORS, reinterpret_cast<void**>(&bits_), nullptr, 0);
    old_bitmap_ = SelectObject(dc_, bitmap_);
    SetGraphicsMode(dc_, GM_ADVANCED);
    SetBkMode(dc_, TRANSPARENT);
  }
  ~GdiCanvas() {
    if (font_) DeleteObject(SelectObject(dc_, old_font_));
    SelectObject(dc_, old_bitmap_);
    DeleteObject(bitmap_);
    DeleteDC(dc_);
  }
  bool ok() const { return bits_ != nullptr; }

  void SetFont(int px, const wchar_t* face) {
    if (font_) DeleteObject(SelectObject(dc_, old_font_));
    font_ = CreateFontW(-px * kTextScale, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                        OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                        DEFAULT_PITCH | FF_DONTCARE, face);
    old_font_ = SelectObject(dc_, font_);
    GetTextMetricsW(dc_, &metrics_);
  }
  float Advance(wchar_t ch) {
    SIZE size;
    GetTextExtentPoint32W(dc_, &ch, 1, &size);
    return float(size.cx);
  }
  // Ink middle of a line relative to its baseline (4x units), like an "mm" anchor.
  float MiddleAboveBaseline() const { return float(metrics_.tmAscent - metrics_.tmDescent) / 2.0f; }

  void Clear() { std::memset(bits_, 0, size_t(w_) * h_ * 4); }

  // Draws text with its baseline-left at (0, 0) after the transform, as fill
  // (stroke_px = 0) or as a stroke of that width around the outline (4x px).
  void Draw(const std::wstring& text, const XFORM& transform, int stroke_px) {
    SetWorldTransform(dc_, &transform);
    SetTextAlign(dc_, TA_LEFT | TA_BASELINE);
    BeginPath(dc_);
    TextOutW(dc_, 0, 0, text.c_str(), int(text.size()));
    EndPath(dc_);
    HBRUSH brush = CreateSolidBrush(RGB(255, 255, 255));
    HGDIOBJ old_brush = SelectObject(dc_, brush);
    if (stroke_px > 0) {
      LOGBRUSH lb = {BS_SOLID, RGB(255, 255, 255), 0};
      HPEN pen = ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_JOIN_ROUND | PS_ENDCAP_ROUND,
                              DWORD(stroke_px * 2), &lb, 0, nullptr);
      HGDIOBJ old_pen = SelectObject(dc_, pen);
      StrokeAndFillPath(dc_);
      SelectObject(dc_, old_pen);
      DeleteObject(pen);
    } else {
      FillPath(dc_);
    }
    SelectObject(dc_, old_brush);
    DeleteObject(brush);
    XFORM identity = {1, 0, 0, 1, 0, 0};
    SetWorldTransform(dc_, &identity);
  }

  // Coverage at 4x (red channel).
  float Coverage(int x, int y) const { return bits_[(size_t(y) * w_ + x) * 4 + 2] / 255.0f; }
  int width() const { return w_; }
  int height() const { return h_; }

  // Box-filtered 1x coverage.
  Mask Downsample() const {
    Mask m(w_ / kTextScale, h_ / kTextScale);
    for (int y = 0; y < m.h; ++y) {
      for (int x = 0; x < m.w; ++x) {
        float sum = 0;
        for (int sy = 0; sy < kTextScale; ++sy) {
          for (int sx = 0; sx < kTextScale; ++sx) {
            sum += Coverage(x * kTextScale + sx, y * kTextScale + sy);
          }
        }
        m.at(x, y) = sum / float(kTextScale * kTextScale);
      }
    }
    return m;
  }

 private:
  int w_, h_;
  HDC dc_ = nullptr;
  HBITMAP bitmap_ = nullptr;
  HGDIOBJ old_bitmap_ = nullptr;
  HFONT font_ = nullptr;
  HGDIOBJ old_font_ = nullptr;
  uint8_t* bits_ = nullptr;
  TEXTMETRICW metrics_ = {};
};

XFORM Translate(float x, float y) { return XFORM{1, 0, 0, 1, x, y}; }

// Text centred (by its ink) in box (x0, y0, x1, y1) of a w x h layer.
Mask CenteredText(int w, int h, const std::wstring& text, const float box[4], int font_px,
                  const wchar_t* face) {
  GdiCanvas canvas(w, h);
  if (!canvas.ok()) return Mask(w, h);
  canvas.SetFont(font_px, face);
  canvas.Clear();
  // Draw once at a known place to measure the ink.
  float ox = float(canvas.width()) * 0.25f, oy = float(canvas.height()) * 0.75f;
  canvas.Draw(text, Translate(ox, oy), 0);
  int x0 = canvas.width(), y0 = canvas.height(), x1 = -1, y1 = -1;
  for (int y = 0; y < canvas.height(); ++y) {
    for (int x = 0; x < canvas.width(); ++x) {
      if (canvas.Coverage(x, y) > 0.0f) {
        x0 = std::min(x0, x);
        y0 = std::min(y0, y);
        x1 = std::max(x1, x);
        y1 = std::max(y1, y);
      }
    }
  }
  canvas.Clear();
  if (x1 < 0) return Mask(w, h);
  float ink_cx = (x0 + x1 + 1) / 2.0f - ox, ink_cy = (y0 + y1 + 1) / 2.0f - oy;
  float cx = (box[0] + box[2]) / 2.0f * kTextScale, cy = (box[1] + box[3]) / 2.0f * kTextScale;
  canvas.Draw(text, Translate(cx - ink_cx, cy - ink_cy), 0);
  return canvas.Downsample();
}

enum class Symbol { kCross, kCircle, kSquare, kTriangle };

// Keyboard / mouse labels

// What a binding shows: a mouse button (0 left, 1 right, 2 middle; text is
// its short name) or text.
struct KeyLabel {
  int mouse = -1;
  std::wstring text;
};

// The first alternative of a keybind value ("Z,Tab" -> Z), modifiers included.
KeyLabel ParseBinding(const std::string& binding) {
  std::string_view s(binding);
  s = s.substr(0, s.find(','));
  while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
  while (!s.empty() && s.back() == ' ') s.remove_suffix(1);
  std::wstring text;
  for (;;) {
    size_t plus = s.find('+');
    if (plus == std::string_view::npos || plus == 0) break;
    std::string_view head = s.substr(0, plus);
    if (head == "Shift") {
      text += L"SHIFT+";
    } else if (head == "Ctrl" || head == "Control") {
      text += L"CTRL+";
    } else if (head == "Alt") {
      text += L"ALT+";
    } else {
      break;
    }
    s.remove_prefix(plus + 1);
  }
  struct Mouse {
    std::string_view name;
    int button;
    const wchar_t* text;
  };
  static constexpr Mouse kMouse[] = {
      {"LMB", 0, L"LMB"},        {"MouseLeft", 0, L"LMB"}, {"RMB", 1, L"RMB"},
      {"MouseRight", 1, L"RMB"}, {"MMB", 2, L"MMB"},       {"MouseMiddle", 2, L"MMB"},
  };
  for (const Mouse& m : kMouse) {
    if (s != m.name) continue;
    if (text.empty()) return {m.button, m.text};
    return {-1, text + m.text};
  }
  static const std::unordered_map<std::string_view, const wchar_t*> kNames = {
      {"Space", L"SPACE"},       {"Shift", L"SHIFT"},      {"LeftShift", L"SHIFT"},
      {"RightShift", L"SHIFT"},  {"Control", L"CTRL"},     {"Ctrl", L"CTRL"},
      {"LeftCtrl", L"CTRL"},     {"RightCtrl", L"CTRL"},   {"LeftControl", L"CTRL"},
      {"RightControl", L"CTRL"}, {"Alt", L"ALT"},          {"LeftAlt", L"ALT"},
      {"RightAlt", L"ALT"},      {"Escape", L"ESC"},       {"Esc", L"ESC"},
      {"Return", L"ENTER"},      {"Enter", L"ENTER"},      {"NumpadEnter", L"ENTER"},
      {"Tab", L"TAB"},           {"Backspace", L"BKSP"},   {"Delete", L"DEL"},
      {"Insert", L"INS"},        {"Home", L"HOME"},        {"End", L"END"},
      {"PageUp", L"PGUP"},       {"PageDown", L"PGDN"},    {"Left", L"\u2190"},
      {"Up", L"\u2191"},         {"Right", L"\u2192"},     {"Down", L"\u2193"},
      {"CapsLock", L"CAPS"},     {"NumLock", L"NUM"},      {"ScrollLock", L"SCRL"},
      {"PrintScreen", L"PRTSC"}, {"Pause", L"PAUSE"},      {"Backtick", L"`"},
      {"Minus", L"-"},           {"Plus", L"="},           {"Comma", L","},
      {"Period", L"."},          {"Semicolon", L";"},      {"Slash", L"/"},
      {"Backslash", L"\\"},      {"LBracket", L"["},       {"RBracket", L"]"},
      {"Quote", L"'"},           {"NumpadPlus", L"NUM+"},  {"NumpadMinus", L"NUM-"},
      {"NumpadStar", L"NUM*"},   {"NumpadSlash", L"NUM/"}, {"NumpadDecimal", L"NUM."},
      {"Mouse4", L"M4"},         {"Mouse5", L"M5"},        {"XButton1", L"M4"},
      {"XButton2", L"M5"},
  };
  auto it = kNames.find(s);
  if (it != kNames.end()) {
    text += it->second;
  } else if (s.size() == 7 && s.substr(0, 6) == "Numpad") {
    text += L"NUM";
    text += wchar_t(s[6]);
  } else {
    for (char c : s) text += wchar_t(std::toupper(static_cast<unsigned char>(c)));
  }
  if (text.empty()) text = L"-";
  return {-1, text};
}

// Ink size (1x pixels) of text at font_px.
std::pair<float, float> InkSize(const std::wstring& text, int font_px, const wchar_t* face) {
  GdiCanvas canvas(font_px * int(text.size() + 2), font_px * 3);
  if (!canvas.ok()) return {0.0f, 0.0f};
  canvas.SetFont(font_px, face);
  canvas.Clear();
  canvas.Draw(text, Translate(font_px * kTextScale * 0.5f, font_px * kTextScale * 2.0f), 0);
  int x0 = canvas.width(), y0 = canvas.height(), x1 = -1, y1 = -1;
  for (int y = 0; y < canvas.height(); ++y) {
    for (int x = 0; x < canvas.width(); ++x) {
      if (canvas.Coverage(x, y) <= 0.0f) continue;
      x0 = std::min(x0, x);
      y0 = std::min(y0, y);
      x1 = std::max(x1, x);
      y1 = std::max(y1, y);
    }
  }
  if (x1 < 0) return {0.0f, 0.0f};
  return {float(x1 - x0 + 1) / kTextScale, float(y1 - y0 + 1) / kTextScale};
}

// Text as large as fits box (its ink, centred).
Mask FittedText(int w, int h, const std::wstring& text, const float box[4],
                const wchar_t* face = kSerif) {
  constexpr int kReference = 32;
  auto [ink_w, ink_h] = InkSize(text, kReference, face);
  if (ink_w <= 0.0f || ink_h <= 0.0f) return Mask(w, h);
  float scale = std::min((box[2] - box[0]) / ink_w, (box[3] - box[1]) / ink_h);
  return CenteredText(w, h, text, box, std::max(4, int(kReference * scale)), face);
}

// A mouse seen from above, filling the height of box; button 0 / 1 / 2 is
// filled (any other value: none).
Mask MouseMask(int w, int h, const float box[4], int button) {
  Mask m(w, h);
  float bh = box[3] - box[1];
  float mw = std::min(box[2] - box[0], bh * 0.68f), r = mw / 2.0f;
  float cx = (box[0] + box[2]) / 2.0f, top = box[1], bottom = box[3];
  float line = std::max(1.2f, mw * 0.09f), split = top + bh * 0.45f;
  constexpr int kSamples = 4;
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      int covered = 0;
      for (int sy = 0; sy < kSamples; ++sy) {
        for (int sx = 0; sx < kSamples; ++sx) {
          float px = x + (sx + 0.5f) / kSamples, py = y + (sy + 0.5f) / kSamples;
          float ay = std::clamp(py, top + r, bottom - r);
          float d = std::hypot(px - cx, py - ay) - r;  // < 0 inside the body
          if (d >= 0.0f) continue;
          bool outline = d > -line || std::abs(py - split) < line / 2.0f ||
                         (py < split && std::abs(px - cx) < line / 2.0f);
          bool pressed = py < split && ((button == 0 && px < cx) || (button == 1 && px > cx) ||
                                        (button == 2 && std::abs(px - cx) < mw * 0.12f &&
                                         py > top + bh * 0.1f && py < split - bh * 0.06f));
          covered += outline || pressed;
        }
      }
      m.at(x, y) = float(covered) / float(kSamples * kSamples);
    }
  }
  return m;
}

const float kHoleColor[3] = {26 / 255.0f, 22 / 255.0f, 20 / 255.0f};
const float kKeyColor[3] = {1.0f, 1.0f, 1.0f};

Image DecodePng(const unsigned char* data, size_t size) {
  int w = 0, h = 0, channels = 0;
  unsigned char* pixels = stbi_load_from_memory(data, int(size), &w, &h, &channels, 4);
  if (!pixels) return {};
  Image img(w, h);
  for (size_t i = 0; i < img.px.size(); ++i) img.px[i] = pixels[i] / 255.0f;
  stbi_image_free(pixels);
  return img;
}

void FillHoles(Image& img, const float color[3]) {
  std::vector<bool> outside(size_t(img.w) * img.h);
  std::vector<int> stack;
  auto push = [&](int x, int y) {
    size_t i = size_t(y) * img.w + x;
    if (outside[i] || img.at(x, y)[3] >= 0.5f) return;
    outside[i] = true;
    stack.push_back(int(i));
  };
  for (int x = 0; x < img.w; ++x) {
    push(x, 0);
    push(x, img.h - 1);
  }
  for (int y = 0; y < img.h; ++y) {
    push(0, y);
    push(img.w - 1, y);
  }
  while (!stack.empty()) {
    int i = stack.back();
    stack.pop_back();
    int x = i % img.w, y = i / img.w;
    if (x > 0) push(x - 1, y);
    if (x + 1 < img.w) push(x + 1, y);
    if (y > 0) push(x, y - 1);
    if (y + 1 < img.h) push(x, y + 1);
  }
  std::vector<bool> holes(outside.size());
  for (size_t i = 0; i < holes.size(); ++i) holes[i] = !outside[i] && img.px[i * 4 + 3] < 0.5f;
  holes = Morph(holes, img.w, img.h, 3, true);
  for (size_t i = 0; i < holes.size(); ++i) {
    if (!holes[i]) continue;
    float* p = &img.px[i * 4];
    for (int c = 0; c < 3; ++c) p[c] = p[c] * p[3] + color[c] * (1.0f - p[3]);
    p[3] = 1.0f;
  }
}

const Image& Icon(std::string_view name, bool blank = false) {
  static std::mutex mutex;
  static std::unordered_map<std::string, Image> cache;
  std::lock_guard lock(mutex);
  std::string key = (blank ? "blank:" : "") + std::string(name);
  auto it = cache.find(key);
  if (it != cache.end()) return it->second;
  Image img;
  for (size_t i = 0; i < kGlyphIconFileCount; ++i) {
    if (name != kGlyphIconFiles[i].name) continue;
    img = DecodePng(kGlyphIconFiles[i].data, kGlyphIconFiles[i].size);
    if (img.w) FillHoles(img, blank ? kKeyColor : kHoleColor);
    break;
  }
  if (!img.w) REXLOG_WARN("GLYPHS: icon {} missing", name);
  return cache.emplace(std::move(key), std::move(img)).first->second;
}

std::array<int, 4> ContentBox(const Image& img) {
  std::array<int, 4> box = {img.w, img.h, 0, 0};
  for (int y = 0; y < img.h; ++y) {
    for (int x = 0; x < img.w; ++x) {
      if (img.at(x, y)[3] <= 0.0f) continue;
      box[0] = std::min(box[0], x);
      box[1] = std::min(box[1], y);
      box[2] = std::max(box[2], x + 1);
      box[3] = std::max(box[3], y + 1);
    }
  }
  return box;
}

void Stamp(Image& img, const float box[4], const Image& icon) {
  if (!icon.w) return;
  auto c = ContentBox(icon);
  float iw = float(c[2] - c[0]), ih = float(c[3] - c[1]);
  if (iw <= 0.0f || ih <= 0.0f) return;
  float scale = std::min((box[2] - box[0]) / iw, (box[3] - box[1]) / ih);
  float dw = iw * scale, dh = ih * scale;
  float dx = (box[0] + box[2] - dw) / 2.0f, dy = (box[1] + box[3] - dh) / 2.0f;
  auto sample = [&](float u, float v, float out[4]) {
    u -= 0.5f;
    v -= 0.5f;
    int x0 = int(std::floor(u)), y0 = int(std::floor(v));
    float fx = u - float(x0), fy = v - float(y0);
    for (int k = 0; k < 4; ++k) {
      int x = std::clamp(x0 + (k & 1), 0, icon.w - 1), y = std::clamp(y0 + (k >> 1), 0, icon.h - 1);
      float weight = ((k & 1) ? fx : 1.0f - fx) * ((k >> 1) ? fy : 1.0f - fy);
      const float* p = icon.at(x, y);
      for (int ch = 0; ch < 3; ++ch) out[ch] += p[ch] * p[3] * weight;
      out[3] += p[3] * weight;
    }
  };
  constexpr int kSamples = 4;
  int x_end = std::min(img.w, int(std::ceil(dx + dw))),
      y_end = std::min(img.h, int(std::ceil(dy + dh)));
  for (int y = std::max(0, int(dy)); y < y_end; ++y) {
    for (int x = std::max(0, int(dx)); x < x_end; ++x) {
      float acc[4] = {};
      for (int sy = 0; sy < kSamples; ++sy) {
        for (int sx = 0; sx < kSamples; ++sx) {
          float px = x + (sx + 0.5f) / kSamples, py = y + (sy + 0.5f) / kSamples;
          if (px < dx || py < dy || px > dx + dw || py > dy + dh) continue;
          sample(c[0] + (px - dx) / scale, c[1] + (py - dy) / scale, acc);
        }
      }
      for (float& v : acc) v /= float(kSamples * kSamples);
      if (acc[3] <= 0.0f) continue;
      float* d = img.at(x, y);
      float keep = d[3] * (1.0f - acc[3]), a = acc[3] + keep;
      for (int ch = 0; ch < 3; ++ch) d[ch] = (acc[ch] + d[ch] * keep) / a;
      d[3] = a;
    }
  }
}

void Stamp(Image& img, const int box[4], const Image& icon, float size = 1.0f) {
  float cx = (box[0] + box[2]) / 2.0f, cy = (box[1] + box[3]) / 2.0f;
  float hw = (box[2] - box[0]) * size / 2.0f, hh = (box[3] - box[1]) * size / 2.0f;
  const float f[4] = {cx - hw, cy - hh, cx + hw, cy + hh};
  Stamp(img, f, icon);
}

void ClearArt(Image& img, const int rect[4], int margin = 3) {
  int x0 = std::max(0, rect[0] - margin), y0 = std::max(0, rect[1] - margin);
  int x1 = std::min(img.w, rect[2] + margin), y1 = std::min(img.h, rect[3] + margin);
  std::vector<int> stack;
  auto push = [&](int x, int y) {
    if (x < x0 || y < y0 || x >= x1 || y >= y1) return;
    float* p = img.at(x, y);
    if (p[3] <= 0.0f) return;
    std::fill(p, p + 4, 0.0f);
    stack.push_back(y * img.w + x);
  };
  for (int y = std::max(0, rect[1]); y < std::min(img.h, rect[3]); ++y) {
    for (int x = std::max(0, rect[0]); x < std::min(img.w, rect[2]); ++x) {
      if (img.at(x, y)[3] > 0.5f) push(x, y);
    }
  }
  while (!stack.empty()) {
    int i = stack.back();
    stack.pop_back();
    int x = i % img.w, y = i / img.w;
    for (int dy = -1; dy <= 1; ++dy) {
      for (int dx = -1; dx <= 1; ++dx) push(x + dx, y + dy);
    }
  }
}

void ReplaceArt(Image& img, const int rect[4], const Image& icon, float size = 1.0f) {
  ClearArt(img, rect);
  Stamp(img, rect, icon, size);
}

constexpr float kKeySize = 0.82f;

struct Binding {
  std::vector<std::string> modifiers;
  std::string key;
};
Binding SplitBinding(const std::string& binding) {
  std::string_view s(binding);
  s = s.substr(0, s.find(','));
  Binding out;
  for (;;) {
    while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
    size_t plus = s.find('+');
    if (plus == std::string_view::npos || plus == 0 || plus + 1 == s.size()) break;
    std::string_view head = s.substr(0, plus);
    if (head != "Shift" && head != "Ctrl" && head != "Control" && head != "Alt") break;
    out.modifiers.emplace_back(head);
    s.remove_prefix(plus + 1);
  }
  while (!s.empty() && s.back() == ' ') s.remove_suffix(1);
  out.key = std::string(s);
  return out;
}

const std::unordered_map<std::string_view, std::string_view> kKeyIcons = {
    {"LMB", "mouse_left"},
    {"MouseLeft", "mouse_left"},
    {"RMB", "mouse_right"},
    {"MouseRight", "mouse_right"},
    {"MMB", "mouse_scroll"},
    {"MouseMiddle", "mouse_scroll"},
    {"Space", "keyboard_space"},
    {"Shift", "keyboard_shift"},
    {"LeftShift", "keyboard_shift"},
    {"RightShift", "keyboard_shift"},
    {"Control", "keyboard_ctrl"},
    {"Ctrl", "keyboard_ctrl"},
    {"LeftCtrl", "keyboard_ctrl"},
    {"RightCtrl", "keyboard_ctrl"},
    {"LeftControl", "keyboard_ctrl"},
    {"RightControl", "keyboard_ctrl"},
    {"Alt", "keyboard_alt"},
    {"LeftAlt", "keyboard_alt"},
    {"RightAlt", "keyboard_alt"},
    {"Escape", "keyboard_escape"},
    {"Esc", "keyboard_escape"},
    {"Return", "keyboard_enter"},
    {"Enter", "keyboard_enter"},
    {"NumpadEnter", "keyboard_numpad_enter"},
    {"Tab", "keyboard_tab"},
    {"Backspace", "keyboard_backspace"},
    {"Delete", "keyboard_delete"},
    {"Insert", "keyboard_insert"},
    {"Home", "keyboard_home"},
    {"End", "keyboard_end"},
    {"PageUp", "keyboard_page_up"},
    {"PageDown", "keyboard_page_down"},
    {"Left", "keyboard_arrow_left"},
    {"Up", "keyboard_arrow_up"},
    {"Right", "keyboard_arrow_right"},
    {"Down", "keyboard_arrow_down"},
    {"CapsLock", "keyboard_capslock"},
    {"NumLock", "keyboard_numlock"},
    {"PrintScreen", "keyboard_printscreen"},
    {"Backtick", "keyboard_tilde"},
    {"Minus", "keyboard_minus"},
    {"Plus", "keyboard_equals"},
    {"Comma", "keyboard_comma"},
    {"Period", "keyboard_period"},
    {"Semicolon", "keyboard_semicolon"},
    {"Slash", "keyboard_slash_forward"},
    {"Backslash", "keyboard_slash_back"},
    {"LBracket", "keyboard_bracket_open"},
    {"RBracket", "keyboard_bracket_close"},
    {"Quote", "keyboard_apostrophe"},
    {"NumpadPlus", "keyboard_numpad_plus"},
    {"NumpadStar", "keyboard_asterisk"},
};

Image KeyIcon(const std::string& key) {
  std::string name;
  auto it = kKeyIcons.find(key);
  if (it != kKeyIcons.end()) {
    name = it->second;
  } else if ((key.size() == 1 && std::isalnum(static_cast<unsigned char>(key[0]))) ||
             (key.size() >= 2 && key.size() <= 3 && key[0] == 'F' &&
              std::all_of(key.begin() + 1, key.end(), ::isdigit) && std::stoi(key.substr(1)) >= 1 &&
              std::stoi(key.substr(1)) <= 12)) {
    name = "keyboard_";
    for (char ch : key) name += char(std::tolower(static_cast<unsigned char>(ch)));
  }
  if (!name.empty() && Icon(name).w) return Icon(name);
  std::wstring text = ParseBinding(key).text;
  Image cap = Icon(text.size() > 2 ? "keyboard_space" : "keyboard_a", true);
  if (!cap.w) return cap;
  auto c = ContentBox(cap);
  float mx = (c[2] - c[0]) * 0.18f, my = (c[3] - c[1]) * 0.26f;
  const float box[4] = {c[0] + mx, c[1] + my, c[2] - mx, c[3] - my};
  Mask m = FittedText(cap.w, cap.h, text, box, kKeyFont);
  for (int y = 0; y < cap.h; ++y) {
    for (int x = 0; x < cap.w; ++x) {
      if (cap.at(x, y)[3] > 0.0f) Over(cap.at(x, y), kHoleColor, m.at(x, y));
    }
  }
  return cap;
}

Image IconRow(const std::vector<Image>& icons) {
  constexpr float kHeight = 96.0f, kGap = 8.0f;
  float width = 0.0f;
  std::vector<float> widths;
  for (const Image& icon : icons) {
    auto c = ContentBox(icon);
    widths.push_back(c[3] > c[1] ? kHeight * float(c[2] - c[0]) / float(c[3] - c[1]) : 0.0f);
    width += widths.back() + (width > 0.0f ? kGap : 0.0f);
  }
  Image row(int(std::ceil(width)), int(kHeight));
  float x = 0.0f;
  for (size_t i = 0; i < icons.size(); ++i) {
    const float box[4] = {x, 0.0f, x + widths[i], kHeight};
    Stamp(row, box, icons[i]);
    x += widths[i] + kGap;
  }
  return row;
}

Image BindingIcon(const std::string& binding) {
  Binding b = SplitBinding(binding);
  if (b.modifiers.empty()) return KeyIcon(b.key);
  std::vector<Image> icons;
  for (const std::string& m : b.modifiers) icons.push_back(KeyIcon(m));
  icons.push_back(KeyIcon(b.key));
  return IconRow(icons);
}

Image KeyCluster(const std::string* bindings) {
  Binding keys[4];
  for (int i = 0; i < 4; ++i) keys[i] = SplitBinding(bindings[i]);
  bool shared = !keys[0].modifiers.empty();
  for (int i = 1; i < 4; ++i) shared = shared && keys[i].modifiers == keys[0].modifiers;
  constexpr float kKey = 96.0f, kGap = 8.0f, kModifier = 56.0f;
  float top = shared ? kModifier + kGap : 0.0f;
  Image img(int(kKey * 3 + kGap * 2), int(top + kKey * 2 + kGap));
  if (shared) {
    std::vector<Image> icons;
    for (const std::string& m : keys[0].modifiers) icons.push_back(KeyIcon(m));
    const float box[4] = {0.0f, 0.0f, float(img.w), kModifier};
    Stamp(img, box, IconRow(icons));
  }
  const float slots[4][2] = {{kKey + kGap, top},
                             {0.0f, top + kKey + kGap},
                             {kKey + kGap, top + kKey + kGap},
                             {(kKey + kGap) * 2, top + kKey + kGap}};
  for (int i = 0; i < 4; ++i) {
    const float box[4] = {slots[i][0], slots[i][1], slots[i][0] + kKey, slots[i][1] + kKey};
    Stamp(img, box, KeyIcon(keys[i].key));
  }
  return img;
}

// White letters in a cell (bright, unsaturated, opaque pixels) and the box of
// the rows and columns holding at least two of them. False if there are none.
bool FindWhiteLetters(Cell& cell, std::vector<bool>& letters, float box[4]) {
  const int w = cell.w, h = cell.h;
  letters.assign(size_t(w) * h, false);
  int count = 0;
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const float* p = cell.at(x, y);
      bool l = Luminance(p) > 0.68f && Saturation(p) < 0.12f && p[3] > 0.5f;
      letters[size_t(y) * w + x] = l;
      count += l;
    }
  }
  if (count < 10) return false;
  box[0] = float(w);
  box[1] = float(h);
  box[2] = box[3] = -1;
  for (int y = 0; y < h; ++y) {
    int row = 0;
    for (int x = 0; x < w; ++x) row += letters[size_t(y) * w + x];
    if (row >= 2) {
      box[1] = std::min(box[1], float(y));
      box[3] = std::max(box[3], float(y + 1));
    }
  }
  for (int x = 0; x < w; ++x) {
    int col = 0;
    for (int y = 0; y < h; ++y) col += letters[size_t(y) * w + x];
    if (col >= 2) {
      box[0] = std::min(box[0], float(x));
      box[2] = std::max(box[2], float(x + 1));
    }
  }
  return box[2] >= 0 && box[3] >= 0;
}

// White outlined letters in a w x h cell, replaced by icon.
void WhiteLabel(Image& img, int x0, int y0, int w, int h, const Image& icon) {
  Cell cell{img, x0, y0, w, h};
  std::vector<bool> letters;
  float box[4];
  if (!FindWhiteLetters(cell, letters, box)) return;
  auto grown = Morph(letters, w, h, 3, true);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      size_t i = size_t(y) * w + x;
      grown[i] = grown[i] && cell.at(x, y)[3] > 0.5f;
    }
  }
  Inpaint(cell, grown);
  const float icon_box[4] = {float(x0 + 1), y0 + std::max(0.0f, box[1] - 5), float(x0 + w - 1),
                             y0 + std::min(float(h), box[3] + 5)};
  Stamp(img, icon_box, icon);
}

enum class Tilt { kNone, kUp, kDown, kLeft, kRight };
Tilt StickTilt(Image& img, int x0, int y0, int n) {
  Cell cell{img, x0, y0, n, n};
  std::vector<bool> letters;
  float box[4];
  if (!FindWhiteLetters(cell, letters, box)) return Tilt::kNone;
  float puck[4] = {float(n), float(n), -1, -1};
  for (int y = 0; y < n; ++y) {
    for (int x = 0; x < n; ++x) {
      if (cell.at(x, y)[3] <= 0.5f) continue;
      puck[0] = std::min(puck[0], float(x));
      puck[1] = std::min(puck[1], float(y));
      puck[2] = std::max(puck[2], float(x));
      puck[3] = std::max(puck[3], float(y));
    }
  }
  float dx = (box[0] + box[2]) / 2.0f - (puck[0] + puck[2] + 1) / 2.0f;
  float dy = (box[1] + box[3]) / 2.0f - (puck[1] + puck[3] + 1) / 2.0f;
  if (std::max(std::abs(dx), std::abs(dy)) <= 3.0f) return Tilt::kNone;
  if (std::abs(dx) > std::abs(dy)) return dx > 0 ? Tilt::kRight : Tilt::kLeft;
  return dy > 0 ? Tilt::kDown : Tilt::kUp;
}

constexpr int kSheetFaces[4][4] = {
    {2, 2, 41, 41}, {42, 2, 81, 41}, {82, 2, 121, 41}, {122, 2, 161, 41}};
struct SheetPlate {
  int box[4];
  const wchar_t* label;
};
constexpr SheetPlate kSheetPlates[] = {
    {{3, 93, 46, 124}, L"R1"},
    {{47, 98, 84, 135}, L"R2"},
    {{1, 131, 39, 168}, L"L2"},
    {{40, 139, 83, 168}, L"L1"},
};
struct SheetStick {
  int x, y;
  wchar_t side;
};
constexpr int kStickCell = 42;
constexpr SheetStick kSheetSticks[] = {
    {168, 6, L'L'},   {212, 3, L'L'},   {118, 54, L'R'},  {166, 54, L'R'},  {212, 54, L'R'},
    {118, 98, L'L'},  {165, 118, L'L'}, {212, 118, L'L'}, {116, 142, L'R'}, {163, 165, L'R'},
    {211, 165, L'R'}, {116, 186, L'R'}, {165, 212, L'R'}, {212, 213, L'R'},
};
constexpr int kBackBox[4] = {2, 45, 54, 90}, kStartBox[4] = {56, 46, 109, 90};
constexpr int kDpadBox[4] = {2, 173, 62, 233};

// The binding of a button named by its PlayStation label.
const std::string& BindingFor(std::wstring_view label, const KeyBindings& k) {
  if (label == L"R1") return k.right_shoulder;
  if (label == L"R2") return k.right_trigger;
  if (label == L"L1") return k.left_shoulder;
  if (label == L"L2") return k.left_trigger;
  if (label == L"L3") return k.lstick_press;
  return k.rstick_press;
}

const std::string& BindingFor(Symbol symbol, const KeyBindings& k) {
  switch (symbol) {
    case Symbol::kCross: return k.a;
    case Symbol::kCircle: return k.b;
    case Symbol::kSquare: return k.x;
    default: return k.y;
  }
}

const Image& FaceIcon(Symbol symbol) {
  switch (symbol) {
    case Symbol::kCross: return Icon("playstation_button_color_cross");
    case Symbol::kCircle: return Icon("playstation_button_color_circle");
    case Symbol::kSquare: return Icon("playstation_button_color_square");
    default: return Icon("playstation_button_color_triangle");
  }
}

const Image& PlayStationIcon(std::wstring_view label) {
  if (label == L"L3") return Icon("playstation_button_l3");
  if (label == L"R3") return Icon("playstation_button_r3");
  std::string name = "playstation_trigger_";
  for (wchar_t ch : label) name += char(std::tolower(int(ch)));
  return Icon(name);
}

const Image& StickIcon(wchar_t side, Tilt tilt) {
  static constexpr const char* kSuffix[] = {"", "_up", "_down", "_left", "_right"};
  return Icon(std::string("playstation_stick_") + (side == L'L' ? "l" : "r") + kSuffix[int(tilt)]);
}

Image StickKeyIcon(wchar_t side, Tilt tilt, const KeyBindings& k) {
  if (side == L'R' && k.mouse_look) return Icon("mouse_move");
  const std::string keys[4] = {
      side == L'L' ? k.lstick_up : k.rstick_up, side == L'L' ? k.lstick_left : k.rstick_left,
      side == L'L' ? k.lstick_down : k.rstick_down, side == L'L' ? k.lstick_right : k.rstick_right};
  switch (tilt) {
    case Tilt::kUp: return BindingIcon(keys[0]);
    case Tilt::kLeft: return BindingIcon(keys[1]);
    case Tilt::kDown: return BindingIcon(keys[2]);
    case Tilt::kRight: return BindingIcon(keys[3]);
    default: return KeyCluster(keys);
  }
}

Image MakePlayStationSheet(const Image& xbox, GlyphStyle style) {
  Image img = xbox;
  const Symbol faces[4] = {Symbol::kCross, Symbol::kCircle, Symbol::kSquare, Symbol::kTriangle};
  for (int i = 0; i < 4; ++i) ReplaceArt(img, kSheetFaces[i], FaceIcon(faces[i]));
  for (const SheetPlate& p : kSheetPlates) ReplaceArt(img, p.box, PlayStationIcon(p.label));
  bool ps5 = style == GlyphStyle::kPlayStation5;
  ReplaceArt(img, kBackBox, Icon(ps5 ? "playstation5_button_create" : "playstation4_button_share"));
  ReplaceArt(img, kStartBox,
             Icon(ps5 ? "playstation5_button_options" : "playstation4_button_options"));
  std::vector<Tilt> tilts;
  for (const SheetStick& s : kSheetSticks) tilts.push_back(StickTilt(img, s.x, s.y, kStickCell));
  for (size_t i = 0; i < tilts.size(); ++i) {
    const SheetStick& s = kSheetSticks[i];
    const int box[4] = {s.x, s.y, s.x + kStickCell, s.y + kStickCell};
    ReplaceArt(img, box, StickIcon(s.side, tilts[i]));
  }
  ReplaceArt(img, kDpadBox, Icon("playstation_dpad"));
  return img;
}

Image MakeKeyboardSheet(const Image& xbox, const KeyBindings& k) {
  Image img = xbox;
  const std::string* faces[4] = {&k.a, &k.b, &k.x, &k.y};
  for (int i = 0; i < 4; ++i) ReplaceArt(img, kSheetFaces[i], BindingIcon(*faces[i]), kKeySize);
  for (const SheetPlate& p : kSheetPlates) {
    ReplaceArt(img, p.box, BindingIcon(BindingFor(p.label, k)), 0.9f);
  }
  ReplaceArt(img, kBackBox, BindingIcon(k.back), kKeySize);
  ReplaceArt(img, kStartBox, BindingIcon(k.start), kKeySize);
  std::vector<Tilt> tilts;
  for (const SheetStick& s : kSheetSticks) tilts.push_back(StickTilt(img, s.x, s.y, kStickCell));
  for (size_t i = 0; i < tilts.size(); ++i) {
    const SheetStick& s = kSheetSticks[i];
    const int box[4] = {s.x, s.y, s.x + kStickCell, s.y + kStickCell};
    bool cluster = tilts[i] == Tilt::kNone && !(s.side == L'R' && k.mouse_look);
    ReplaceArt(img, box, StickKeyIcon(s.side, tilts[i], k), cluster ? 1.0f : kKeySize);
  }
  const std::string dpad[4] = {k.dpad_up, k.dpad_left, k.dpad_down, k.dpad_right};
  ReplaceArt(img, kDpadBox, KeyCluster(dpad));
  return img;
}

void SwapRedBlue(Image& img) {
  for (size_t i = 0; i < img.px.size(); i += 4) std::swap(img.px[i], img.px[i + 2]);
}

// Average colour of the opaque ring (radii r0..r1) around a sphere's letter.
std::array<float, 3> RingColor(const Image& img, float cx, float cy, float r0, float r1) {
  std::array<float, 3> sum = {};
  int n = 0;
  for (int y = int(cy - r1); y <= int(cy + r1) + 1; ++y) {
    for (int x = int(cx - r1); x <= int(cx + r1) + 1; ++x) {
      if (x < 0 || y < 0 || x >= img.w || y >= img.h) continue;
      float d = std::hypot(x - cx, y - cy);
      const float* p = img.at(x, y);
      if (d < r0 || d > r1 || p[3] < 0.5f) continue;
      for (int c = 0; c < 3; ++c) sum[c] += p[c];
      ++n;
    }
  }
  if (n) {
    for (float& c : sum) c /= float(n);
  }
  return sum;
}

using Rgb = std::array<float, 3>;
bool IsGreen(const Rgb& c) { return c[1] > c[0] + 0.05f && c[1] > c[2] + 0.15f; }
bool IsRed(const Rgb& c) { return c[0] > c[1] + 0.15f && c[0] > c[2] + 0.15f; }
bool IsBlue(const Rgb& c) { return c[2] > c[0] + 0.15f && c[2] > c[1] + 0.1f; }
bool IsYellow(const Rgb& c) { return c[0] > c[2] + 0.2f && c[1] > c[2] + 0.15f; }

// The game's button sheet: 512x256 DXT5 with green / red / blue / yellow
// spheres in the A / B / X / Y cells and transparent space around them.
bool IsButtonSheet(const Image& img) {
  if (img.w != 512 || img.h != 256) return false;
  bool spheres = IsGreen(RingColor(img, 21, 21, 10, 16)) && IsRed(RingColor(img, 61, 21, 10, 16)) &&
                 IsBlue(RingColor(img, 101, 21, 10, 16)) &&
                 IsYellow(RingColor(img, 141, 21, 10, 16));
  bool gap = img.at(0, 0)[3] < 0.1f && img.at(41, 21)[3] < 0.2f;
  return spheres && gap;
}

// Glyphs in other HUD textures

// One change to a texture: a face button sphere, a shoulder plate or a white
// text label.
struct Edit {
  enum class Kind { kSphere, kPlate, kLabel } kind = Kind::kSphere;
  int box[4] = {};      // x0, y0, x1, y1
  int letters[4] = {};  // plates: the letters' box
  Symbol symbol = Symbol::kCross;
  std::wstring label;
  float letter_lum = 0.22f;  // spheres: letter pixels are darker than this
};

struct Component {
  int x0, y0, x1, y1;  // x1 / y1 exclusive
  int count;
};

// 4-connected regions of alpha > 0.5.
std::vector<Component> OpaqueComponents(const Image& img) {
  std::vector<int> label(size_t(img.w) * img.h, -1);
  std::vector<Component> out;
  std::vector<int> stack;
  for (int sy = 0; sy < img.h; ++sy) {
    for (int sx = 0; sx < img.w; ++sx) {
      size_t si = size_t(sy) * img.w + sx;
      if (label[si] >= 0 || img.at(sx, sy)[3] <= 0.5f) continue;
      Component c{sx, sy, sx + 1, sy + 1, 0};
      label[si] = int(out.size());
      stack.assign(1, int(si));
      while (!stack.empty()) {
        int i = stack.back();
        stack.pop_back();
        int x = i % img.w, y = i / img.w;
        ++c.count;
        c.x0 = std::min(c.x0, x);
        c.y0 = std::min(c.y0, y);
        c.x1 = std::max(c.x1, x + 1);
        c.y1 = std::max(c.y1, y + 1);
        const int nx[4] = {x - 1, x + 1, x, x}, ny[4] = {y, y, y - 1, y + 1};
        for (int k = 0; k < 4; ++k) {
          if (nx[k] < 0 || ny[k] < 0 || nx[k] >= img.w || ny[k] >= img.h) continue;
          size_t j = size_t(ny[k]) * img.w + nx[k];
          if (label[j] >= 0 || img.at(nx[k], ny[k])[3] <= 0.5f) continue;
          label[j] = int(out.size());
          stack.push_back(int(j));
        }
      }
      out.push_back(c);
    }
  }
  return out;
}

// A coloured sphere or coin (disc plus its lower edge) with a dark letter:
// A (green), B (red), X (blue), Y (yellow).
bool FindSphere(const Image& img, const Component& c, Edit& edit) {
  int w = c.x1 - c.x0, h = c.y1 - c.y0;
  if (w < 36 || w > 80 || h < w - 3 || h * 10 > w * 13) return false;
  float fill = float(c.count) / float(w * h);
  if (fill < 0.68f || fill > 0.92f) return false;
  int n = w;
  if (n > img.h) return false;
  float cx = c.x0 + n / 2.0f - 0.5f, cy = c.y0 + n / 2.0f - 0.5f;
  Rgb ring = RingColor(img, cx, cy, n * 0.26f, n * 0.41f);
  Symbol symbol;
  if (IsGreen(ring)) {
    symbol = Symbol::kCross;
  } else if (IsRed(ring)) {
    symbol = Symbol::kCircle;
  } else if (IsBlue(ring)) {
    symbol = Symbol::kSquare;
  } else if (IsYellow(ring)) {
    symbol = Symbol::kTriangle;
  } else {
    return false;
  }
  float letter_lum = Luminance(ring.data()) * 0.6f;
  int dark = 0, total = 0;
  for (int y = c.y0; y < std::min(c.y0 + n, img.h); ++y) {
    for (int x = c.x0; x < c.x1; ++x) {
      const float* p = img.at(x, y);
      if (std::hypot(x - cx, y - cy) > n * 0.25f || p[3] <= 0.5f) continue;
      ++total;
      dark += Luminance(p) < letter_lum;
    }
  }
  if (!total || dark * 10 < total) return false;
  int y0 = std::min(c.y0, img.h - n);
  edit = Edit{Edit::Kind::kSphere, {c.x0, y0, c.x1, c.y1}, {}, symbol, {}};
  edit.letter_lum = letter_lum;
  return true;
}

// Dark letters of a w x h mask: 8-connected parts at least 60% as tall as the
// tallest (drops cracks and bevel fragments); their pixels, box and count.
void ExtractLetters(const std::vector<bool>& dark, int w, int h, std::vector<bool>& letters,
                    int lb[4], int& count) {
  struct Part {
    int x0, y0, x1, y1;
    std::vector<int> pixels;
  };
  std::vector<Part> parts;
  std::vector<bool> seen(dark.size());
  for (int i = 0; i < int(dark.size()); ++i) {
    if (!dark[i] || seen[i]) continue;
    Part part{w, h, -1, -1, {}};
    std::vector<int> stack{i};
    seen[i] = true;
    while (!stack.empty()) {
      int j = stack.back();
      stack.pop_back();
      int x = j % w, y = j / w;
      part.pixels.push_back(j);
      part.x0 = std::min(part.x0, x);
      part.y0 = std::min(part.y0, y);
      part.x1 = std::max(part.x1, x + 1);
      part.y1 = std::max(part.y1, y + 1);
      for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
          int nx = x + dx, ny = y + dy;
          if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
          int k = ny * w + nx;
          if (!dark[k] || seen[k]) continue;
          seen[k] = true;
          stack.push_back(k);
        }
      }
    }
    parts.push_back(std::move(part));
  }
  int tallest = 0;
  for (const Part& p : parts) tallest = std::max(tallest, p.y1 - p.y0);
  letters.assign(dark.size(), false);
  lb[0] = w;
  lb[1] = h;
  lb[2] = lb[3] = -1;
  count = 0;
  for (const Part& p : parts) {
    if ((p.y1 - p.y0) * 10 < tallest * 6) continue;
    for (int j : p.pixels) letters[j] = true;
    count += int(p.pixels.size());
    lb[0] = std::min(lb[0], p.x0);
    lb[1] = std::min(lb[1], p.y0);
    lb[2] = std::max(lb[2], p.x1);
    lb[3] = std::max(lb[3], p.y1);
  }
}

// Reads two shoulder button letters (RB / LB / RT / LT) in box lb of a w-wide
// letter mask; returns the PlayStation label (R1 / L1 / R2 / L2) or null.
const wchar_t* ReadShoulderLetters(const std::vector<bool>& letters, int w, const int lb[4]) {
  int lw = lb[2] - lb[0], lh = lb[3] - lb[1];
  if (lw < 8 || lh < 6) return nullptr;
  // Split the two letters at the emptiest column of the middle half.
  auto column = [&](int x) {
    int n = 0;
    for (int y = lb[1]; y < lb[3]; ++y) n += letters[size_t(y) * w + x];
    return n;
  };
  int split = lb[0] + lw / 2, fewest = lh + 1;
  for (int x = lb[0] + lw / 4; x < lb[2] - lw / 4; ++x) {
    if (column(x) < fewest) {
      fewest = column(x);
      split = x;
    }
  }
  // Fraction of letter pixels of [x0, x1) x [y0, y1).
  auto ink = [&](int x0, int y0, int x1, int y1) {
    int n = 0, total = 0;
    for (int y = y0; y < y1; ++y) {
      for (int x = x0; x < x1; ++x) {
        ++total;
        n += letters[size_t(y) * w + x];
      }
    }
    return total ? float(n) / float(total) : 0.0f;
  };
  // R has its bowl at the upper right; L is empty there.
  int first_w = split - lb[0];
  bool r = ink(lb[0] + first_w * 3 / 5, lb[1] + lh / 6, split, lb[1] + lh * 2 / 5) > 0.12f;
  // B's lower half spans most of its width (the bowl); T's is a narrow stem.
  int second_x = split + 1;
  while (second_x < lb[2] && !column(second_x)) ++second_x;
  int second_w = lb[2] - second_x;
  if (first_w < 3 || second_w < 3) return nullptr;
  int wide = 0, rows = 0;
  for (int y = lb[1] + lh / 2; y < lb[3]; ++y, ++rows) {
    int left = -1, right = -1;
    for (int x = second_x; x < lb[2]; ++x) {
      if (!letters[size_t(y) * w + x]) continue;
      if (left < 0) left = x;
      right = x;
    }
    wide += left >= 0 && (right - left + 1) * 2 >= second_w;
  }
  bool b = rows && wide * 2 > rows;
  return r ? (b ? L"R1" : L"R2") : (b ? L"L1" : L"L2");
}

// A light stone plate with dark shoulder button letters (RB / LB / RT / LT).
bool FindPlate(const Image& img, const Component& c, Edit& edit) {
  int w = c.x1 - c.x0, h = c.y1 - c.y0;
  if (w < 30 || w > 160 || h < 14 || w * 2 < h * 3 || w > h * 4) return false;
  std::vector<bool> opaque(size_t(w) * h);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) opaque[size_t(y) * w + x] = img.at(c.x0 + x, c.y0 + y)[3] > 0.9f;
  }
  auto interior = Morph(opaque, w, h, 7, false);
  int inside = 0;
  float lum = 0;
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      if (!interior[size_t(y) * w + x]) continue;
      ++inside;
      lum += Luminance(img.at(c.x0 + x, c.y0 + y));
    }
  }
  if (inside < w * h * 2 / 5 || lum / float(inside) < 0.5f) return false;
  // Dark pixels inside, without long horizontal runs (bevel shadows).
  std::vector<bool> dark(size_t(w) * h);
  for (int y = 0; y < h; ++y) {
    auto is_dark = [&](int x) {
      return interior[size_t(y) * w + x] && Luminance(img.at(c.x0 + x, c.y0 + y)) < 0.5f;
    };
    for (int x = 0; x < w;) {
      if (!is_dark(x)) {
        ++x;
        continue;
      }
      int end = x;
      while (end < w && is_dark(end)) ++end;
      if ((end - x) * 10 <= w * 3) {
        for (int i = x; i < end; ++i) dark[size_t(y) * w + i] = true;
      }
      x = end;
    }
  }
  std::vector<bool> letters;
  int lb[4], count = 0;
  ExtractLetters(dark, w, h, letters, lb, count);
  int lw = lb[2] - lb[0], lh = lb[3] - lb[1];
  if (lw < 8 || lh < 6 || lw > w * 3 / 4 || count * 25 < inside || count * 5 > inside * 2) {
    return false;
  }
  const wchar_t* label = ReadShoulderLetters(letters, w, lb);
  if (!label) return false;
  edit = Edit{Edit::Kind::kPlate,
              {c.x0, c.y0, c.x1, c.y1},
              {c.x0 + lb[0], c.y0 + lb[1], c.x0 + lb[2], c.y0 + lb[3]},
              Symbol::kCross,
              label};
  return true;
}

// The execution prompt's 3D trigger button
//
// A stone button model over the enemy: a diffuse with two large dark serif
// letters (RT) and DXT5nm normal / specular maps with the letters engraved.
// The old letters are covered with stone from above them in every map, and
// the new label is painted and engraved the way the old one was.

// Separable Gaussian blur of a w x h field.
std::vector<float> Blur(const std::vector<float>& in, int w, int h, float sigma) {
  int r = std::max(1, int(std::ceil(sigma * 3.0f)));
  std::vector<float> kernel(size_t(2 * r + 1));
  float sum = 0.0f;
  for (int i = -r; i <= r; ++i) {
    kernel[size_t(i + r)] = std::exp(-0.5f * float(i * i) / (sigma * sigma));
    sum += kernel[size_t(i + r)];
  }
  for (float& k : kernel) k /= sum;
  std::vector<float> tmp(in.size()), out(in.size());
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      float acc = 0.0f;
      for (int i = -r; i <= r; ++i) {
        acc += in[size_t(y) * w + std::clamp(x + i, 0, w - 1)] * kernel[size_t(i + r)];
      }
      tmp[size_t(y) * w + x] = acc;
    }
  }
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      float acc = 0.0f;
      for (int i = -r; i <= r; ++i) {
        acc += tmp[size_t(std::clamp(y + i, 0, h - 1)) * w + x] * kernel[size_t(i + r)];
      }
      out[size_t(y) * w + x] = acc;
    }
  }
  return out;
}

// Central differences of a w x h field.
void Gradient(const std::vector<float>& f, int w, int h, std::vector<float>& gx,
              std::vector<float>& gy) {
  gx.assign(f.size(), 0.0f);
  gy.assign(f.size(), 0.0f);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      int x0 = std::max(x - 1, 0), x1 = std::min(x + 1, w - 1);
      int y0 = std::max(y - 1, 0), y1 = std::min(y + 1, h - 1);
      gx[size_t(y) * w + x] =
          (f[size_t(y) * w + x1] - f[size_t(y) * w + x0]) / float(std::max(x1 - x0, 1));
      gy[size_t(y) * w + x] =
          (f[size_t(y1) * w + x] - f[size_t(y0) * w + x]) / float(std::max(y1 - y0, 1));
    }
  }
}

std::vector<float> ToField(const std::vector<bool>& mask) {
  std::vector<float> f(mask.size());
  for (size_t i = 0; i < mask.size(); ++i) f[i] = mask[i] ? 1.0f : 0.0f;
  return f;
}

struct Engraving {
  int w = 0, h = 0;
  std::vector<bool> letters;  // the old letters
  int box[4] = {};
  int shift = 0;                         // rows down to the letter-free stone that covers them
  std::wstring label;                    // PlayStation label (R2 ...)
  float ink[3] = {}, highlight[3] = {};  // the letters' paint and bevel highlight
};

bool FindEngraving(const Image& img, Engraving& out) {
  int w = img.w, h = img.h;
  if (w != h || w < 128 || w > 512) return false;
  std::vector<bool> dark(size_t(w) * h);
  float lum = 0.0f;
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      float l = Luminance(img.at(x, y));
      lum += l;
      dark[size_t(y) * w + x] = l < 0.25f;
    }
  }
  if (lum / float(w * h) < 0.3f) return false;
  int lb[4], count = 0;
  ExtractLetters(dark, w, h, out.letters, lb, count);
  int lw = lb[2] - lb[0], lh = lb[3] - lb[1];
  if (lh * 5 < h || lh * 5 > h * 3 || lw * 10 < w * 3 || lw * 5 > w * 4) return false;
  int cx = (lb[0] + lb[2]) / 2, cy = (lb[1] + lb[3]) / 2;
  if (cx * 4 < w || cx * 4 > w * 3 || cy * 10 < h * 3 || cy * 10 > h * 7) return false;
  if (count * 33 < lw * lh || count * 2 > lw * lh) return false;
  const wchar_t* label = ReadShoulderLetters(out.letters, w, lb);
  if (!label) return false;
  out.w = w;
  out.h = h;
  std::copy(lb, lb + 4, out.box);
  out.shift = lh + 8;
  out.label = label;
  // Paint: the mean of the letters; highlight: the brightest twelfth of them.
  std::vector<float> lums;
  float ink[3] = {};
  int n = 0;
  auto grown = Morph(out.letters, w, h, 3, true);
  for (int i = 0; i < w * h; ++i) {
    const float* p = &img.px[size_t(i) * 4];
    if (out.letters[i]) {
      for (int c = 0; c < 3; ++c) ink[c] += p[c];
      ++n;
    }
    if (grown[i]) lums.push_back(Luminance(p));
  }
  for (int c = 0; c < 3; ++c) out.ink[c] = ink[c] / float(std::max(n, 1));
  std::sort(lums.begin(), lums.end());
  float bright = lums.empty() ? 0.6f : lums[lums.size() - 1 - lums.size() / 12];
  for (int c = 0; c < 3; ++c) out.highlight[c] = bright;
  return true;
}

// The old letters blurred by `sigma`, its gradients, and the pixels near the
// letters that fits look at.
struct LetterField {
  std::vector<int> band;
  std::vector<float> f, gx, gy;
};

LetterField MakeLetterField(const Engraving& e, float sigma) {
  LetterField field;
  auto band = Morph(e.letters, e.w, e.h, 13, true);
  for (int i = 0; i < e.w * e.h; ++i) {
    if (band[i]) field.band.push_back(i);
  }
  field.f = Blur(ToField(e.letters), e.w, e.h, sigma);
  Gradient(field.f, e.w, e.h, field.gx, field.gy);
  return field;
}

// How much of channel c's variation around the old letters their shape
// explains (R^2 of v = a * gx + b * gy + c * f + d); a and b out.
float EngravingFit(const Image& img, const LetterField& field, int c, float* k_x, float* k_y) {
  double ata[4][4] = {}, atb[4] = {}, mean = 0.0;
  for (int i : field.band) {
    double row[4] = {field.gx[i], field.gy[i], field.f[i], 1.0}, v = img.px[size_t(i) * 4 + c];
    for (int r = 0; r < 4; ++r) {
      for (int q = 0; q < 4; ++q) ata[r][q] += row[r] * row[q];
      atb[r] += row[r] * v;
    }
    mean += v;
  }
  if (field.band.empty()) return 0.0f;
  mean /= double(field.band.size());
  // Gaussian elimination with partial pivoting.
  double m[4][5];
  for (int r = 0; r < 4; ++r) {
    for (int q = 0; q < 4; ++q) m[r][q] = ata[r][q];
    m[r][4] = atb[r];
  }
  for (int col = 0; col < 4; ++col) {
    int pivot = col;
    for (int r = col + 1; r < 4; ++r) {
      if (std::abs(m[r][col]) > std::abs(m[pivot][col])) pivot = r;
    }
    if (std::abs(m[pivot][col]) < 1e-12) return 0.0f;
    std::swap(m[col], m[pivot]);
    for (int r = 0; r < 4; ++r) {
      if (r == col) continue;
      double factor = m[r][col] / m[col][col];
      for (int q = col; q < 5; ++q) m[r][q] -= factor * m[col][q];
    }
  }
  double coef[4];
  for (int r = 0; r < 4; ++r) coef[r] = m[r][4] / m[r][r];
  double total = 0.0, residual = 0.0;
  for (int i : field.band) {
    double v = img.px[size_t(i) * 4 + c];
    double fit = coef[0] * field.gx[i] + coef[1] * field.gy[i] + coef[2] * field.f[i] + coef[3];
    total += (v - mean) * (v - mean);
    residual += (v - fit) * (v - fit);
  }
  if (k_x) *k_x = float(coef[0]);
  if (k_y) *k_y = float(coef[1]);
  return total > 1e-9 ? float(1.0 - residual / total) : 0.0f;
}

// Engraving of a DXT5nm normal map (y in green, x in alpha): bevel width and
// depth of the old letters, to engrave the new ones the same way.
struct NormalEngraving {
  float sigma = 2.5f;
  float k = 1.7f;  // x strength, also used for y (engravings are isotropic)
  float y_sign = 1.0f;
};

NormalEngraving FitNormalEngraving(const Image& img, const Engraving& e) {
  NormalEngraving best;
  float best_r2 = -1.0f;
  for (float sigma : {1.0f, 1.6f, 2.0f, 2.5f, 3.0f, 4.0f}) {
    LetterField field = MakeLetterField(e, sigma);
    float kx = 0.0f, ky = 0.0f, unused = 0.0f;
    float r2 = EngravingFit(img, field, 3, &kx, &unused);
    EngravingFit(img, field, 1, &unused, &ky);
    if (r2 > best_r2 && std::abs(kx) > 0.2f) {
      best_r2 = r2;
      best = {sigma, kx, ky < 0.0f ? -1.0f : 1.0f};
    }
  }
  return best;
}

// Covers the old letters with the stone `shift` rows above them, blended in
// with a soft edge.
void CoverLetters(Image& img, const Engraving& e) {
  int w = e.w, h = e.h;
  auto region = Morph(e.letters, w, h, 15, true);
  std::vector<float> weight = Blur(ToField(region), w, h, 2.0f);
  Image src = img;
  for (int y = 0; y < h; ++y) {
    int sy = ((y - e.shift) % h + h) % h;
    for (int x = 0; x < w; ++x) {
      float a = std::min(weight[size_t(y) * w + x] * 1.5f, 1.0f);
      if (a <= 0.0f) continue;
      float* p = img.at(x, y);
      const float* q = src.at(x, sy);
      for (int c = 0; c < 4; ++c) p[c] = p[c] * (1.0f - a) + q[c] * a;
    }
  }
}

// The new label's coverage in the old letters' box.
Mask EngravedLabel(const Engraving& e, const KeyLabel& label) {
  const float box[4] = {float(e.box[0]), float(e.box[1]), float(e.box[2]), float(e.box[3])};
  return label.mouse >= 0 ? MouseMask(e.w, e.h, box, label.mouse)
                          : FittedText(e.w, e.h, label.text, box);
}

void RelabelDiffuse(Image& img, const Engraving& e, const Mask& label) {
  CoverLetters(img, e);
  // Glossy paint: the bevel facing the upper left catches the light.
  std::vector<float> soft = Blur(label.v, e.w, e.h, 1.6f), gx, gy;
  Gradient(soft, e.w, e.h, gx, gy);
  for (int i = 0; i < e.w * e.h; ++i) {
    float a = label.v[i];
    if (a <= 0.0f) continue;
    float light = std::clamp((gx[i] + gy[i]) * 0.7f * 6.0f, 0.0f, 1.0f);
    float* p = &img.px[size_t(i) * 4];
    for (int c = 0; c < 3; ++c) {
      float paint = e.ink[c] * (1.0f - light) + e.highlight[c] * light;
      p[c] = p[c] * (1.0f - a) + paint * a;
    }
  }
}

void RelabelNormal(Image& img, const Engraving& e, const NormalEngraving& fit, const Mask& label) {
  CoverLetters(img, e);
  std::vector<float> f = Blur(label.v, e.w, e.h, fit.sigma), gx, gy;
  Gradient(f, e.w, e.h, gx, gy);
  for (int i = 0; i < e.w * e.h; ++i) {
    float* p = &img.px[size_t(i) * 4];
    p[1] = std::clamp(p[1] + fit.y_sign * fit.k * gy[i], 0.0f, 1.0f);
    p[3] = std::clamp(p[3] + fit.k * gx[i], 0.0f, 1.0f);
  }
}

// The execution prompt's trigger button (RT) by content hash: its diffuse,
// normal and specular maps as seen in game. Only this model is known; another
// button model (LT, a different size) would need its hashes added here.
enum class ButtonMap { kNone, kDiffuse, kNormal, kSpecular };
struct TriggerMap {
  uint64_t hash;
  ButtonMap map;
};
constexpr TriggerMap kTriggerButtonMaps[] = {
    {0x6BBDE8FA78C8880Cull, ButtonMap::kDiffuse},
    {0xCF6492981EE4E327ull, ButtonMap::kNormal},
    {0x1C593B85D0F8126Dull, ButtonMap::kSpecular},
};

// The Fury meter bar: "LS RS" (click both sticks) in white under its right end.
constexpr uint64_t kFuryBarHash = 0x2AAA2B5F56BF031Aull;

std::vector<Edit> FindGlyphs(const Image& img, uint64_t content_hash) {
  std::vector<Edit> edits;
  if (content_hash == kFuryBarHash && img.w == 832 && img.h == 80) {
    edits.push_back(Edit{Edit::Kind::kLabel, {752, 40, 792, 80}, {}, Symbol::kCross, L"L3"});
    edits.push_back(Edit{Edit::Kind::kLabel, {792, 40, 832, 80}, {}, Symbol::kCross, L"R3"});
    return edits;
  }
  for (const Component& c : OpaqueComponents(img)) {
    Edit edit;
    if (FindSphere(img, c, edit) || FindPlate(img, c, edit)) edits.push_back(edit);
  }
  if (edits.size() == 1 && edits[0].kind == Edit::Kind::kSphere) edits.clear();
  return edits;
}

Image ApplyEdits(const Image& xbox, const std::vector<Edit>& edits, GlyphStyle style,
                 const KeyBindings& k) {
  bool keyboard = style == GlyphStyle::kKeyboard;
  Image img = xbox;
  for (const Edit& e : edits) {
    switch (e.kind) {
      case Edit::Kind::kSphere: {
        int n = e.box[2] - e.box[0];
        ClearArt(img, e.box);
        const int disc[4] = {e.box[0], e.box[1], e.box[0] + n, e.box[1] + n};
        if (keyboard) {
          Stamp(img, disc, BindingIcon(BindingFor(e.symbol, k)), kKeySize);
        } else {
          Stamp(img, disc, FaceIcon(e.symbol));
        }
        break;
      }
      case Edit::Kind::kPlate:
        ReplaceArt(img, e.box,
                   keyboard ? BindingIcon(BindingFor(e.label, k)) : PlayStationIcon(e.label),
                   keyboard ? 0.9f : 1.0f);
        break;
      case Edit::Kind::kLabel:
        WhiteLabel(img, e.box[0], e.box[1], e.box[2] - e.box[0], e.box[3] - e.box[1],
                   keyboard ? BindingIcon(BindingFor(e.label, k)) : PlayStationIcon(e.label));
        break;
    }
  }
  return img;
}

std::string Describe(const std::vector<Edit>& edits) {
  std::string out;
  for (const Edit& e : edits) {
    if (!out.empty()) out += ", ";
    if (e.kind == Edit::Kind::kSphere) {
      const char* names[4] = {"cross", "circle", "square", "triangle"};
      out += names[int(e.symbol)];
    } else {
      for (wchar_t ch : e.label) out += char(ch);
    }
  }
  return out;
}

}  // namespace

struct ButtonGlyphs::Impl {
  dl::IRenderDevice* device = nullptr;
  GlyphStyle style = GlyphStyle::kXbox;
  // A texture with Xbox glyphs: the button sheet, or HUD art with edits.
  struct Entry {
    Image xbox;  // in display (RGB) order
    uint32_t levels = 1;
    bool red_blue_swapped = false;  // the texture stores BGR
    bool sheet = false;
    std::vector<Edit> edits;
    // A map of the 3D trigger button, and the normal map's engraving fit.
    ButtonMap map = ButtonMap::kNone;
    NormalEngraving normal;
    bool normal_fitted = false;
    dl::RefCntAutoPtr<dl::ITexture> versions[3];  // PS4, PS5, keyboard
    uint32_t keyboard_generation = 0;             // bindings the keyboard version shows
  };
  std::unordered_map<uint64_t, Entry> entries;
  std::unordered_map<uint64_t, bool> checked;
  KeyBindings bindings;
  uint32_t bindings_generation = 1;
  // The trigger button's letters, measured from its diffuse.
  std::optional<Engraving> trigger;

  // alpha_weighted: colour averaged by alpha in the mips (UI art); off for
  // data in alpha (normal maps).
  dl::RefCntAutoPtr<dl::ITexture> Upload(const Image& img, uint32_t levels,
                                         bool alpha_weighted = true) {
    // RGBA8 with box-filtered mips.
    std::vector<std::vector<uint8_t>> data;
    std::vector<dl::TextureSubResData> subresources;
    Image level = img;
    for (uint32_t l = 0; l < levels; ++l) {
      std::vector<uint8_t> bytes(size_t(level.w) * level.h * 4);
      for (size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = uint8_t(std::clamp(level.px[i], 0.0f, 1.0f) * 255.0f + 0.5f);
      }
      data.push_back(std::move(bytes));
      if (l + 1 < levels) {
        Image next(std::max(level.w / 2, 1), std::max(level.h / 2, 1));
        for (int y = 0; y < next.h; ++y) {
          for (int x = 0; x < next.w; ++x) {
            float acc[4] = {};
            for (int sy = 0; sy < 2; ++sy) {
              for (int sx = 0; sx < 2; ++sx) {
                const float* p =
                    level.at(std::min(x * 2 + sx, level.w - 1), std::min(y * 2 + sy, level.h - 1));
                float weight = alpha_weighted ? p[3] : 1.0f;
                for (int c = 0; c < 3; ++c) acc[c] += p[c] * weight;
                acc[3] += p[3];
              }
            }
            float* q = next.at(x, y);
            float total = alpha_weighted ? acc[3] : 4.0f;
            for (int c = 0; c < 3; ++c) q[c] = total > 0 ? acc[c] / total : 0.0f;
            q[3] = acc[3] / 4.0f;
          }
        }
        level = std::move(next);
      }
    }
    int w = img.w;
    for (uint32_t l = 0; l < levels; ++l) {
      subresources.push_back(
          dl::TextureSubResData{data[l].data(), uint64_t(std::max(w >> l, 1)) * 4});
    }
    dl::TextureDesc desc;
    desc.Name = "ButtonGlyphs";
    desc.Type = dl::RESOURCE_DIM_TEX_2D_ARRAY;
    desc.Width = uint32_t(img.w);
    desc.Height = uint32_t(img.h);
    desc.ArraySize = 1;
    desc.MipLevels = levels;
    desc.Format = dl::TEX_FORMAT_RGBA8_UNORM;
    desc.BindFlags = dl::BIND_SHADER_RESOURCE;
    desc.Usage = dl::USAGE_IMMUTABLE;
    dl::TextureData init{subresources.data(), uint32_t(subresources.size())};
    dl::RefCntAutoPtr<dl::ITexture> texture;
    device->CreateTexture(desc, &init, &texture);
    return texture;
  }
};

ButtonGlyphs::ButtonGlyphs(dl::IRenderDevice* device) : impl_(std::make_unique<Impl>()) {
  impl_->device = device;
}

ButtonGlyphs::~ButtonGlyphs() = default;

void ButtonGlyphs::SetStyle(GlyphStyle style) { impl_->style = style; }

void ButtonGlyphs::SetBindings(const KeyBindings& bindings) {
  if (bindings == impl_->bindings) return;
  impl_->bindings = bindings;
  ++impl_->bindings_generation;
}

dl::ITexture* ButtonGlyphs::Replace(uint64_t content_hash,
                                    const TextureCache::LoadedTexture* loaded) {
  Impl& m = *impl_;
  if (loaded && loaded->level0 && !m.checked.count(content_hash)) {
    bool found = false;
    using rex::graphics::xenos::TextureFormat;
    bool dxt5 = loaded->guest_format == uint32_t(TextureFormat::k_DXT4_5) ||
                loaded->guest_format == uint32_t(TextureFormat::k_DXT4_5_AS_16_16_16_16);
    if (dxt5 && loaded->width == 512 && loaded->height == 256) {
      Image img = DecodeBc3(loaded->level0, int(loaded->width), int(loaded->height));
      // The stored channel order may be BGR (the fetch swizzle restores it).
      bool swapped = false;
      if (!IsButtonSheet(img)) {
        SwapRedBlue(img);
        swapped = IsButtonSheet(img);
        if (!swapped) SwapRedBlue(img);
      }
      if (swapped || IsButtonSheet(img)) {
        found = true;
        Impl::Entry& e = m.entries[content_hash];
        e.xbox = std::move(img);
        e.levels = loaded->levels;
        e.red_blue_swapped = swapped;
        e.sheet = true;
        REXLOG_INFO("GLYPHS: button sheet found (content hash {:016X}, {} levels{})", content_hash,
                    loaded->levels, swapped ? ", BGR" : "");
      }
    }
    // HUD art (single level, as the UI player loads it).
    if (!found && dxt5 && loaded->levels == 1 && loaded->width >= 28 && loaded->height >= 14 &&
        loaded->width <= 2048 && loaded->height <= 2048) {
      Image img = DecodeBc3(loaded->level0, int(loaded->width), int(loaded->height));
      std::vector<Edit> edits = FindGlyphs(img, content_hash);
      if (!edits.empty()) {
        found = true;
        REXLOG_INFO("GLYPHS: {}x{} texture {:016X}: {}", img.w, img.h, content_hash,
                    Describe(edits));
        Impl::Entry& e = m.entries[content_hash];
        e.xbox = std::move(img);
        e.edits = std::move(edits);
      }
    }
    // Maps of the execution prompt's trigger button.
    for (const TriggerMap& t : kTriggerButtonMaps) {
      if (found || t.hash != content_hash || loaded->width != loaded->height) continue;
      bool dxt1 = loaded->guest_format == uint32_t(TextureFormat::k_DXT1) ||
                  loaded->guest_format == uint32_t(TextureFormat::k_DXT1_AS_16_16_16_16);
      if (!dxt1 && !dxt5) continue;
      int size = int(loaded->width);
      Image img =
          dxt1 ? DecodeBc1(loaded->level0, size, size) : DecodeBc3(loaded->level0, size, size);
      if (t.map == ButtonMap::kDiffuse) {
        Engraving engraving;
        if (!FindEngraving(img, engraving)) {
          REXLOG_WARN("GLYPHS: trigger button {:016X}: letters not found", content_hash);
          continue;
        }
        m.trigger = std::move(engraving);
      }
      found = true;
      Impl::Entry& e = m.entries[content_hash];
      e.map = t.map;
      e.levels = loaded->levels;
      e.xbox = std::move(img);
      const char* names[] = {"", "diffuse", "normal", "specular"};
      REXLOG_INFO("GLYPHS: trigger button {} map {:016X}", names[int(t.map)], content_hash);
    }
    m.checked[content_hash] = found;
  }
  if (m.style == GlyphStyle::kXbox) return nullptr;
  auto it = m.entries.find(content_hash);
  if (it == m.entries.end()) return nullptr;
  Impl::Entry& e = it->second;
  bool keyboard = m.style == GlyphStyle::kKeyboard;
  // Only the sheet differs between PS4 and PS5 (SHARE / CREATE).
  int index = keyboard ? 2 : e.sheet && m.style == GlyphStyle::kPlayStation5 ? 1 : 0;
  auto& texture = e.versions[index];
  if (keyboard && texture && e.keyboard_generation != m.bindings_generation) texture.Release();
  if (!texture) {
    Image img;
    if (e.map != ButtonMap::kNone) {
      // The diffuse defines the letters; until it's loaded the maps stay.
      if (!m.trigger || m.trigger->w != e.xbox.w || m.trigger->h != e.xbox.h) return nullptr;
      const Engraving& engraving = *m.trigger;
      KeyLabel label = keyboard ? ParseBinding(BindingFor(engraving.label, m.bindings))
                                : KeyLabel{-1, engraving.label};
      Mask mask = EngravedLabel(engraving, label);
      img = e.xbox;
      if (e.map == ButtonMap::kDiffuse) {
        RelabelDiffuse(img, engraving, mask);
      } else if (e.map == ButtonMap::kNormal) {
        if (!e.normal_fitted) {
          e.normal = FitNormalEngraving(e.xbox, engraving);
          e.normal_fitted = true;
        }
        RelabelNormal(img, engraving, e.normal, mask);
      } else {
        CoverLetters(img, engraving);
      }
    } else {
      img = e.sheet ? (keyboard ? MakeKeyboardSheet(e.xbox, m.bindings)
                                : MakePlayStationSheet(e.xbox, m.style))
                    : ApplyEdits(e.xbox, e.edits, m.style, m.bindings);
    }
    if (e.red_blue_swapped) SwapRedBlue(img);
    texture = m.Upload(img, e.levels, e.map == ButtonMap::kNone);
    if (keyboard) e.keyboard_generation = m.bindings_generation;
    if (!texture) return nullptr;
  }
  return texture;
}

}  // namespace renderer
