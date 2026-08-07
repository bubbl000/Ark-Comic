#pragma once
// 深色主题消息框（确认/警告）
#include <windows.h>
#include <string>

namespace ark::ui {

// buttons: 0 = 仅确定(OK)；1 = 是/否(YesNo)。返回 true 表示"确定/是"。
bool ShowMsgBox(HWND owner, const std::wstring& text, const std::wstring& title,
                bool yesNo = false);

} // namespace ark::ui