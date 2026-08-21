#include "ui/main_window.h"
#include "ui/tray.h"
#include "ui/settings_dlg.h"
#include "net/ws_client.h"
#include "core/config.h"
#include "core/models.h"
#include "core/json.h"
#include "core/history.h"
#include "util/utf8.h"
#include "resource.h"
#include <commctrl.h>
#include <vector>

namespace
{
    HWND s_hwnd = nullptr;
    HINSTANCE s_hInst = nullptr;
    HICON s_icon = nullptr;

    // 子控件句柄
    HWND g_btnClear = nullptr;
    HWND g_statusText = nullptr;
    HWND g_list = nullptr;

    // 托盘闪烁(新消息到达且主界面隐藏时,500ms 交替图标,共 6 秒)
    constexpr UINT_PTR TIMER_TRAY_FLASH = 1;
    constexpr int TRAY_FLASH_MAX_TICKS = 12;
    HICON s_flashIcon = nullptr;
    bool s_flashing = false;
    int s_flashTicks = 0;

    // 断线重连(指数退避:2s,4s,8s,16s,32s,60s 封顶)
    constexpr UINT_PTR TIMER_RECONNECT = 2;
    constexpr int RECONNECT_BASE_MS = 2000;
    constexpr int RECONNECT_MAX_MS = 60000;
    int g_reconnectTries = 0;

    // 历史保存节流(消息密集时合并为 1s 写一次盘)
    constexpr UINT_PTR TIMER_HISTORY_SAVE = 3;
    bool g_historyDirty = false;

    // 加载历史期间跳过逐条保存(避免反复全量写盘)
    bool g_loadingHistory = false;

    // 统一 UI 字体(11px)
    HFONT s_uiFont = nullptr;

    HFONT CreateUiFont()
    {
        // 微软雅黑 12px(系统默认字体在中文环境易落到宋体,渲染粗糙)
        return CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    }

    void ApplyUiFont(HWND ctrl)
    {
        SendMessageW(ctrl, WM_SETFONT, (WPARAM)s_uiFont, TRUE);
    }

    // WebSocket 客户端与消息列表(最新在前)
    WsClient g_ws;
    std::vector<GotifyMessage> g_messages;

    constexpr int MAX_MESSAGES = 1000;

    void SetStatusText(const wchar_t* text)
    {
        SetWindowTextW(g_statusText, text);
    }

    void StartConnect(HWND hwnd)
    {
        if (g_ws.IsRunning())
        {
            return; // 已连接,保持
        }

        if (g_config.serverUrl.empty() || g_config.clientToken.empty())
        {
            return; // 未配置,由 main.cpp 引导配置
        }

        SetStatusText(L"正在连接...");
        if (!g_ws.Start(hwnd, g_config.serverUrl, g_config.clientToken))
        {
            SetStatusText(L"连接启动失败");
        }
    }

    void AddMessageToList(HWND hwnd, const GotifyMessage& m)
    {
        // 按消息 id 去重(历史与实时叠加、重连等场景)
        for (const auto& existing : g_messages)
        {
            if (existing.id == m.id)
            {
                return;
            }
        }

        // 超出上限时移除最旧的
        if (g_messages.size() >= MAX_MESSAGES)
        {
            g_messages.pop_back();
            ListView_DeleteItem(g_list, MAX_MESSAGES - 1);
        }

        g_messages.insert(g_messages.begin(), m);

        // 插入列表顶部:[应用名] 标题 - 内容
        std::wstring app = Utf8ToWide(m.appname);
        std::wstring title = Utf8ToWide(m.title);
        std::wstring body = Utf8ToWide(m.message);

        std::wstring display;
        if (!app.empty())
        {
            display += L"[";
            display += app;
            display += L"] ";
        }
        if (!title.empty())
        {
            display += title;
        }
        if (!title.empty() && !body.empty())
        {
            display += L" - ";
        }
        display += body;
        if (display.empty())
        {
            display = L"(空消息)";
        }

        LVITEMW item = {};
        item.mask = LVIF_TEXT;
        item.iItem = 0;
        item.pszText = display.data();
        ListView_InsertItem(g_list, &item);

        // 持久化历史(加载历史期间跳过;节流合并写盘)
        if (!g_loadingHistory)
        {
            g_historyDirty = true;
            SetTimer(hwnd, TIMER_HISTORY_SAVE, 1000, nullptr); // 重复调用会重置计时
        }
    }

