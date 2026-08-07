#pragma once
// 主题色板：与 C# 旧版 App.xaml 颜色系统完全一致。
// 强调色可在运行时切换（AppConfig.ThemeColor 驱动），其余背景/文字色固定。
#include <d2d1.h>
#include <string>
#include <cstdio>

namespace ark::theme {

inline D2D1_COLOR_F rgb(DWORD hex, float a = 1.0f) {
    float r = (float)((hex >> 16) & 0xFF) / 255.0f;
    float g = (float)((hex >> 8) & 0xFF) / 255.0f;
    float b = (float)(hex & 0xFF) / 255.0f;
    return D2D1::ColorF(r, g, b, a);
}

// 运行时主题状态（强调色 + 其派生的 muted 背景）
namespace detail {
inline D2D1_COLOR_F g_accent = rgb(0xCBE93A);          // 当前强调色
inline D2D1_COLOR_F g_muted = rgb(0x2A6B70);           // 标签 chip 背景（强调色 20% 亮度）
}

// 背景
inline D2D1_COLOR_F BgMain()      { return rgb(0x1A1D24); } // #1A1D24
inline D2D1_COLOR_F BgSidebar()   { return rgb(0x18191C); } // #18191c
inline D2D1_COLOR_F BgTitlebar()  { return rgb(0x1F2023); } // #1f2023
inline D2D1_COLOR_F BgCard()      { return rgb(0x242932); } // #242932
inline D2D1_COLOR_F BgCardHover() { return rgb(0x2D333F); } // #2D333F
// 强调（随主题切换）
inline D2D1_COLOR_F AccentCyan()  { return detail::g_accent; }
inline D2D1_COLOR_F MutedCyan()   { return detail::g_muted; }
// 文字
inline D2D1_COLOR_F TextPrimary()   { return rgb(0xE2E8F0); } // #E2E8F0
inline D2D1_COLOR_F TextSecondary() { return rgb(0x94A3B8); } // #94A3B8
inline D2D1_COLOR_F BorderColor()   { return rgb(0x2D333F); } // #2D333F
// 窗口外边框
inline D2D1_COLOR_F WindowBorder()  { return rgb(0x2A3038); } // #2A3038
// 关闭按钮 hover
inline D2D1_COLOR_F CloseHover()  { return rgb(0xC42B1C); }  // #C42B1C
// 强调按钮文字（深色）
inline D2D1_COLOR_F AccentText()  { return rgb(0x14171C); }  // #14171C

inline const wchar_t* FontUI()      { return L"Microsoft YaHei UI"; }
inline const wchar_t* FontIcon()    { return L"Segoe MDL2 Assets"; }

// 设置强调色（0xRRGGBB），同步派生 muted 背景
inline void SetAccentColor(DWORD hex) {
    detail::g_accent = rgb(hex);
    const float k = 0.20f; // muted 背景 = 强调色 20% 亮度
    detail::g_muted = D2D1::ColorF(detail::g_accent.r * k, detail::g_accent.g * k,
                                   detail::g_accent.b * k, 1.0f);
}

// 从 "#RRGGBB" 字符串设置强调色；非法格式返回 false 不生效
inline bool SetAccentHex(const std::string& h) {
    if (h.size() != 7 || h[0] != '#') return false;
    auto hexv = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    DWORD v = 0;
    for (int i = 1; i < 7; i++) {
        int n = hexv(h[i]);
        if (n < 0) return false;
        v = (v << 4) | (DWORD)n;
    }
    SetAccentColor(v);
    return true;
}

// 当前强调色十六进制（#RRGGBB），用于设置窗口显示
inline std::string CurrentAccentHex() {
    auto toHex = [](float f) { int v = (int)(f * 255 + 0.5f); if (v < 0) v = 0; if (v > 255) v = 255; return v; };
    char buf[8];
    sprintf_s(buf, "#%02X%02X%02X", toHex(detail::g_accent.r), toHex(detail::g_accent.g), toHex(detail::g_accent.b));
    return buf;
}

} // namespace ark::theme