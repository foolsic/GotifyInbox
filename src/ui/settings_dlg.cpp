#include "ui/settings_dlg.h"
#include "core/config.h"
#include "resource.h"
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>

namespace
{
    // 控件句柄(对话框生命周期内有效)
    HWND g_urlEdit = nullptr;
    HWND g_tokenEdit = nullptr;

    // 模态循环退出标志(WM_DESTROY 时置位)
    bool s_dialogClosed = false;

    // 对话框统一 UI 字体(10px)
    HFONT s_dlgFont = nullptr;

    void ApplyDlgFont(HWND ctrl)
    {
        SendMessageW(ctrl, WM_SETFONT, (WPARAM)s_dlgFont, TRUE);
    }

    void SetEditText(HWND edit, const std::wstring& text)
    {
        SetWindowTextW(edit, text.c_str());
    }

    std::wstring GetEditText(HWND edit)
    {
        int len = GetWindowTextLengthW(edit);
        std::wstring s(len, L'\0');
        GetWindowTextW(edit, s.data(), len + 1);
        return s;
    }

    HWND CreateLabel(HWND parent, const wchar_t* text, int x, int y, int w, int h)
    {
        return CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE,
                               x, y, w, h, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
    }

    BOOL CALLBACK EnumApplyFont(HWND child, LPARAM)
    {
        SendMessageW(child, WM_SETFONT, (WPARAM)s_dlgFont, TRUE);
        return TRUE;
    }

    LRESULT CALLBACK SettingsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg)
        {
        case WM_CREATE:
        {
            HINSTANCE hInst = GetModuleHandleW(nullptr);

            CreateLabel(hwnd, L"服务器地址:", 20, 22, 90, 20);
            g_urlEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 120, 20, 310, 22,
                hwnd, (HMENU)(INT_PTR)IDC_EDIT_URL, hInst, nullptr);

            // 客户端令牌明文显示
            CreateLabel(hwnd, L"客户端令牌:", 20, 56, 90, 20);
            g_tokenEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 120, 54, 310, 22,
                hwnd, (HMENU)(INT_PTR)IDC_EDIT_TOKEN, hInst, nullptr);

            // 底部按钮
            CreateWindowExW(0, L"BUTTON", L"保存",
                WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 230, 91, 90, 30,
                hwnd, (HMENU)(INT_PTR)IDOK, hInst, nullptr);
            CreateWindowExW(0, L"BUTTON", L"取消",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 330, 91, 90, 30,
                hwnd, (HMENU)(INT_PTR)IDCANCEL, hInst, nullptr);

            // 填充当前配置
            SetEditText(g_urlEdit, g_config.serverUrl);
            SetEditText(g_tokenEdit, g_config.clientToken);

            // 统一 UI 字体(微软雅黑 12px),应用到所有子控件
            s_dlgFont = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                    CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
            EnumChildWindows(hwnd, EnumApplyFont, 0);
            return 0;
        }

        case WM_COMMAND:
            switch (LOWORD(wp))
            {
            case IDOK:
            {
                // 收集配置并保存(重连功能固定启用,不再提供开关)
                g_config.serverUrl = GetEditText(g_urlEdit);
                g_config.clientToken = GetEditText(g_tokenEdit);

                // 校验服务器地址:去首尾空白后必须以 http:// 或 https:// 开头
                std::wstring url = g_config.serverUrl;
                size_t b = url.find_first_not_of(L" \t\r\n");
                size_t e = url.find_last_not_of(L" \t\r\n");
                url = (b == std::wstring::npos) ? L"" : url.substr(b, e - b + 1);
                g_config.serverUrl = url;

                if (url.empty() ||
                    (url.rfind(L"http://", 0) != 0 && url.rfind(L"https://", 0) != 0))
                {
                    MessageBoxW(hwnd, L"服务器地址必须以 http:// 或 https:// 开头",
                                L"GotifyInbox 设置", MB_OK | MB_ICONWARNING);
                    return 0; // 校验失败,不关闭对话框
                }

                g_config.Save();
                DestroyWindow(hwnd);
                return 0;
            }
            case IDCANCEL:
                DestroyWindow(hwnd);
                return 0;
            }
            return 0;

        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            // 只退出本对话框的模态循环,不能 PostQuitMessage(会连主消息循环一起退出)
            s_dialogClosed = true;
            if (s_dlgFont) { DeleteObject(s_dlgFont); s_dlgFont = nullptr; }
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

void ShowSettingsDialog(HWND parent)
{
    HINSTANCE hInst = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);

    static bool registered = false;
    if (!registered)
    {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = SettingsProc;
        wc.hInstance = hInst;
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
        wc.lpszClassName = L"GotifyInboxSettingsWnd";
        RegisterClassExW(&wc);
        registered = true;
    }

    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"GotifyInboxSettingsWnd", L"GotifyInbox 设置",
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, 460, 175,
        parent, nullptr, hInst, nullptr);
    if (!dlg)
    {
        return;
    }

    // 模态:禁用父窗口并进入本地消息循环
    EnableWindow(parent, FALSE);
    ShowWindow(dlg, SW_SHOW);

    s_dialogClosed = false;
    MSG msg;
    while (!s_dialogClosed && GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        if (msg.hwnd == dlg || IsChild(dlg, msg.hwnd))
        {
            IsDialogMessageW(dlg, &msg);
        }
        else
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);
}