    // 新消息到达:主界面隐藏时托盘图标闪烁提醒(可见时不闪,列表已实时更新)
    void StartFlash(HWND hwnd)
    {
        if (s_flashing || IsWindowVisible(hwnd))
        {
            return;
        }
        s_flashing = true;
        s_flashTicks = 0;
        SetTimer(hwnd, TIMER_TRAY_FLASH, 500, nullptr);
    }

    void StopFlash(HWND hwnd)
    {
        if (!s_flashing)
        {
            return;
        }
        s_flashing = false;
        KillTimer(hwnd, TIMER_TRAY_FLASH);
        Tray::SetIcon(s_icon);
    }

    LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    void OnCreate(HWND hwnd)
    {
        // 统一 UI 字体(微软雅黑 12px)
        s_uiFont = CreateUiFont();

        // 顶部工具栏:状态文本(左) | 清空(右)
        g_statusText = CreateWindowExW(0, L"STATIC", L"未连接",
            WS_CHILD | WS_VISIBLE, 10, 12, 560, 20,
            hwnd, (HMENU)(INT_PTR)IDC_STATUS_TEXT, s_hInst, nullptr);

        g_btnClear = CreateWindowExW(0, L"BUTTON", L"清空",
            WS_CHILD | WS_VISIBLE, 710, 8, 80, 28,
            hwnd, (HMENU)(INT_PTR)IDC_BTN_CLEAR, s_hInst, nullptr);

        ApplyUiFont(g_statusText);
        ApplyUiFont(g_btnClear);

        // 消息列表(普通列表;文本由 ListView 内部存储,1000 条上限无性能压力)
        g_list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_NOSORTHEADER,
            10, 46, 780, 520,
            hwnd, (HMENU)(INT_PTR)IDC_MAIN_LIST, s_hInst, nullptr);
        ListView_SetExtendedListViewStyle(g_list,
            LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        ApplyUiFont(g_list);

        LVCOLUMNW col = {};
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.cx = 780;
        col.pszText = const_cast<LPWSTR>(L"消息");
        ListView_InsertColumn(g_list, 0, &col);

        // 托盘图标
        s_icon = LoadIconW(s_hInst, MAKEINTRESOURCEW(IDI_APP));
        s_flashIcon = LoadIconW(nullptr, IDI_INFORMATION); // 闪烁态:系统提示图标
        Tray::Add(hwnd, s_icon, L"GotifyInbox");

        // 加载历史消息(与实时消息按 id 去重;加载完统一写一次规范化)
        g_loadingHistory = true;
        std::vector<GotifyMessage> history;
        if (LoadHistory(history))
        {
            // 文件按"最新在前"存储,而 AddMessageToList 每次插到顶部,
            // 故逆序遍历(最旧在前)以恢复"最新在前"的显示顺序
            for (auto it = history.rbegin(); it != history.rend(); ++it)
            {
                AddMessageToList(hwnd, *it);
            }
        }
        g_loadingHistory = false;
        if (!g_messages.empty())
        {
            SaveHistory(g_messages);
        }

        // 数据目录回退提示(exe 目录不可写时数据保存在 %APPDATA%\GotifyInbox)
        if (DataDir() != ExeDir())
        {
            SetStatusText(L"exe 目录不可写,数据保存在 %APPDATA%\\GotifyInbox");
        }
    }

    void ShowMainWindow(HWND hwnd)
    {
        StopFlash(hwnd);
        ShowWindow(hwnd, SW_SHOW);
        SetForegroundWindow(hwnd);
    }

