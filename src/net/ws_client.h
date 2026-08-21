#pragma once
#ifndef SECURITY_WIN32
#define SECURITY_WIN32 // security.h 需要此宏;避免依赖构建命令行 /DSECURITY_WIN32
#endif
#include <windows.h>
#include <security.h>
#include <schnlsp.h>
#include <atomic>
#include <cstdint>
#include <string>

// WebSocket 工作线程 -> UI 线程投递的自定义消息(在 resource.h 定义 WM_APP_WS_*):
//   WM_APP_WS_MESSAGE: lParam = new std::string(UTF-8 JSON 文本),UI 线程负责 delete
//   WM_APP_WS_STATUS : wParam = 1 已连接 / 0 瞬时断开(自动重连) / 2 永久错误(停止自动重连),
//                      lParam = new std::wstring(状态文本),UI 线程负责 delete

// WebSocket 客户端(WinSock2 + 自实现 RFC 6455 + Schannel TLS):
//   工作线程连接/接收,通过 PostMessage 将消息与状态投递到 UI 线程
class WsClient
{
public:
    WsClient() = default;
    ~WsClient();

    // 启动连接(serverUrl 形如 https://host:port 或 http://host:port)
    bool Start(HWND hwnd, const std::wstring& serverUrl, const std::wstring& token);

    // 停止并等待工作线程退出(绝不 TerminateThread;socket 句柄由工作线程独占关闭)
    void Stop();

    bool IsRunning() const { return running_.load(); }

private:
    static unsigned __stdcall WorkerProc(void* arg);
    void Worker();
    bool RecvExact(unsigned char* buffer, int n);
    void PostStatus(const wchar_t* text, bool connected, bool permanent = false);

    // select 等待可读,最多 timeoutMs 毫秒;每 200ms 检查运行状态以便快速退出
    bool WaitReadable(int timeoutMs);

    // TLS(Schannel)相关
    bool TlsHandshake(const std::wstring& host);
    bool SendAll(const char* data, int len);
    int  TlsRead(char* buffer, int maxLen);   // 返回实际读取字节数(0=连接关闭,-1=失败)
    void TlsCleanup();

    HWND hwnd_ = nullptr;
    std::wstring url_;
    std::wstring token_;
    std::atomic<bool> running_{false};       // 跨线程停止标志(UI Stop / 工作线程)
    HANDLE thread_ = nullptr;
    std::atomic<intptr_t> sock_{-1};         // SOCKET 句柄(跨线程访问,避免头文件引入 winsock2.h)
    std::atomic<HANDLE> dnsCancel_{nullptr}; // GetAddrInfoExW 取消句柄(Stop 跨线程 GetAddrInfoExCancel)

    // TLS 状态
    bool tls_ = false;
    CredHandle hCred_ = {};
    CtxtHandle hCtx_ = {};
    SecPkgContext_StreamSizes tlsSizes_ = {};
    std::string tlsIn_;   // 已解密待消费的明文缓冲
    std::string tlsRaw_;  // 已收到待解密的密文缓冲
};
