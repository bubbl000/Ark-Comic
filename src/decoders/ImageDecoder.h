#pragma once
// 简化版解码器基类（从方舟图片浏览器移植，去掉瓦片/金字塔/工厂注册）
// 漫画阅读器只需：Open → DecodeFull / DecodeLevel
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <memory>

// 解码结果
struct DecodeResult {
    int width  = 0;
    int height = 0;
    int stride = 0;               // 每行字节数
    std::vector<uint8_t> pixels;  // BGRA8 像素数据

    // 最近邻降采样到目标尺寸（用于 NVJPEG/turbojpeg 1/8 后的二次缩放）
    DecodeResult ScaleDown(int targetW, int targetH) const {
        DecodeResult dst;
        dst.width = targetW; dst.height = targetH; dst.stride = targetW * 4;
        dst.pixels.resize((size_t)targetW * targetH * 4);
        for (int y = 0; y < targetH; y++) {
            int srcY = y * height / targetH;
            const uint8_t* srcRow = pixels.data() + (size_t)srcY * stride;
            uint8_t* dstRow = dst.pixels.data() + (size_t)y * dst.stride;
            for (int x = 0; x < targetW; x++) {
                memcpy(dstRow + x * 4, srcRow + (x * width / targetW) * 4, 4);
            }
        }
        return dst;
    }
};

// 图像格式元信息
struct ImageInfo {
    int width       = 0;
    int height      = 0;
    int bitDepth    = 8;
    bool hasAlpha   = false;
    std::string format;   // "JPEG", "PNG", "WebP" ...
    std::string decoderName;
};

// 解码器接口
class ImageDecoder {
public:
    virtual ~ImageDecoder() = default;

    struct OpenResult {
        ImageInfo info;
        std::shared_ptr<void> state;  // FileMapping 注入
    };
    virtual std::optional<OpenResult> Open(const uint8_t* data, size_t len) = 0;

    // 全图解码（输出原始尺寸 BGRA8）
    virtual std::optional<DecodeResult> DecodeFull(const OpenResult& open) = 0;

    // 层级解码：level n 对应 1/2^n 降采样（用于缩略图，避免解码完整大图）
    // level 0 = 原图；level 1/2/3 = 1/2/1/4/1/8
    virtual std::optional<DecodeResult> DecodeLevel(const OpenResult& open, int level) = 0;

    virtual const char* Name() const = 0;
};