    // 复制选中消息到剪贴板(数据取自 g_messages 缓存,不走 LVM_GETITEMTEXT)
    void CopyMessageToClipboard(HWND hwnd, int index)
    {
        if (index < 0 || index >= (int)g_messages.size())
        {
            return;
        }

        const GotifyMessage& m = g_messages[index];
        std::wstring title = Utf8ToWide(m.title);
        std::wstring body = Utf8ToWide(m.message);

        std::wstring text;
        if (!title.empty())
        {
            text += title;
            text += L"\r\n";
        }
        if (!body.empty())
        {
            text += body;
        }
        if (!m.date.empty())
        {
            std::wstring date = Utf8ToWide(m.date);
            if (!date.empty())
            {
                text += L"\r\n\r\n时间: ";
                text += date;
            }
        }
        if (text.empty())
        {
            return;
        }

        if (!OpenClipboard(hwnd))
        {
            return;
        }
        EmptyClipboard();

        const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (hMem)
        {
            void* p = GlobalLock(hMem);
            if (p)
            {
                memcpy(p, text.c_str(), bytes);
                GlobalUnlock(hMem);
                if (!SetClipboardData(CF_UNICODETEXT, hMem))
                {
                    GlobalFree(hMem); // 失败时内存仍归调用者
                }
            }
            else
            {
                GlobalFree(hMem);
            }
        }
        CloseClipboard();
    }

    // 列表右键菜单:复制消息
    void ShowListContextMenu(HWND hwnd, int index)
    {
        if (index < 0 || index >= (int)g_messages.size())
        {
            return;
        }

        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, IDM_LIST_COPY, L"复制消息");

