#pragma once
#include "ImageDecoder.h"

// WIC 解码器：处理 PNG / WebP / BMP / GIF / TIFF 等 WIC 支持的所有格式
// 也作为 JPEG 的最终回退（NVJPEG + libjpeg-turbo 都失败时）
class WicDecoder : public ImageDecoder {
public:
    const char* Name() const override { return "WIC"; }
    std::optional<OpenResult> Open(const uint8_t* data, size_t len) override;
    std::optional<DecodeResult> DecodeFull(const OpenResult& open) override;
    std::optional<DecodeResult> DecodeLevel(const OpenResult& open, int level) override;
};
