#pragma once
// 图片解码统一入口（整合 JpegDecoder + WicDecoder）
// 漫画阅读器只需这两个 API：
//   DecodeImageFull    - 整页解码（JPEG 走 NVJPEG→turbojpeg→WIC，其他走 WIC）
//   DecodeImageThumb   - 缩略图解码（JPEG 走 turbojpeg 1/2^n 降采样，其他走 WIC BitmapScaler）
#include <cstdint>
#include <vector>
#include <string>

namespace ark::decoders {

// 整页解码：给定文件路径，输出 BGRA8 像素 + 宽高
// JPEG 优先 NVJPEG 硬解 → libjpeg-turbo → WIC 回退；PNG/其他走 WIC
bool DecodeImageFull(const std::wstring& path,
                     std::vector<unsigned char>& px, int& w, int& h);

// 缩略图解码：给定文件路径 + 目标尺寸，输出 BGRA8 像素 + 实际尺寸（等比 ≤ target）
// JPEG 用 turbojpeg 1/2^n DCT 降采样（极快，不解码全图）；PNG/其他用 WIC BitmapScaler
bool DecodeImageThumb(const std::wstring& path, int targetW, int targetH,
                      std::vector<unsigned char>& px, int& w, int& h);

// NVJPEG 硬解是否可用（用于状态展示）
bool HwAccelAvailable();

} // namespace ark::decoders