        POINT pt;
        GetCursorPos(&pt);
        int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                                 pt.x, pt.y, 0, hwnd, nullptr);
        DestroyMenu(menu);

        if (cmd == IDM_LIST_COPY)
        {
            CopyMessageToClipboard(hwnd, index);
        }
    }

    void ShowTrayMenu(HWND hwnd)
    {
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, IDM_TRAY_SHOW, L"消息");
        AppendMenuW(menu, MF_STRING, IDM_TRAY_SETTINGS, L"设置");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, IDM_TRAY_EXIT, L"退出");

        POINT pt;
        GetCursorPos(&pt);
        SetForegroundWindow(hwnd);
        int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                 pt.x, pt.y, 0, hwnd, nullptr);
        DestroyMenu(menu);

        switch (cmd)
        {
        case IDM_TRAY_SHOW:
            ShowMainWindow(hwnd);
            break;
        case IDM_TRAY_SETTINGS:
            ShowSettingsDialog(hwnd);
            StartConnect(hwnd); // 保存配置后自动连接
            break;
        case IDM_TRAY_EXIT:
            DestroyWindow(hwnd);
            break;
        }
    }

    LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg)
        {
        case WM_CREATE:
            OnCreate(hwnd);
            return 0;

        case WM_COMMAND:
            switch (LOWORD(wp))
            {
            case IDC_BTN_CLEAR:
                ListView_DeleteAllItems(g_list);
                g_messages.clear();
                SaveHistory(g_messages); // 清空历史
                return 0;
            }
            return 0;

        case WM_NOTIFY:
        {
            const NMHDR* hdr = reinterpret_cast<const NMHDR*>(lp);
            if (hdr->hwndFrom == g_list && hdr->code == NM_RCLICK)
            {
                // 命中测试得到右键所在行
                POINT pt;
                GetCursorPos(&pt);
                ScreenToClient(g_list, &pt);

                LVHITTESTINFO ht = {};
                ht.pt = pt;
                const int index = ListView_HitTest(g_list, &ht);
                if (index >= 0)
                {
                    ShowListContextMenu(hwnd, index);
                }
            }
            return 0;
        }

        case WM_TIMER:
            if (wp == TIMER_TRAY_FLASH)
            {
                ++s_flashTicks;
                Tray::SetIcon((s_flashTicks & 1) ? s_flashIcon : s_icon);
                if (s_flashTicks >= TRAY_FLASH_MAX_TICKS)
                {
                    StopFlash(hwnd);
                }
            }
            else if (wp == TIMER_RECONNECT)
            {
                KillTimer(hwnd, TIMER_RECONNECT);
                StartConnect(hwnd); // 指数退避后的自动重连
            }
            else if (wp == TIMER_HISTORY_SAVE)
            {
                KillTimer(hwnd, TIMER_HISTORY_SAVE);
                if (g_historyDirty)
                {
                    g_historyDirty = false;
                    SaveHistory(g_messages);
                }
            }
            return 0;

        case WM_APP_WS_MESSAGE:
        {
            // lParam = new std::string(UTF-8 JSON)
            auto* text = reinterpret_cast<std::string*>(lp);
            if (text)
            {
                json::Value root;
                if (json::Parse(*text, root))
                {
                    GotifyMessage m;
                    if (GotifyMessage::ParseFromJson(root, m))
                    {
                        AddMessageToList(hwnd, m);
                        StartFlash(hwnd); // 窗口隐藏时托盘闪烁提醒
                    }
                }
                delete text;
            }
            return 0;
        }

        case WM_APP_WS_STATUS:
        {
            // wParam = 1 已连接 / 0 瞬时断开(自动重连) / 2 永久错误(停止自动重连)
            // lParam = new std::wstring(状态文本)
            auto* status = reinterpret_cast<std::wstring*>(lp);
            if (status)
            {
                SetStatusText(status->c_str());
                delete status;
            }

            if (wp == 1) // 已连接:重置重连退避
            {
                g_reconnectTries = 0;
                KillTimer(hwnd, TIMER_RECONNECT);
            }
            else if (wp == 0) // 瞬时断开:指数退避计划重连
            {
                int delay = RECONNECT_BASE_MS << g_reconnectTries;
                if (delay > RECONNECT_MAX_MS) delay = RECONNECT_MAX_MS;
                if (g_reconnectTries < 5) ++g_reconnectTries; // 退避封顶后不再增长

                std::wstring reconnectMsg = L"连接已断开," + std::to_wstring(delay / 1000) + L" 秒后重连";
                SetStatusText(reconnectMsg.c_str());
                SetTimer(hwnd, TIMER_RECONNECT, (UINT)delay, nullptr);
            }
            // wp == 2:永久错误(URL 无效/DNS 失败/证书失败/认证失败),状态文本已设置,
            // 不安排自动重连;用户修改设置保存后由 StartConnect 手动恢复
            return 0;
        }

        case WM_TRAYICON:
            if (LOWORD(lp) == WM_LBUTTONUP)
            {
                ShowMainWindow(hwnd);
            }
            else if (LOWORD(lp) == WM_RBUTTONUP)
            {
                ShowTrayMenu(hwnd);
            }
            return 0;

        case WM_CLOSE:
            // 关闭窗口时隐藏到托盘,仅通过托盘菜单"退出"真正退出
            ShowWindow(hwnd, SW_HIDE);
            return 0;

        case WM_DESTROY:
            StopFlash(hwnd);
            // 兜底:退出前落盘未保存的历史
            if (g_historyDirty)
            {
                g_historyDirty = false;
                SaveHistory(g_messages);
            }
            g_ws.Stop();
            Tray::Remove();
            if (s_icon) { DestroyIcon(s_icon); s_icon = nullptr; }
            if (s_uiFont) { DeleteObject(s_uiFont); s_uiFont = nullptr; }
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

bool MainWindow::RegisterClass(HINSTANCE hInst)
{
    s_hInst = hInst;

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APP));
    wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"GotifyInboxMainWnd";
    wc.hIconSm = wc.hIcon;

    return RegisterClassExW(&wc) != 0;
}

bool MainWindow::Create(HINSTANCE hInst, int nCmdShow)
{
    // 固定大小窗口:无 WS_THICKFRAME(不可拖拽调整)/无 WS_MAXIMIZEBOX(不可最大化)
    constexpr DWORD FIXED_STYLE = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;

    s_hwnd = CreateWindowExW(0, L"GotifyInboxMainWnd", L"GotifyInbox",
        FIXED_STYLE, CW_USEDEFAULT, CW_USEDEFAULT, 820, 620,
        nullptr, nullptr, hInst, nullptr);
    if (!s_hwnd)
    {
        return false;
    }
    ShowWindow(s_hwnd, nCmdShow);
    return true;
}

void MainWindow::DestroyTray()
{
    Tray::Remove();
}

HWND MainWindow::Hwnd()
{
    return s_hwnd;
}

void MainWindow::AutoConnect()
{
    if (s_hwnd)
    {
        StartConnect(s_hwnd);
    }
}
