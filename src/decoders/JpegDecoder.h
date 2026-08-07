#pragma once
#include "ImageDecoder.h"
#include "NvjpegHardDecoder.h"
#include <mutex>

// libjpeg-turbo + NVJPEG 硬解 JPEG 解码器
// 优先硬解（N 卡可用时），失败回退 libjpeg-turbo，再失败回退 WIC（由调用方处理）
class JpegDecoder : public ImageDecoder {
public:
    const char* Name() const override { return "JPEG"; }

    std::optional<OpenResult> Open(const uint8_t* data, size_t len) override;
    std::optional<DecodeResult> DecodeFull(const OpenResult& open) override;
    std::optional<DecodeResult> DecodeLevel(const OpenResult& open, int level) override;

    // BGRA 像素 → JPEG 编码（另存为用）
    static bool EncodeJpeg(const uint8_t* bgra, int w, int h, int stride,
                           int quality, std::vector<uint8_t>& out);

    // 进程级 NVJPEG 可用性（封装 NvjpegHardDecoder::Available）
    static bool HwAccelAvailable() { return NvjpegHardDecoder::Available(); }

private:
    NvjpegHardDecoder _hw;   // NVJPEG 硬解工具
};
