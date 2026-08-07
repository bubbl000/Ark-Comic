#include "WicDecoder.h"
#include "Logger.h"
#include "FileMapping.h"
#include <wincodec.h>
#include <wrl/client.h>
#include <cstring>
#include <algorithm>

using Microsoft::WRL::ComPtr;

// WIC 工厂（全局单例延迟初始化）
// 注意：WIC 要求 STA 模式，但工作线程已 CoInitializeEx(MULTITHREADED)，
// WIC 内部会做 marshalling，实测可用
static IWICImagingFactory* GetWicFactory() {
    static ComPtr<IWICImagingFactory> factory;
    if (!factory) {
        CoCreateInstance(CLSID_WICImagingFactory, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    }
    return factory.Get();
}

// 魔数预筛：只让 WIC 可能支持的格式进入流解码
static bool IsWicFormat(const uint8_t* data, size_t len) {
    if (len >= 2 && data[0] == 'B' && data[1] == 'M') return true;             // BMP
    if (len >= 4 && data[0] == 0x89 && data[1] == 0x50 &&
        data[2] == 0x4E && data[3] == 0x47) return true;                       // PNG
    if (len >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF) return true;  // JPEG
    if (len >= 4) {  // TIFF
        if ((data[0] == 'I' && data[1] == 'I' && data[2] == 0x2A && data[3] == 0) ||
            (data[0] == 'M' && data[1] == 'M' && data[2] == 0 && data[3] == 0x2A))
            return true;
    }
    if (len >= 4 && data[0] == 'G' && data[1] == 'I' &&
        data[2] == 'F' && data[3] == '8') return true;                         // GIF
    if (len >= 12 && memcmp(data, "RIFF", 4) == 0 &&
        memcmp(data + 8, "WEBP", 4) == 0) return true;                         // WebP
    return false;
}

// 从内存数据创建 WIC 解码器
static ComPtr<IWICBitmapDecoder> CreateWicDecoder(IWICImagingFactory* factory,
    const uint8_t* data, size_t len) {
    ComPtr<IWICStream> stream;
    if (FAILED(factory->CreateStream(&stream))) return nullptr;
    if (FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(data), (DWORD)len)))
        return nullptr;
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr,
        WICDecodeMetadataCacheOnDemand, &decoder)))
        return nullptr;
    return decoder;
}

std::optional<ImageDecoder::OpenResult> WicDecoder::Open(const uint8_t* data, size_t len) {
    if (!IsWicFormat(data, len)) {
        auto* factory = GetWicFactory();
        if (!factory || !CreateWicDecoder(factory, data, len)) return std::nullopt;
    }

    OpenResult result;
    result.info.format = "WIC";
    result.info.decoderName = "WIC";

    auto* factory = GetWicFactory();
    if (factory) {
        auto decoder = CreateWicDecoder(factory, data, len);
        if (decoder) {
            ComPtr<IWICBitmapFrameDecode> frame;
            if (SUCCEEDED(decoder->GetFrame(0, &frame))) {
                UINT w = 0, h = 0;
                frame->GetSize(&w, &h);
                result.info.width = (int)w;
                result.info.height = (int)h;
            }
        }
    }
    return result;
}

std::optional<DecodeResult> WicDecoder::DecodeFull(const OpenResult& open) {
    auto* factory = GetWicFactory();
    if (!factory) return std::nullopt;

    auto fileMap = std::static_pointer_cast<FileMapping>(open.state);
    if (!fileMap || !fileMap->Data()) return std::nullopt;

    auto decoder = CreateWicDecoder(factory, fileMap->Data(), fileMap->Size());
    if (!decoder) return std::nullopt;

    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) return std::nullopt;

    UINT w = 0, h = 0;
    frame->GetSize(&w, &h);
    if (w == 0 || h == 0) return std::nullopt;

    // 统一转 32bpp PBGRA（预乘 alpha，匹配 D2D ALPHA_MODE_PREMULTIPLIED）
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter))) return std::nullopt;
    if (FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
        WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)))
        return std::nullopt;

    DecodeResult result;
    result.width = (int)w;
    result.height = (int)h;
    result.stride = (int)w * 4;
    result.pixels.resize((size_t)result.stride * result.height);

    if (FAILED(converter->CopyPixels(nullptr, result.stride,
        (UINT)result.pixels.size(), result.pixels.data())))
        return std::nullopt;

    return result;
}

std::optional<DecodeResult> WicDecoder::DecodeLevel(const OpenResult& open, int level) {
    auto* factory = GetWicFactory();
    if (!factory) return std::nullopt;
    auto fileMap = std::static_pointer_cast<FileMapping>(open.state);
    if (!fileMap || !fileMap->Data()) return std::nullopt;

    auto decoder = CreateWicDecoder(factory, fileMap->Data(), fileMap->Size());
    if (!decoder) return std::nullopt;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) return std::nullopt;

    UINT origW = 0, origH = 0;
    frame->GetSize(&origW, &origH);
    if (origW == 0 || origH == 0) return std::nullopt;

    int targetW = (std::max)(1, (int)origW >> level);
    int targetH = (std::max)(1, (int)origH >> level);

    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter))) return std::nullopt;
    if (FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
        WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)))
        return std::nullopt;

    DecodeResult result;
    result.width = targetW;
    result.height = targetH;
    result.stride = targetW * 4;
    result.pixels.resize((size_t)result.stride * targetH);

    // level 0 直接拷贝原始尺寸；level > 0 用 BitmapScaler Fant 降采样
    if (level == 0) {
        if (FAILED(converter->CopyPixels(nullptr, result.stride,
            (UINT)result.pixels.size(), result.pixels.data())))
            return std::nullopt;
    } else {
        ComPtr<IWICBitmapScaler> scaler;
        if (FAILED(factory->CreateBitmapScaler(&scaler))) return std::nullopt;
        if (FAILED(scaler->Initialize(converter.Get(), (UINT)targetW, (UINT)targetH,
            WICBitmapInterpolationModeFant)))
            return std::nullopt;
        if (FAILED(scaler->CopyPixels(nullptr, result.stride,
            (UINT)result.pixels.size(), result.pixels.data())))
            return std::nullopt;
    }
    return result;
}
