#include <windows.h>
#include <commctrl.h>
#include <cstdio>
#include <cstdarg>
#include <wchar.h>
#include <tlhelp32.h>
#include "resource.h"
#include "core/config.h"
#include "ui/main_window.h"
#include "ui/settings_dlg.h"

// 找出地址所属模块名(便于定位崩溃位置)
static void ModuleNameOf(void* addr, char* out, size_t outLen)
{
    out[0] = '\0';
    MODULEENTRY32W me = {};
    me.dwSize = sizeof(me);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return;
    bool found = false;
    if (Module32FirstW(snap, &me))
    {
        do
        {
            uintptr_t base = (uintptr_t)me.modBaseAddr;
            if ((uintptr_t)addr >= base && (uintptr_t)addr < base + me.modBaseSize)
            {
                char nameA[64] = {};
                WideCharToMultiByte(CP_ACP, 0, me.szModule, -1, nameA, 64, nullptr, nullptr);
                snprintf(out, outLen, "%s+0x%llX", nameA,
                         (unsigned long long)((uintptr_t)addr - base));
                found = true;
                break;
            }
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    if (!found) snprintf(out, outLen, "0x%p", addr);
}

// 崩溃处理器:写崩溃日志(异常代码、地址、所属模块、调用栈)后继续默认处理
// 安全约束:处理器内不做堆分配(堆损坏时可能递归崩溃),使用栈缓冲 + WriteFile 追加;
// 优先写 exe 所在目录,不可写时回退到 %TEMP%
LONG WINAPI CrashHandler(EXCEPTION_POINTERS* ep)
{
    char buf[4096];
    int len = 0;
    auto Append = [&](const char* fmt, ...)
    {
        if (len >= (int)sizeof(buf) - 256) return; // 预留尾部空间
        va_list ap;
        va_start(ap, fmt);
        int n = vsnprintf(buf + len, sizeof(buf) - (size_t)len, fmt, ap);
        va_end(ap);
        if (n > 0) len += n;
        if (len > (int)sizeof(buf) - 1) len = (int)sizeof(buf) - 1;
    };

    Append("[tick=%llu] code=0x%08lX addr=%p tid=%lu\n",
           (unsigned long long)GetTickCount64(),
           (unsigned long)ep->ExceptionRecord->ExceptionCode,
           ep->ExceptionRecord->ExceptionAddress, GetCurrentThreadId());

    // 寄存器上下文(x64)
    if (ep->ContextRecord)
    {
        auto* c = ep->ContextRecord;
        Append("  rax=%p rbx=%p rcx=%p rdx=%p\n",
               (void*)c->Rax, (void*)c->Rbx, (void*)c->Rcx, (void*)c->Rdx);
        Append("  rsi=%p rdi=%p rbp=%p rsp=%p\n",
               (void*)c->Rsi, (void*)c->Rdi, (void*)c->Rbp, (void*)c->Rsp);
        Append("  r8=%p r9=%p r10=%p r11=%p r12=%p r13=%p r14=%p r15=%p\n",
               (void*)c->R8, (void*)c->R9, (void*)c->R10, (void*)c->R11,
               (void*)c->R12, (void*)c->R13, (void*)c->R14, (void*)c->R15);
    }

    // 异常地址所属模块
    {
        char mod[128] = {};
        ModuleNameOf(ep->ExceptionRecord->ExceptionAddress, mod, sizeof(mod));
        Append("  module: %s\n", mod);
    }

    // 调用栈(前 16 帧)
    {
        void* stack[16] = {};
        typedef USHORT(WINAPI* RtlCaptureStackBackTraceFn)(ULONG, ULONG, PVOID*, PULONG);
        static RtlCaptureStackBackTraceFn fn = nullptr;
        if (!fn)
        {
            HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
            if (ntdll) fn = (RtlCaptureStackBackTraceFn)GetProcAddress(ntdll, "RtlCaptureStackBackTrace");
        }
        if (fn)
        {
            USHORT n = fn(0, 16, stack, nullptr);
            for (USHORT i = 0; i < n; i++)
            {
                char m[128] = {};
                ModuleNameOf(stack[i], m, sizeof(m));
                Append("  #%u %s\n", i, m);
            }
        }
    }

    // 追加写入 crash.log:优先 exe 目录,失败回退 %TEMP%
    wchar_t logPath[MAX_PATH] = {};
    {
        wchar_t exePath[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        wchar_t* slash = wcsrchr(exePath, L'\\');
        if (slash) slash[1] = L'\0';
        wsprintfW(logPath, L"%scrash.log", exePath);
    }
    HANDLE hFile = CreateFileW(logPath, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                               OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
    {
        wchar_t tmp[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, tmp);
        wsprintfW(logPath, L"%scrash.log", tmp);
        hFile = CreateFileW(logPath, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    if (hFile != INVALID_HANDLE_VALUE)
    {
        DWORD written = 0;
        WriteFile(hFile, buf, (DWORD)(len > 0 ? len : 0), &written, nullptr);
        CloseHandle(hFile);
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow)
{
    SetUnhandledExceptionFilter(CrashHandler);

    // 加载配置(无配置文件时使用默认值)
    g_config.Load();

    // 单实例互斥
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"GotifyInbox");
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS)
    {
        MessageBoxW(nullptr, L"GotifyInbox 已在运行中！", L"GotifyInbox",
                    MB_OK | MB_ICONINFORMATION);
        if (mutex) CloseHandle(mutex);
        return 0;
    }

    INITCOMMONCONTROLSEX icc = {};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_WIN95_CLASSES | ICC_BAR_CLASSES;
    InitCommonControlsEx(&icc);

    if (!MainWindow::RegisterClass(hInstance))
    {
        CloseHandle(mutex);
        return 1;
    }

    if (!MainWindow::Create(hInstance, nCmdShow))
    {
        CloseHandle(mutex);
        return 1;
    }

    // 配置完整:启动即最小化到托盘并自动连接
    // 未配置:显示主界面并提示配置
    if (!g_config.serverUrl.empty() && !g_config.clientToken.empty())
    {
        ShowWindow(MainWindow::Hwnd(), SW_HIDE);
        MainWindow::AutoConnect();
    }
    else
    {
        ShowWindow(MainWindow::Hwnd(), SW_SHOW);
        MessageBoxW(MainWindow::Hwnd(),
                    L"请先在设置中配置服务器地址和客户端令牌。\n"
                    L"配置完成后程序将自动连接服务器。",
                    L"GotifyInbox", MB_OK | MB_ICONINFORMATION);
        ShowSettingsDialog(MainWindow::Hwnd());
        MainWindow::AutoConnect(); // 保存配置后自动连接
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    MainWindow::DestroyTray();
    CloseHandle(mutex);
    return (int)msg.wParam;
}
