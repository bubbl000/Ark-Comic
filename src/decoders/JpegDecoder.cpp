#include "JpegDecoder.h"
#include "Logger.h"
#include "FileMapping.h"
#include <cstring>
#include <algorithm>
#include <cstdlib>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

// turbojpeg C API（动态加载 turbojpeg.dll）
typedef void* (__stdcall *fn_tjInit)();
typedef int   (__stdcall *fn_tjHeader)(void*, unsigned char*, unsigned long, int*, int*, int*);
typedef int   (__stdcall *fn_tjDecode)(void*, unsigned char*, unsigned long, unsigned char*, int, int, int, int, int);
typedef int   (__stdcall *fn_tjKill)(void*);
typedef void* (__stdcall *fn_tjInitC)();
typedef int   (__stdcall *fn_tjCompress)(void*, const unsigned char*, int, int, int, int, unsigned char**, unsigned long*, int, int, int);
typedef void  (__stdcall *fn_tjFree)(unsigned char*);

// turbojpeg 官方枚举：TJPF_BGRA = 8（直接输出 BGRA 匹配 D2D）
static const int TJPF_BGRA = 8;
static const int TJFLAG_FASTUPSAMPLE = 256;
static const int TJSAMP_420 = 2;

static HMODULE g_tj = nullptr;
static fn_tjInit    tjI = nullptr;
static fn_tjHeader  tjH = nullptr;
static fn_tjDecode  tjD = nullptr;
static fn_tjKill    tjK = nullptr;
static fn_tjInitC    tjIC = nullptr;
static fn_tjCompress tjC  = nullptr;
static fn_tjFree     tjF  = nullptr;

static bool LoadTJ() {
    if (g_tj) return true;
    g_tj = LoadLibraryW(L"turbojpeg.dll");
    if (!g_tj) { LOG_INFO("TurboJPEG", "turbojpeg.dll 加载失败，JPEG 走 WIC 回退"); return false; }
    tjI = (fn_tjInit)GetProcAddress(g_tj, "tjInitDecompress");
    tjH = (fn_tjHeader)GetProcAddress(g_tj, "tjDecompressHeader2");
    tjD = (fn_tjDecode)GetProcAddress(g_tj, "tjDecompress2");
    tjK = (fn_tjKill)GetProcAddress(g_tj, "tjDestroy");
    tjIC = (fn_tjInitC)GetProcAddress(g_tj, "tjInitCompress");
    tjC  = (fn_tjCompress)GetProcAddress(g_tj, "tjCompress2");
    tjF  = (fn_tjFree)GetProcAddress(g_tj, "tjFree");
    bool ok = tjI && tjH && tjD && tjK;
    if (ok) LOG_INFO("TurboJPEG", "libjpeg-turbo 加载成功，JPEG CPU 解码就绪");
    return ok;
}

// BGRA 像素 → JPEG 编码（另存为用）
bool JpegDecoder::EncodeJpeg(const uint8_t* bgra, int w, int h, int stride,
                             int quality, std::vector<uint8_t>& out) {
    if (!LoadTJ() || !tjIC || !tjC || !tjF) return false;
    if (!bgra || w <= 0 || h <= 0 || stride < w * 4) return false;
    if (quality < 1) quality = 1;
    if (quality > 100) quality = 100;

    void* hc = tjIC();
    if (!hc) return false;
    unsigned char* jpegBuf = nullptr;
    unsigned long  jpegSize = 0;
    int r = tjC(hc, bgra, w, stride, h, TJPF_BGRA, &jpegBuf, &jpegSize, TJSAMP_420, quality, 0);
    tjK(hc);
    if (r != 0 || !jpegBuf || jpegSize == 0) {
        if (jpegBuf) tjF(jpegBuf);
        return false;
    }
    out.assign(jpegBuf, jpegBuf + jpegSize);
    tjF(jpegBuf);
    return true;
}

std::optional<ImageDecoder::OpenResult> JpegDecoder::Open(const uint8_t* data, size_t len) {
    // JPEG 魔数检测（FF D8 FF）
    if (len < 3 || data[0] != 0xFF || data[1] != 0xD8 || data[2] != 0xFF) return {};
    // 至少有一种解码方式可用（NVJPEG 或 turbojpeg）
    bool hasHw = NvjpegHardDecoder::Available();
    bool hasTj = LoadTJ();
    if (!hasHw && !hasTj) return {};

    OpenResult res;
    res.info.format = "JPEG";

    // 优先用 turbojpeg 读头（快），失败时尝试 NVJPEG 读头
    if (hasTj) {
        void* h = tjI(); if (!h) return {};
        int w = 0, hh = 0, s = 0;
        int r = tjH(h, (unsigned char*)data, (unsigned long)len, &w, &hh, &s);
        tjK(h);
        if (r == 0 && w > 0 && hh > 0) {
            res.info.width = w; res.info.height = hh;
            res.info.decoderName = hasHw ? "nvJPEG" : "libjpeg-turbo";
            return res;
        }
    }
    return {};
}

std::optional<DecodeResult> JpegDecoder::DecodeFull(const OpenResult& open) {
    return DecodeLevel(open, 0);
}

// 层级解码：硬解优先 → turbojpeg 1/2^n 降采样 → 失败返回 nullopt 由调用方回退 WIC
std::optional<DecodeResult> JpegDecoder::DecodeLevel(const OpenResult& open, int level) {
    auto fileMap = std::static_pointer_cast<FileMapping>(open.state);
    if (!fileMap || !fileMap->Data()) return {};

    // 1. NVJPEG 硬解优先
    if (NvjpegHardDecoder::Available()) {
        if (auto r = _hw.DecodeLevel(fileMap->Data(), fileMap->Size(), level)) {
            return r;
        }
        LOG_WARN("NVJPEG", "硬解失败，回退 libjpeg-turbo");
    }

    // 2. libjpeg-turbo CPU 解码
    if (!LoadTJ()) return {};
    void* h = tjI(); if (!h) return {};
    auto* data = const_cast<uint8_t*>(fileMap->Data());
    int origW = 0, origH = 0, subsamp = 0;
    if (tjH(h, data, (unsigned long)fileMap->Size(), &origW, &origH, &subsamp) != 0) {
        tjK(h); return {};
    }
    if (origW <= 0 || origH <= 0) { tjK(h); return {}; }

    // turbojpeg 原生支持 1/2^n 降采样（n ≤ 3，即 1/2、1/4、1/8）
    // 通过 DCT 系数丢弃实现，不解码全量像素，极快
    int tjLevel = (std::min)(level, 3);
    int scaledW = (origW + (1 << tjLevel) - 1) >> tjLevel;
    int scaledH = (origH + (1 << tjLevel) - 1) >> tjLevel;

    DecodeResult result;
    result.width = scaledW; result.height = scaledH;
    result.stride = scaledW * 4;
    result.pixels.resize((size_t)result.stride * scaledH);

    if (tjD(h, data, (unsigned long)fileMap->Size(),
            result.pixels.data(), scaledW, scaledW * 4, scaledH,
            TJPF_BGRA, TJFLAG_FASTUPSAMPLE) != 0) {
        tjK(h); return {};
    }
    tjK(h);

    // JPEG 无 alpha，全部设为 255
    for (size_t i = 3; i < result.pixels.size(); i += 4) result.pixels[i] = 255;

    // level > 3 时 turbojpeg 降采样不够，CPU 端再缩放
    if (tjLevel < level) {
        int targetW = (std::max)(1, origW >> level);
        int targetH = (std::max)(1, origH >> level);
        result = result.ScaleDown(targetW, targetH);
    }
    return result;
}
