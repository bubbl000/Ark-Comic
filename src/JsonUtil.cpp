#include "JsonUtil.h"
#include "Utf.h"
#include <fstream>
#include <windows.h>

namespace ark {

static bool ReadFileBinary(const std::wstring& path, std::string& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    out.resize((size_t)sz.QuadPart);
    DWORD read = 0;
    BOOL ok = TRUE;
    if (!out.empty())
        ok = ReadFile(h, &out[0], (DWORD)out.size(), &read, nullptr);
    CloseHandle(h);
    return ok == TRUE;
}

bool ReadJsonFile(const std::wstring& path, Json& out) {
    std::string data;
    if (!ReadFileBinary(path, data)) return false;
    try {
        out = Json::parse(data, nullptr, false); // 不抛异常，失败返回 null
        return !out.is_null();
    } catch (...) {
        return false;
    }
}

bool WriteJsonFile(const std::wstring& path, const Json& value) {
    std::string data = value.dump(2, ' '); // 缩进 2 空格
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(h, data.data(), (DWORD)data.size(), &written, nullptr);
    CloseHandle(h);
    return ok == TRUE;
}

} // namespace ark