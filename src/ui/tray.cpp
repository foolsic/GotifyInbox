#include "ui/tray.h"
#include "resource.h"

namespace
{
    NOTIFYICONDATAW g_nid = {};
    HICON g_currentIcon = nullptr;
}

bool Tray::Add(HWND hwnd, HICON icon, const wchar_t* tip)
{
    g_nid = {};
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = icon;
    wcscpy_s(g_nid.szTip, tip);
    g_currentIcon = icon;

    if (!Shell_NotifyIconW(NIM_ADD, &g_nid))
    {
        return false;
    }

    // 使用新版回调(版本4),支持更完整的鼠标事件
    g_nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &g_nid);
    return true;
}

void Tray::Remove()
{
    if (g_nid.hWnd)
    {
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        g_nid = {};
    }
    g_currentIcon = nullptr;
}

void Tray::SetIcon(HICON icon)
{
    g_currentIcon = icon;
    g_nid.hIcon = icon;
    g_nid.uFlags = NIF_ICON;
    if (!Shell_NotifyIconW(NIM_MODIFY, &g_nid))
    {
        // 修改失败(如资源管理器重启导致图标丢失):重新添加并恢复版本协议
        g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        if (Shell_NotifyIconW(NIM_ADD, &g_nid))
        {
            g_nid.uVersion = NOTIFYICON_VERSION_4;
            Shell_NotifyIconW(NIM_SETVERSION, &g_nid);
        }
    }
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
}

HICON Tray::CurrentIcon()
{
    return g_currentIcon;
}
