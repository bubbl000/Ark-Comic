#include "I18n.h"

namespace i18n {

namespace {
// 全局语言状态，默认中文
Lang g_lang = Lang::Zh;
}

Lang Current() { return g_lang; }

void SetLang(Lang l) { g_lang = l; }

Lang Toggle() {
    g_lang = (g_lang == Lang::Zh) ? Lang::En : Lang::Zh;
    return g_lang;
}

std::wstring Tr(const wchar_t* zh, const wchar_t* en) {
    return g_lang == Lang::En ? en : zh;
}

} // namespace i18n
