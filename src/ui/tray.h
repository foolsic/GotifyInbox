#pragma once
#include <windows.h>

namespace Tray
{
    // 添加托盘图标(重复调用会更新)
    bool Add(HWND hwnd, HICON icon, const wchar_t* tip);

    // 删除托盘图标
    void Remove();

    // 切换图标(用于消息到达闪烁)
    void SetIcon(HICON icon);

    // 当前图标句柄(供闪烁交替使用)
    HICON CurrentIcon();
}
