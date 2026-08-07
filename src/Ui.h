#pragma once
// D2D/DWrite 全局资源与绘制辅助
#include <d2d1.h>
#include <dwrite.h>
#include <string>

namespace ark::ui {

// 全局 D2D/DWrite 单例
class D2D {
public:
    static ID2D1Factory* Factory();
    static IDWriteFactory* Write();
    // 获取（缓存）文本格式
    static IDWriteTextFormat* Format(const wchar_t* family, float size, DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL);

    // 绘制圆角矩形（fill 与 border 由调用方决定，borderW<=0 不描边）
    static void RoundedRect(ID2D1RenderTarget* rt, D2D1_RECT_F r, float radius,
                            const D2D1_COLOR_F& fill,
                            const D2D1_COLOR_F& border = D2D1::ColorF(0, 0, 0, 0),
                            float borderW = 0);
    // 绘制文本（自动按 rect 对齐，使用 UI 字体）
    static void Text(ID2D1RenderTarget* rt, const std::wstring& text, const D2D1_RECT_F& rect,
                     const D2D1_COLOR_F& color, float size,
                     DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL,
                     DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING,
                     DWRITE_PARAGRAPH_ALIGNMENT palign = DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    // 绘制 Segoe MDL2 图标
    static void Icon(ID2D1RenderTarget* rt, const std::wstring& glyph, const D2D1_RECT_F& rect,
                     const D2D1_COLOR_F& color, float size);
    // 测量文本宽度（UI 字体）
    static float TextWidth(const std::wstring& text, float size, DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL);
    // 从图片文件加载封面位图（WIC 解码，缓存；失败返回 nullptr）
    static ID2D1Bitmap* Bitmap(ID2D1RenderTarget* rt, const std::wstring& path);
    // 清空位图缓存（渲染目标释放/重建时调用）
    static void ClearBitmapCache();
};

} // namespace ark::ui