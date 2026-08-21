#pragma once
#include <windows.h>

// 主窗口:管理窗口生命周期、子控件、托盘交互
namespace MainWindow
{
    bool RegisterClass(HINSTANCE hInst);
    bool Create(HINSTANCE hInst, int nCmdShow);
    void DestroyTray();
    HWND Hwnd();

    // 配置完整时自动连接;未配置时无操作(由 main.cpp 引导配置)
    void AutoConnect();
}
