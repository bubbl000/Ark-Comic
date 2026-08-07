#pragma once
// JSON 读写工具（UTF-8），基于 nlohmann/json
#include <json.hpp>
#include <string>

namespace ark {

using Json = nlohmann::json;

// 读取 UTF-8 JSON 文件。失败返回 false 并置 json 为空。
bool ReadJsonFile(const std::wstring& path, Json& out);

// 写入 JSON 文件（UTF-8，缩进 2 空格，与 C# System.Text.Json 风格接近）。
bool WriteJsonFile(const std::wstring& path, const Json& value);

} // namespace ark