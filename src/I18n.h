#pragma once
// 界面国际化：中/英文切换。Tr(zh, en) 按当前语言返回对应文本。
#include <string>

namespace i18n {

enum class Lang { Zh, En };

Lang Current();              // 当前语言
void SetLang(Lang l);        // 设置语言
Lang Toggle();               // 切换语言，返回切换后的语言
std::wstring Tr(const wchar_t* zh, const wchar_t* en);  // 中文返回 zh，英文返回 en

} // namespace i18n
