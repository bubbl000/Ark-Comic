#include "Ui.h"
#include "Theme.h"
#include "ActivityLog.h"
#include <map>
#include <tuple>
#include <wincodec.h>

namespace ark::ui {

static ID2D1Factory* s_factory = nullptr;
static IDWriteFactory* s_write = nullptr;
static IWICImagingFactory* s_wic = nullptr;

static void EnsureFactories() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (!s_factory) D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &s_factory);
    if (!s_write) DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&s_write);
    if (!s_wic) CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                 IID_IWICImagingFactory, (void**)&s_wic);
}

ID2D1Factory* D2D::Factory() { EnsureFactories(); return s_factory; }
IDWriteFactory* D2D::Write() { EnsureFactories(); return s_write; }

static std::map<std::tuple<std::wstring, int, int>, IDWriteTextFormat*> s_formats;

IDWriteTextFormat* D2D::Format(const wchar_t* family, float size, DWRITE_FONT_WEIGHT weight) {
    auto key = std::make_tuple(std::wstring(family), (int)(size * 10), (int)weight);
    auto it = s_formats.find(key);
    if (it != s_formats.end()) return it->second;
    IDWriteTextFormat* fmt = nullptr;
    if (SUCCEEDED(Write()->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                            DWRITE_FONT_STRETCH_NORMAL, size, L"zh-CN", &fmt))) {
        s_formats[key] = fmt;
    }
    return fmt;
}

void D2D::RoundedRect(ID2D1RenderTarget* rt, D2D1_RECT_F r, float radius,
                      const D2D1_COLOR_F& fill, const D2D1_COLOR_F& border, float borderW) {
    ID2D1SolidColorBrush* brush = nullptr;
    rt->CreateSolidColorBrush(fill, &brush);
    if (brush) {
        if (radius > 0) rt->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush);
        else rt->FillRectangle(r, brush);
        brush->Release();
    }
    if (borderW > 0) {
        rt->CreateSolidColorBrush(border, &brush);
        if (brush) {
            if (radius > 0) rt->DrawRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush, borderW);
            else rt->DrawRectangle(r, brush, borderW);
            brush->Release();
        }
    }
}

void D2D::Text(ID2D1RenderTarget* rt, const std::wstring& text, const D2D1_RECT_F& rect,
               const D2D1_COLOR_F& color, float size, DWRITE_FONT_WEIGHT weight,
               DWRITE_TEXT_ALIGNMENT align, DWRITE_PARAGRAPH_ALIGNMENT palign) {
    if (text.empty()) return;
    IDWriteTextFormat* fmt = Format(theme::FontUI(), size, weight);
    if (!fmt) return;
    fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    fmt->SetTextAlignment(align);
    fmt->SetParagraphAlignment(palign);
    ID2D1SolidColorBrush* brush = nullptr;
    rt->CreateSolidColorBrush(color, &brush);
    if (brush) {
        rt->DrawText(text.c_str(), (UINT32)text.size(), fmt, rect, brush);
        brush->Release();
    }
}

void D2D::Icon(ID2D1RenderTarget* rt, const std::wstring& glyph, const D2D1_RECT_F& rect,
               const D2D1_COLOR_F& color, float size) {
    if (glyph.empty()) return;
    IDWriteTextFormat* fmt = Format(theme::FontIcon(), size);
    if (!fmt) return;
    fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    ID2D1SolidColorBrush* brush = nullptr;
    rt->CreateSolidColorBrush(color, &brush);
    if (brush) {
        rt->DrawText(glyph.c_str(), (UINT32)glyph.size(), fmt, rect, brush);
        brush->Release();
    }
}

float D2D::TextWidth(const std::wstring& text, float size, DWRITE_FONT_WEIGHT weight) {
    IDWriteTextFormat* fmt = Format(theme::FontUI(), size, weight);
    if (!fmt) return 0;
    IDWriteTextLayout* layout = nullptr;
    if (FAILED(Write()->CreateTextLayout(text.c_str(), (UINT32)text.size(), fmt, 10000, 100, &layout)))
        return 0;
    DWRITE_TEXT_METRICS tm{};
    layout->GetMetrics(&tm);
    layout->Release();
    return tm.width;
}

// ==================== 封面位图（WIC 解码 + 缓存） ====================
static std::map<std::wstring, ID2D1Bitmap*> s_bitmaps;
static ID2D1RenderTarget* s_bitmapRt = nullptr;

// 解码图片并缩放到 maxDim 内，避免大图占满内存
static ID2D1Bitmap* LoadBitmapFromFile(ID2D1RenderTarget* rt, const std::wstring& path, UINT maxDim) {
    if (!s_wic) return nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    if (FAILED(s_wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                WICDecodeMetadataCacheOnDemand, &decoder)))
        return nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    if (FAILED(decoder->GetFrame(0, &frame))) { decoder->Release(); return nullptr; }
    IWICBitmapScaler* scaler = nullptr;
    if (FAILED(s_wic->CreateBitmapScaler(&scaler))) { frame->Release(); decoder->Release(); return nullptr; }
    UINT w = 0, h = 0;
    frame->GetSize(&w, &h);
    if (w == 0 || h == 0) { scaler->Release(); frame->Release(); decoder->Release(); return nullptr; }
    // 等比缩放，最长边不超过 maxDim
    UINT nw = w, nh = h;
    if (w > maxDim || h > maxDim) {
        double scale = (double)maxDim / (w > h ? w : h);
        nw = (UINT)(w * scale); nh = (UINT)(h * scale);
    }
    if (FAILED(scaler->Initialize(frame, nw, nh, WICBitmapInterpolationModeHighQualityCubic))) {
        scaler->Release(); frame->Release(); decoder->Release(); return nullptr;
    }
    IWICFormatConverter* conv = nullptr;
    if (FAILED(s_wic->CreateFormatConverter(&conv))) { scaler->Release(); frame->Release(); decoder->Release(); return nullptr; }
    if (FAILED(conv->Initialize(scaler, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                                nullptr, 0.0, WICBitmapPaletteTypeMedianCut))) {
        conv->Release(); scaler->Release(); frame->Release(); decoder->Release(); return nullptr;
    }
    ID2D1Bitmap* bmp = nullptr;
    rt->CreateBitmapFromWicBitmap(conv, nullptr, &bmp);
    conv->Release(); scaler->Release(); frame->Release(); decoder->Release();
    return bmp;
}

void D2D::ClearBitmapCache() {
    for (auto& kv : s_bitmaps) if (kv.second) kv.second->Release();
    s_bitmaps.clear();
    s_bitmapRt = nullptr;
}

ID2D1Bitmap* D2D::Bitmap(ID2D1RenderTarget* rt, const std::wstring& path) {
    if (!rt || path.empty()) return nullptr;
    // 渲染目标变化（重建）时整体失效
    if (s_bitmapRt && s_bitmapRt != rt) ClearBitmapCache();
    auto it = s_bitmaps.find(path);
    if (it != s_bitmaps.end()) return it->second;
    EnsureFactories();
    // 性能遥测：封面解码耗时（仅缓存未命中时，避免热路径开销）
    PerfScope ps(L"解码", "DecodeCover");
    ps.SetExtra({Perf::S("file", ActivityFmt::NarrowUtf8(path))});
    ID2D1Bitmap* bmp = LoadBitmapFromFile(rt, path, 512);
    if (bmp) { s_bitmaps[path] = bmp; s_bitmapRt = rt; }
    return bmp;
}

} // namespace ark::ui