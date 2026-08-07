#include "ImageCodecs.h"
#include "JpegDecoder.h"
#include "WicDecoder.h"
#include "FileMapping.h"
#include "Logger.h"
#include <cstring>
#include <algorithm>
#include <memory>

namespace ark::decoders {

// JPEG 魔数：FF D8 FF
static bool IsJpeg(const uint8_t* data, size_t len) {
    return len >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF;
}

// 读文件头前 N 字节用于格式判断（不映射整个文件）
static bool ReadFileHead(const std::wstring& path, std::vector<uint8_t>& head, size_t n = 16) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    head.resize(n);
    DWORD read = 0;
    BOOL ok = ReadFile(h, head.data(), (DWORD)n, &read, nullptr);
    CloseHandle(h);
    head.resize(read);
    return ok && read > 0;
}

bool HwAccelAvailable() {
    return JpegDecoder::HwAccelAvailable();
}

// 共享 FileMapping（用 shared_ptr 持有，state 字段直接存它）
// 解码器通过 std::static_pointer_cast<FileMapping>(open.state) 取回
static std::shared_ptr<FileMapping> MakeFileMap(const std::wstring& path) {
    auto fm = std::make_shared<FileMapping>(path);
    return fm->Data() ? fm : nullptr;
}

// ─── 整页解码 ───
bool DecodeImageFull(const std::wstring& path,
                     std::vector<unsigned char>& px, int& w, int& h) {
    std::vector<uint8_t> head;
    if (!ReadFileHead(path, head)) return false;

    auto fm = MakeFileMap(path);
    if (!fm) return false;

    bool isJpeg = IsJpeg(head.data(), head.size());

    // 用裸指针调用，避免 unique_ptr 派生类到基类引用的转换问题
    auto tryDecode = [&](ImageDecoder* d) -> std::optional<DecodeResult> {
        auto open = d->Open(fm->Data(), fm->Size());
        if (!open) return std::nullopt;
        open->state = std::static_pointer_cast<void>(fm);
        return d->DecodeFull(*open);
    };

    if (isJpeg) {
        JpegDecoder jpeg;
        if (auto r = tryDecode(&jpeg)) {
            px = std::move(r->pixels); w = r->width; h = r->height;
            return true;
        }
        LOG_WARN("ImageCodecs", "JPEG 专属解码失败，回退 WIC");
    }

    // WIC 回退 / PNG / 其他格式
    WicDecoder wic;
    if (auto r = tryDecode(&wic)) {
        px = std::move(r->pixels); w = r->width; h = r->height;
        return true;
    }
    return false;
}

// 计算金字塔 level：让 1/2^level 后的尺寸 >= target（保证后续 ScaleDown 不上采样）
static int CalcLevel(int orig, int target) {
    if (target <= 0 || orig <= target) return 0;
    int level = 0;
    while ((orig >> (level + 1)) >= target) level++;
    return std::min(level, 8);  // 上限 1/256
}

// ─── 缩略图解码 ───
bool DecodeImageThumb(const std::wstring& path, int targetW, int targetH,
                      std::vector<unsigned char>& px, int& w, int& h) {
    std::vector<uint8_t> head;
    if (!ReadFileHead(path, head)) return false;

    auto fm = MakeFileMap(path);
    if (!fm) return false;

    bool isJpeg = IsJpeg(head.data(), head.size());

    // 先用专属解码器 Open 拿宽高
    std::unique_ptr<ImageDecoder> decoder;
    if (isJpeg) decoder = std::make_unique<JpegDecoder>();
    else        decoder = std::make_unique<WicDecoder>();

    auto open = decoder->Open(fm->Data(), fm->Size());
    if (!open) {
        // JPEG Open 失败回退 WIC
        if (isJpeg) {
            LOG_WARN("ImageCodecs", "JPEG Open 失败，缩略图走 WIC");
            decoder = std::make_unique<WicDecoder>();
            open = decoder->Open(fm->Data(), fm->Size());
        }
        if (!open) return false;
    }
    open->state = std::static_pointer_cast<void>(fm);

    int origW = open->info.width, origH = open->info.height;
    if (origW <= 0 || origH <= 0) return false;

    // 等比缩放到 targetW × targetH 框内
    float s = std::min((float)targetW / origW, (float)targetH / origH);
    int finalW = std::max(1, (int)lround(origW * s));
    int finalH = std::max(1, (int)lround(origH * s));

    // 选 level：让 1/2^level 后尺寸 >= finalW（避免上采样）
    int level = std::min(CalcLevel(origW, finalW), CalcLevel(origH, finalH));

    auto r = decoder->DecodeLevel(*open, level);
    if (!r) {
        r = decoder->DecodeLevel(*open, 0);  // level>0 失败时尝试原图
        if (!r) return false;
    }

    // 二次精确缩放到 finalW × finalH
    if (r->width != finalW || r->height != finalH) {
        *r = r->ScaleDown(finalW, finalH);
    }

    px = std::move(r->pixels);
    w = r->width; h = r->height;
    return true;
}

} // namespace ark::decoders
