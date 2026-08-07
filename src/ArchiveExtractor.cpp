#include "ArchiveExtractor.h"
#include "FileUtil.h"
#include "Utf.h"
#include "NaturalSort.h"
#include <miniz.h>
#include <algorithm>
#include <cstdio>
#include <windows.h>

namespace ark {

// 缓存目录：%LOCALAPPDATA%\ArkComic\cbz_cache\<文件名>_<大小>_<修改时间>
std::wstring ArchiveExtractor::CacheDirFor(const std::string& archive) {
    wchar_t base[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH)) {
        GetTempPathW(MAX_PATH, base);
    }
    std::wstring dir = std::wstring(base) + L"\\ArkComic\\cbz_cache\\";
    std::wstring wfile = W(archive);
    // 文件名（去扩展名）
    std::wstring name = GetFileNameNoExtW(wfile);
    // 大小 + 修改时间（源文件变化自动失效）
    HANDLE h = CreateFileW(wfile.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER size{}, mtime{};
        GetFileSizeEx(h, &size);
        FILETIME ft{};
        GetFileTime(h, nullptr, nullptr, &ft);
        mtime.QuadPart = ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
        CloseHandle(h);
        wchar_t buf[64];
        swprintf_s(buf, L"_%lld_%lld", size.QuadPart, mtime.QuadPart);
        name += buf;
    }
    return dir + name;
}

// 列出目录下递归所有图片（自然排序）
std::vector<std::string> ArchiveExtractor::ListDirImages(const std::string& dir) {
    std::vector<std::string> out;
    std::vector<std::wstring> stack = { W(dir) };
    while (!stack.empty()) {
        std::wstring cur = stack.back(); stack.pop_back();
        std::wstring pattern = cur + L"\\*";
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            std::wstring full = cur + L"\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (wcscmp(fd.cFileName, L".") != 0 && wcscmp(fd.cFileName, L"..") != 0)
                    stack.push_back(full);
            } else if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) && IsImageFile(U8(fd.cFileName))) {
                out.push_back(U8(full));
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) {
        return NaturalCompare(a, b) < 0;
    });
    return out;
}

// CBZ/ZIP：解压图片到缓存后列图（按压缩包内条目名自然排序后编号落盘，规避中文名问题）
std::vector<std::string> ArchiveExtractor::ExtractArchive(const std::string& archive) {
    std::wstring cacheDir = CacheDirFor(archive);
    std::wstring done = cacheDir + L"\\.done";
    // 命中缓存：直接列已解压的编号图片
    if (DirExistsW(cacheDir) && FileExistsW(done)) {
        std::vector<std::string> cached;
        for (auto& f : ListFiles(cacheDir)) {
            if (IsImageFile(U8(GetFileNameW(f)))) cached.push_back(U8(f));
        }
        std::sort(cached.begin(), cached.end(), [](const std::string& a, const std::string& b) {
            return NaturalCompare(a, b) < 0;
        });
        return cached;
    }
    // 未命中：清空旧缓存后重新解压
    if (DirExistsW(cacheDir)) DeleteDirRecursiveW(cacheDir);
    CreateDirRecursive(cacheDir);

    // 用 _wfopen（宽字符路径，支持中文）+ init_cfile 打开压缩包
    FILE* fp = _wfopen(W(archive).c_str(), L"rb");
    if (!fp) return {};
    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    if (fsize <= 0) { fclose(fp); return {}; }
    fseek(fp, 0, SEEK_SET);  // init_cfile 以当前文件位置为读取起点，须回卷到 0

    mz_zip_archive zip{};
    if (!mz_zip_reader_init_cfile(&zip, fp, (mz_uint64)fsize, 0)) { fclose(fp); return {}; }
    mz_uint n = mz_zip_reader_get_num_files(&zip);
    // 收集图片条目（原始索引 + 压缩包内名），按名自然排序
    std::vector<std::pair<mz_uint, std::string>> entries;
    for (mz_uint i = 0; i < n; i++) {
        mz_zip_archive_file_stat st{};
        if (!mz_zip_reader_file_stat(&zip, i, &st)) continue;
        std::string name = st.m_filename;
        size_t slash = name.find_last_of('/');
        std::string base = (slash == std::string::npos) ? name : name.substr(slash + 1);
        if (!st.m_is_directory && IsImageFile(base)) entries.push_back({ i, name });
    }
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
        return NaturalCompare(a.second, b.second) < 0;
    });
    // 按顺序编号落盘（ASCII 文件名，规避中文路径）
    std::vector<std::string> out;
    for (size_t i = 0; i < entries.size(); i++) {
        size_t dot = entries[i].second.find_last_of('.');
        std::string ext = (dot == std::string::npos) ? ".bin" : entries[i].second.substr(dot);
        for (size_t k = 0; k < ext.size(); k++) ext[k] = (char)tolower((unsigned char)ext[k]);
        size_t size = 0;
        void* buf = mz_zip_reader_extract_to_heap(&zip, entries[i].first, &size, 0);
        if (!buf) continue;
        char num[16];
        sprintf_s(num, "%04zu", i + 1);
        std::wstring dst = cacheDir + L"\\" + W(std::string(num) + ext);
        HANDLE f = CreateFileW(dst.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(f, buf, (DWORD)size, &written, nullptr);
            CloseHandle(f);
            out.push_back(U8(dst));
        }
        mz_free(buf);
    }
    mz_zip_reader_end(&zip);
    fclose(fp);  // init_cfile 不负责关闭文件
    // 写完成标记
    if (!out.empty()) {
        HANDLE f = CreateFileW(done.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) { WriteFile(f, "1", 1, nullptr, nullptr); CloseHandle(f); }
    }
    return out;
}

// 返回漫画所有页面图片路径（已自然排序）。失败/无图返回空。
std::vector<std::string> ArchiveExtractor::GetPages(const std::string& path) {
    if (path.empty()) return {};
    if (IsArchivePath(path)) return ExtractArchive(path);
    return ListDirImages(path);
}

// 页数（调 GetPages）
int ArchiveExtractor::GetPageCount(const std::string& path) {
    return (int)GetPages(path).size();
}

} // namespace ark