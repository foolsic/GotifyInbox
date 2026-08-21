#pragma once

// 资源
#define IDI_APP        1
#define IDR_MANIFEST   2

// 托盘
#define WM_TRAYICON   (WM_APP + 1)
#define IDM_TRAY_SHOW      1001
#define IDM_TRAY_SETTINGS  1002
#define IDM_TRAY_EXIT      1003

// 列表右键菜单
#define IDM_LIST_COPY      1004

// WebSocket 工作线程 -> UI 线程
#define WM_APP_WS_MESSAGE (WM_APP + 2)
#define WM_APP_WS_STATUS  (WM_APP + 3)

// 控件 ID
#define IDC_MAIN_LIST   2001
#define IDC_BTN_CLEAR   2004
#define IDC_STATUS_TEXT 2005

// 设置窗口控件
#define IDC_EDIT_URL     2101
#define IDC_EDIT_TOKEN   2102
