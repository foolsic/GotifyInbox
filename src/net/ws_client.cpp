// winsock2.h 必须在任何 windows.h 之前包含,否则与 winsock.h 冲突
#include <winsock2.h>
#include <ws2tcpip.h>
#include <wincrypt.h>
#include <bcrypt.h>
#include "net/ws_client.h"
#include "resource.h"
#include "util/utf8.h"
#include <process.h>
#include <cstdlib>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "advapi32.lib")

// WS 续帧拼接总长度上限(防御性,正常消息远小于此)
constexpr size_t MAX_FRAGMENT_BYTES = 4 * 1024 * 1024;

// 本机 WinHTTP 的 WebSocket 升级选项(WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET)返回
// ERROR_INVALID_PARAMETER 且 Connection 头被 WinHTTP 锁定,故改用 WinSock2 自实现 RFC 6455;
// HTTPS 场景通过 Schannel 提供 TLS(wss),证书使用系统信任库自动验证。

WsClient::~WsClient()
{
    Stop();
}

bool WsClient::Start(HWND hwnd, const std::wstring& serverUrl, const std::wstring& token)
{
    Stop();

    hwnd_ = hwnd;
    url_ = serverUrl;
    token_ = token;
    running_ = true;

    thread_ = (HANDLE)_beginthreadex(nullptr, 0, WorkerProc, this, 0, nullptr);
    if (!thread_)
    {
        // 线程创建失败:复位状态,避免 IsRunning() 恒真导致后续连接被 StartConnect 短路
        running_ = false;
        sock_ = (intptr_t)INVALID_SOCKET;
        return false;
    }
    return true;
}

void WsClient::Stop()
{
    running_ = false;

    // 唤醒阻塞中的 socket 操作;句柄归工作线程独占,此处只 shutdown 不关闭,
    // 避免跨线程 closesocket 与工作线程 recv/send 竞争(句柄可能被复用)
    if (sock_ != (intptr_t)INVALID_SOCKET)
    {
        shutdown((SOCKET)sock_, SD_BOTH);
    }

    // 取消阻塞中的 DNS 解析(GetAddrInfoExW + GetAddrInfoExCancel,参数为句柄指针)
    if (HANDLE h = dnsCancel_.load())
    {
        GetAddrInfoExCancel(&h);
    }

    if (thread_)
    {
        // 等待工作线程自然退出:所有等待点均 200ms 轮询运行状态,DNS 可被取消;
        // 绝不 TerminateThread(可能遗留 Schannel/解析器内部锁导致后续死锁)
        for (int i = 0; i < 40; i++)
        {
            if (WaitForSingleObject(thread_, 500) == WAIT_OBJECT_0) break;
            if (HANDLE h = dnsCancel_.load()) GetAddrInfoExCancel(&h);
        }
        // 兜底:继续等待(解析器自身有超时,线程最终必然退出)
        WaitForSingleObject(thread_, 30000);
        CloseHandle(thread_);
        thread_ = nullptr;
        dnsCancel_ = nullptr;
    }
}

// select 等待可读,最多 timeoutMs 毫秒;每 200ms 检查运行状态以便快速退出
bool WsClient::WaitReadable(int timeoutMs)
{
    for (int waited = 0; waited < timeoutMs && running_; waited += 200)
    {
        intptr_t s = sock_;
        if (s == (intptr_t)INVALID_SOCKET)
        {
            return false;
        }

        fd_set rf;
        FD_ZERO(&rf);
        FD_SET((SOCKET)s, &rf);
        timeval tv = {};
        tv.tv_sec = 0;
        tv.tv_usec = 200000;
        int rc = select(0, &rf, nullptr, nullptr, &tv);
        if (rc > 0)
        {
            return true;
        }
        if (rc < 0)
        {
            return false;
        }
    }
    return false; // 超时或已停止
}

void WsClient::TlsCleanup()
{
    if (tls_ || hCtx_.dwLower || hCtx_.dwUpper)
    {
        DeleteSecurityContext(&hCtx_);
        hCtx_ = {};
    }
    if (hCred_.dwLower || hCred_.dwUpper)
    {
        FreeCredentialsHandle(&hCred_);
        hCred_ = {};
    }
    tls_ = false;
    tlsIn_.clear();
    tlsRaw_.clear();
}

void WsClient::PostStatus(const wchar_t* text, bool connected, bool permanent)
{
    if (!hwnd_ || !IsWindow(hwnd_))
    {
        return;
    }
    auto* msg = new std::wstring(text);
    WPARAM wp = connected ? 1 : (permanent ? 2 : 0);
    if (!PostMessageW(hwnd_, WM_APP_WS_STATUS, wp, (LPARAM)msg))
    {
        delete msg; // 投递失败(窗口已销毁):释放,避免泄漏
    }
}

unsigned __stdcall WsClient::WorkerProc(void* arg)
{
    static_cast<WsClient*>(arg)->Worker();
    return 0;
}

namespace
{
    struct UrlParts
    {
        std::wstring host;
        std::wstring path;
        int port = 0;
        bool secure = false;
    };

    bool ParseUrl(const std::wstring& url, UrlParts& out)
    {
        std::wstring s = url;
        while (!s.empty() && s.back() == L'/') s.pop_back();
        if (s.empty()) return false;

        if (s.rfind(L"https://", 0) == 0)
        {
            out.secure = true;
            s = s.substr(8);
        }
        else if (s.rfind(L"http://", 0) == 0)
        {
            out.secure = false;
            s = s.substr(7);
        }
        else
        {
            return false;
        }

        size_t slash = s.find(L'/');
        std::wstring hostport = (slash == std::wstring::npos) ? s : s.substr(0, slash);
        out.path = (slash == std::wstring::npos) ? L"" : s.substr(slash);

        if (!hostport.empty() && hostport.front() == L'[')
        {
            // IPv6 字面量形式:[::1]:8080
            size_t close = hostport.find(L']');
            if (close == std::wstring::npos)
            {
                return false;
            }
            out.host = hostport.substr(1, close - 1);
            size_t colon = hostport.find(L':', close);
            if (colon != std::wstring::npos)
            {
                out.port = _wtoi(hostport.substr(colon + 1).c_str());
            }
            else
            {
                out.port = out.secure ? 443 : 80;
            }
        }
        else
        {
            size_t colon = hostport.find(L':');
            if (colon != std::wstring::npos)
            {
                out.host = hostport.substr(0, colon);
                out.port = _wtoi(hostport.substr(colon + 1).c_str());
            }
            else
            {
                out.host = hostport;
                out.port = out.secure ? 443 : 80;
            }
        }

        return !out.host.empty() && out.port > 0;
    }

    std::string Base64Encode(const unsigned char* data, int len)
    {
        static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        for (int i = 0; i < len; i += 3)
        {
            unsigned v = (data[i] << 16) | ((i + 1 < len ? data[i + 1] : 0) << 8) | (i + 2 < len ? data[i + 2] : 0);
            out += t[(v >> 18) & 63];
            out += t[(v >> 12) & 63];
            out += (i + 1 < len) ? t[(v >> 6) & 63] : '=';
            out += (i + 2 < len) ? t[v & 63] : '=';
        }
        return out;
    }

    std::string MakeRandomKey()
    {
        unsigned char bytes[16];
        if (BCryptGenRandom(nullptr, bytes, sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        {
            return {}; // RNG 不可用:拒绝弱随机,由调用方按连接失败处理
        }
        return Base64Encode(bytes, 16);
    }

    // URL 编码(客户端令牌拼入 query 前必须编码)
    std::string UrlEncode(const std::string& s)
    {
        static const char* hex = "0123456789ABCDEF";
        std::string out;
        for (unsigned char c : s)
        {
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                c == '-' || c == '_' || c == '.' || c == '~')
            {
                out += (char)c;
            }
            else
            {
                out += '%';
                out += hex[c >> 4];
                out += hex[c & 0x0F];
            }
        }
        return out;
    }

    // 从 HTTP 响应头中取字段值(大小写不敏感)
    std::string GetHeaderValue(const std::string& headers, const std::string& name)
    {
        size_t pos = 0;
        while (pos < headers.size())
        {
            size_t lineEnd = headers.find("\r\n", pos);
            if (lineEnd == std::string::npos)
            {
                lineEnd = headers.size();
            }
            std::string line = headers.substr(pos, lineEnd - pos);
            size_t colon = line.find(':');
            if (colon != std::string::npos)
            {
                std::string field = line.substr(0, colon);
                std::string value = line.substr(colon + 1);
                while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
                {
                    value.erase(0, 1);
                }
                if (_stricmp(field.c_str(), name.c_str()) == 0)
                {
                    return value;
                }
            }
            pos = lineEnd + 2;
        }
        return "";
    }

    // SHA1 后 Base64(RFC 6455 Sec-WebSocket-Accept 校验)
    bool Sha1Base64(const std::string& data, std::string& out)
    {
        HCRYPTPROV prov = 0;
        HCRYPTHASH hash = 0;
        if (!CryptAcquireContextW(&prov, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT))
        {
            return false;
        }
        bool ok = CryptCreateHash(prov, CALG_SHA1, 0, 0, &hash) != FALSE;
        if (ok)
        {
            ok = CryptHashData(hash, (const BYTE*)data.data(), (DWORD)data.size(), 0) != FALSE;
        }
        if (ok)
        {
            BYTE digest[20] = {};
            DWORD len = sizeof(digest);
            ok = CryptGetHashParam(hash, HP_HASHVAL, digest, &len, 0) != FALSE;
            if (ok)
            {
                out = Base64Encode(digest, (int)len);
            }
        }
        if (hash)
        {
            CryptDestroyHash(hash);
        }
        CryptReleaseContext(prov, 0);
        return ok;
    }

    // 将 SECURITY_STATUS 转为可读描述
    const wchar_t* SecStatusText(SECURITY_STATUS st)
    {
        switch (st)
        {
        case SEC_E_OK: return L"成功";
        case SEC_E_INCOMPLETE_MESSAGE: return L"数据不完整";
        case SEC_E_UNTRUSTED_ROOT: return L"证书不受信任(根证书未安装)";
        case SEC_E_CERT_UNKNOWN: return L"证书无法验证";
        case SEC_E_CERT_EXPIRED: return L"证书已过期";
        case SEC_E_WRONG_PRINCIPAL: return L"证书域名不匹配";
        case SEC_E_INSUFFICIENT_MEMORY: return L"内存不足";
        case SEC_E_INTERNAL_ERROR: return L"内部错误";
        case SEC_E_NO_CREDENTIALS: return L"无可用凭据";
        case SEC_E_ALGORITHM_MISMATCH: return L"算法不匹配";
        case SEC_E_ILLEGAL_MESSAGE: return L"非法消息";
        case SEC_E_CONTEXT_EXPIRED: return L"会话已过期";
        case SEC_I_CONTINUE_NEEDED: return L"需要继续";
        case SEC_I_INCOMPLETE_CREDENTIALS: return L"需要客户端证书";
        case SEC_E_INVALID_TOKEN: return L"无效令牌";
        case SEC_E_NO_AUTHENTICATING_AUTHORITY: return L"无验证机构";
        case SEC_E_UNSUPPORTED_FUNCTION: return L"不支持的功能";
        case SEC_E_APPLICATION_PROTOCOL_MISMATCH: return L"应用协议不匹配";
        default: return L"未知错误";
        }
    }

    // 证书类错误:属永久性配置问题(证书不受信任/过期/域名不匹配/要求客户端证书等),
    // 自动重连无法恢复,应停止重连等待用户处理
    bool IsCertError(SECURITY_STATUS st)
    {
        switch (st)
        {
        case SEC_E_UNTRUSTED_ROOT:
        case SEC_E_CERT_UNKNOWN:
        case SEC_E_CERT_EXPIRED:
        case SEC_E_WRONG_PRINCIPAL:
        case SEC_E_NO_AUTHENTICATING_AUTHORITY:
        case SEC_E_INCOMPLETE_CREDENTIALS:
            return true;
        default:
            return false;
        }
    }
}

// 从 socket 精确读取 n 字节(处理短读;TLS 模式从解密缓冲取)
bool WsClient::RecvExact(unsigned char* buffer, int n)
{
    int got = 0;
    while (got < n && running_ && sock_ != (intptr_t)INVALID_SOCKET)
    {
        if (tls_)
        {
            // 先消费已解密的明文
            while (!tlsIn_.empty() && got < n)
            {
                int take = (int)tlsIn_.size();
                if (take > n - got) take = n - got;
                memcpy(buffer + got, tlsIn_.data(), take);
                tlsIn_.erase(0, take);
                got += take;
            }
            if (got >= n) break;
            // 明文不足,尝试读取并解密更多
            if (TlsRead(nullptr, 0) <= 0) return false;
        }
        else
        {
            int r = recv((SOCKET)sock_, (char*)buffer + got, n - got, 0);
            if (r <= 0) return false;
            got += r;
        }
    }
    return got == n;
}

// 发送全部数据(TLS 模式分块加密)
bool WsClient::SendAll(const char* data, int len)
{
    if (!tls_)
    {
        int sent = 0;
        while (sent < len)
        {
            int r = send((SOCKET)sock_, data + sent, len - sent, 0);
            if (r == SOCKET_ERROR) return false;
            sent += r;
        }
        return true;
    }

    // TLS:按 StreamSizes 分块 EncryptMessage
    int maxMsg = tlsSizes_.cbMaximumMessage > 0 ? (int)tlsSizes_.cbMaximumMessage : 16384;
    int off = 0;
    while (off < len)
    {
        int chunk = len - off;
        if (chunk > maxMsg) chunk = maxMsg;

        std::string buf;
        buf.resize(tlsSizes_.cbHeader + chunk + tlsSizes_.cbTrailer);
        memcpy(buf.data() + tlsSizes_.cbHeader, data + off, chunk);

        SecBuffer bufs[4] = {};
        bufs[0].BufferType = SECBUFFER_STREAM_HEADER;
        bufs[0].pvBuffer = buf.data();
        bufs[0].cbBuffer = tlsSizes_.cbHeader;
        bufs[1].BufferType = SECBUFFER_DATA;
        bufs[1].pvBuffer = buf.data() + tlsSizes_.cbHeader;
        bufs[1].cbBuffer = chunk;
        bufs[2].BufferType = SECBUFFER_STREAM_TRAILER;
        bufs[2].pvBuffer = buf.data() + tlsSizes_.cbHeader + chunk;
        bufs[2].cbBuffer = tlsSizes_.cbTrailer;
        bufs[3].BufferType = SECBUFFER_EMPTY;

        SecBufferDesc desc = { SECBUFFER_VERSION, 4, bufs };
        SECURITY_STATUS st = EncryptMessage(&hCtx_, 0, &desc, 0);
        if (st != SEC_E_OK) return false;

        int total = bufs[0].cbBuffer + bufs[1].cbBuffer + bufs[2].cbBuffer;
        int sent = 0;
        while (sent < total)
        {
            int r = send((SOCKET)sock_, buf.data() + sent, total - sent, 0);
            if (r == SOCKET_ERROR) return false;
            sent += r;
        }
        off += chunk;
    }
    return true;
}

// TLS 模式读取:尝试把已收到的密文解密进 tlsIn_;
// 若 tlsIn_ 有明文且 buffer 非空,则拷贝最多 maxLen 字节返回;
// 返回 >0 明文长度,0 连接关闭/超时,-1 错误
int WsClient::TlsRead(char* buffer, int maxLen)
{
    for (;;)
    {
        // 尝试解密已有密文
        while (!tlsRaw_.empty() && running_ && sock_ != (intptr_t)INVALID_SOCKET)
        {
            SecBuffer bufs[4] = {};
            bufs[0].BufferType = SECBUFFER_DATA;
            bufs[0].pvBuffer = tlsRaw_.data();
            bufs[0].cbBuffer = (ULONG)tlsRaw_.size();
            bufs[1].BufferType = SECBUFFER_EMPTY;
            bufs[2].BufferType = SECBUFFER_EMPTY;
            bufs[3].BufferType = SECBUFFER_EMPTY;
            SecBufferDesc desc = { SECBUFFER_VERSION, 4, bufs };

            SECURITY_STATUS st = DecryptMessage(&hCtx_, &desc, 0, nullptr);
            if (st == SEC_E_INCOMPLETE_MESSAGE)
            {
                break; // 需要更多密文,去 recv
            }
            if (st == SEC_I_CONTEXT_EXPIRED)
            {
                return 0; // 连接关闭
            }
            if (st == SEC_I_RENEGOTIATE)
            {
                // 服务器要求重协商:不支持,断开
                return -1;
            }
            if (st != SEC_E_OK)
            {
                PostStatus((std::wstring(L"TLS 解密失败: ") + SecStatusText(st)).c_str(), false);
                return -1;
            }

            // 收集解密出的明文与剩余密文
            // 注意:Schannel 可能返回 pvBuffer==NULL 而 cbBuffer>0,必须判空再使用
            std::string plain;
            std::string extra;
            for (int i = 0; i < 4; i++)
            {
                if (bufs[i].BufferType == SECBUFFER_DATA && bufs[i].cbBuffer > 0 && bufs[i].pvBuffer)
                {
                    plain.append((const char*)bufs[i].pvBuffer, bufs[i].cbBuffer);
                }
                else if (bufs[i].BufferType == SECBUFFER_EXTRA && bufs[i].cbBuffer > 0 && bufs[i].pvBuffer)
                {
                    extra.append((const char*)bufs[i].pvBuffer, bufs[i].cbBuffer);
                }
            }
            tlsIn_.append(plain);
            tlsRaw_ = extra; // 剩余密文下次继续解密
        }

        // 若已有明文,返回给调用者
        if (!tlsIn_.empty())
        {
            if (buffer && maxLen > 0)
            {
                int take = (int)tlsIn_.size();
                if (take > maxLen) take = maxLen;
                memcpy(buffer, tlsIn_.data(), take);
                tlsIn_.erase(0, take);
                return take;
            }
            return (int)tlsIn_.size(); // buffer 为空时仅推进解密
        }

        // 无明文,尝试 recv 更多密文(45s 无数据视为死链)
        if (!running_ || sock_ == (intptr_t)INVALID_SOCKET) return 0;
        if (!WaitReadable(45000)) return 0;
        char raw[16384];
        int r = recv((SOCKET)sock_, raw, sizeof(raw), 0);
        if (r <= 0) return 0;
        tlsRaw_.append(raw, r);
        // 回到循环顶部继续解密
    }
}

// Schannel TLS 客户端握手
bool WsClient::TlsHandshake(const std::wstring& host)
{
    SCHANNEL_CRED sc = {};
    sc.dwVersion = SCHANNEL_CRED_VERSION;
    // 自动验证服务器证书(系统信任库 + 主机名)
    sc.dwFlags = SCH_CRED_AUTO_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS;

    TimeStamp expiry = {};
    SECURITY_STATUS st = AcquireCredentialsHandleW(nullptr,
        const_cast<LPWSTR>(UNISP_NAME_W),
        SECPKG_CRED_OUTBOUND, nullptr, &sc, nullptr, nullptr, &hCred_, &expiry);
    if (st != SEC_E_OK)
    {
        PostStatus((std::wstring(L"TLS 凭据失败: ") + SecStatusText(st)).c_str(), false);
        return false;
    }

    // 目标名:host(用于 SNI 与证书主机名校验)
    std::wstring target = host;

    SecBufferDesc outDesc = {};
    SecBuffer outBuf = {};
    SecBufferDesc inDesc = {};
    SecBuffer inBuf = {};
    ULONG attrs = 0;
    bool firstCall = true;

    for (int round = 0; round < 16; round++)
    {
        outDesc.ulVersion = SECBUFFER_VERSION;
        outDesc.cBuffers = 1;
        outDesc.pBuffers = &outBuf;
        outBuf.BufferType = SECBUFFER_TOKEN;
        outBuf.pvBuffer = nullptr;
        outBuf.cbBuffer = 0;

        // 输入缓冲:槽 0 放服务器数据,槽 1 接收 EXTRA(ISC 未消费的剩余数据)
        SecBuffer inBufs[2] = {};
        inBufs[0].BufferType = SECBUFFER_TOKEN;
        inBufs[0].pvBuffer = tlsRaw_.empty() ? nullptr : tlsRaw_.data();
        inBufs[0].cbBuffer = (ULONG)tlsRaw_.size();
        inBufs[1].BufferType = SECBUFFER_EMPTY;
        inDesc.ulVersion = SECBUFFER_VERSION;
        inDesc.cBuffers = 2;
        inDesc.pBuffers = inBufs;

        if (firstCall)
        {
            st = InitializeSecurityContextW(&hCred_, nullptr, (SEC_WCHAR*)target.c_str(),
                ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY |
                ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM,
                0, 0, nullptr, 0, &hCtx_, &outDesc, &attrs, &expiry);
            firstCall = false;
        }
        else
        {
            st = InitializeSecurityContextW(&hCred_, &hCtx_, (SEC_WCHAR*)target.c_str(),
                ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY |
                ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM,
                0, 0, &inDesc, 0, &hCtx_, &outDesc, &attrs, &expiry);
        }

        if (st == SEC_E_INCOMPLETE_MESSAGE)
        {
            // 服务器数据未到齐,继续接收(ISC 未消费输入,tlsRaw_ 保留)
            if (!WaitReadable(30000))
            {
                PostStatus(L"TLS 握手时连接超时", false);
                TlsCleanup();
                return false;
            }
            char raw[16384];
            int r = recv((SOCKET)sock_, raw, sizeof(raw), 0);
            if (r <= 0)
            {
                PostStatus(L"TLS 握手时连接中断", false);
                TlsCleanup();
                return false;
            }
            tlsRaw_.append(raw, r);
            continue;
        }
        if (st == SEC_I_INCOMPLETE_CREDENTIALS)
        {
            PostStatus(L"服务器要求客户端证书", false, true); // 永久错误:停止自动重连
            TlsCleanup();
            return false;
        }
        if (st != SEC_E_OK && st != SEC_I_CONTINUE_NEEDED)
        {
            wchar_t buf[128];
            swprintf_s(buf, L"TLS 握手失败(0x%08X): %s", (unsigned)st, SecStatusText(st));
            PostStatus(buf, false, IsCertError(st)); // 证书类错误属永久问题
            TlsCleanup();
            return false;
        }

        // 发送我们生成的 token
        if (outBuf.cbBuffer > 0 && outBuf.pvBuffer)
        {
            if (!SendAll((const char*)outBuf.pvBuffer, outBuf.cbBuffer))
            {
                PostStatus(L"TLS 发送失败", false);
                if (outBuf.pvBuffer) FreeContextBuffer(outBuf.pvBuffer);
                TlsCleanup();
                return false;
            }
            if (outBuf.pvBuffer) FreeContextBuffer(outBuf.pvBuffer);
        }

        // 保留 ISC 未消费的 EXTRA:
        // 注意 Windows 10 Schannel 可能不填充 EXTRA 的 pvBuffer(NULL),
        // 但 cbBuffer 有效 —— 剩余数据即 tlsRaw_ 尾部未消费的字节
        size_t extraLen = 0;
        if (inDesc.cBuffers > 1 && inBufs[1].BufferType == SECBUFFER_EXTRA)
            extraLen = inBufs[1].cbBuffer;
        if (extraLen > 0 && extraLen <= tlsRaw_.size())
        {
            size_t off = tlsRaw_.size() - extraLen;
            tlsRaw_.erase(0, off); // 丢弃已消费的前缀,保留尾部 EXTRA
        }
        else
        {
            tlsRaw_.clear();
        }

        if (st == SEC_E_OK)
        {
            break; // 握手完成
        }

        // 接收服务器响应
        if (!WaitReadable(30000))
        {
            PostStatus(L"TLS 握手时连接超时", false);
            TlsCleanup();
            return false;
        }
        char raw[16384];
        int r = recv((SOCKET)sock_, raw, sizeof(raw), 0);
        if (r <= 0)
        {
            PostStatus(L"TLS 握手时连接中断", false);
            TlsCleanup();
            return false;
        }
        tlsRaw_.append(raw, r);
    }

    if (st != SEC_E_OK)
    {
        PostStatus(L"TLS 握手未完成", false);
        TlsCleanup();
        return false;
    }

    st = QueryContextAttributesW(&hCtx_, SECPKG_ATTR_STREAM_SIZES, &tlsSizes_);
    if (st != SEC_E_OK)
    {
        PostStatus(L"TLS 流参数查询失败", false);
        TlsCleanup();
        return false;
    }

    tls_ = true;
    return true;
}

void WsClient::Worker()
{
    // WSAStartup 进程内只需一次(静态初始化;进程退出时由系统清理,
    // 重连多次启停线程不会反复初始化和清理 Winsock)
    static bool wsaReady = [] {
        WSADATA wsa;
        return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    }();
    if (!wsaReady)
    {
        PostStatus(L"网络初始化失败", false, true); // 系统级失败,重连无法恢复
        running_ = false;
        return;
    }

    UrlParts up;
    if (!ParseUrl(url_, up))
    {
        PostStatus(L"服务器地址无效", false, true); // 配置错误:停止自动重连
        running_ = false;
        return;
    }

    char hostA[256] = {};
    WideCharToMultiByte(CP_UTF8, 0, up.host.c_str(), -1, hostA, 256, nullptr, nullptr);
    wchar_t portW[16] = {};
    swprintf_s(portW, L"%d", up.port);

    // 异步可取消的 DNS 解析:GetAddrInfoExW(OVERLAPPED + 事件 + 取消句柄)。
    // 调用立即返回 WSA_IO_PENDING,工作线程在事件上 200ms 轮询等待(running_ 可打断),
    // Stop() 通过 GetAddrInfoExCancel 取消(取消后事件会被置位),避免阻塞无法退出
    ADDRINFOEXW hints = {};
    hints.ai_family = AF_UNSPEC; // 同时支持 IPv4/IPv6
    hints.ai_socktype = SOCK_STREAM;
    ADDRINFOEXW* result = nullptr;
    HANDLE dnsEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    OVERLAPPED ov = {};
    ov.hEvent = dnsEvent;
    HANDLE hCancel = nullptr;

    int gai = GetAddrInfoExW(up.host.c_str(), portW, NS_ALL, nullptr, &hints,
                             &result, nullptr, &ov, nullptr, &hCancel);
    dnsCancel_ = hCancel;

    if (gai == WSA_IO_PENDING)
    {
        // 等待完成:200ms 轮询运行状态,可被 Stop 快速打断
        while (running_ && WaitForSingleObject(dnsEvent, 200) == WAIT_TIMEOUT) {}
        if (!running_)
        {
            // 被 Stop 中止:取消 DNS 并等待其完成,释放资源后退出
            if (HANDLE h = dnsCancel_.load()) GetAddrInfoExCancel(&h);
            WaitForSingleObject(dnsEvent, 5000);
            CloseHandle(dnsEvent);
            if (result) FreeAddrInfoExW(result);
            dnsCancel_ = nullptr;
            return;
        }
    }
    else if (gai != 0)
    {
        // 立即失败(参数错误等)
        CloseHandle(dnsEvent);
        if (running_)
            PostStatus(L"无法解析服务器地址", false, true); // 域名解析失败:永久错误
        running_ = false;
        return;
    }
    // gai == 0(立即成功)或事件已触发:result 已填充;解析失败时 result 为 NULL
    CloseHandle(dnsEvent);
    dnsCancel_ = nullptr;
    if (!result)
    {
        PostStatus(L"无法解析服务器地址", false, true); // 域名解析失败:永久错误
        running_ = false;
        return;
    }

    sock_ = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (sock_ == (intptr_t)INVALID_SOCKET)
    {
        PostStatus(L"创建套接字失败", false); // 资源性失败:瞬时,保留自动重连
        FreeAddrInfoExW(result);
        running_ = false;
        return;
    }

    // 小消息场景禁用 Nagle 算法,降低交互延迟
    int nodelay = 1;
    setsockopt((SOCKET)sock_, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof(nodelay));

    // 非阻塞 connect + select:最长 10 秒,每 200ms 检查运行状态(可被 Stop 快速打断)
    {
        u_long nb = 1;
        ioctlsocket((SOCKET)sock_, FIONBIO, &nb);

        int cr = connect((SOCKET)sock_, result->ai_addr, (int)result->ai_addrlen);
        int connectErr = 0;
        if (cr == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK)
        {
            bool writable = false;
            for (int waited = 0; waited < 10000 && running_; waited += 200)
            {
                fd_set wf;
                FD_ZERO(&wf);
                FD_SET((SOCKET)sock_, &wf);
                timeval tv = {};
                tv.tv_sec = 0;
                tv.tv_usec = 200000;
                if (select(0, nullptr, &wf, nullptr, &tv) > 0)
                {
                    writable = true;
                    break;
                }
            }
            if (!writable)
            {
                // select 超时:连接仍在进行,按超时处理,不能落穿到握手阶段
                connectErr = running_ ? WSAETIMEDOUT : 0;
                cr = SOCKET_ERROR;
            }
            else
            {
                // 可写后必须检查 SO_ERROR:区分"连接成功"与"被拒/网络不可达"
                socklen_t elen = sizeof(connectErr);
                getsockopt((SOCKET)sock_, SOL_SOCKET, SO_ERROR, (char*)&connectErr, &elen);
                cr = (connectErr == 0) ? 0 : SOCKET_ERROR;
            }
        }
        else if (cr == SOCKET_ERROR)
        {
            socklen_t elen = sizeof(connectErr);
            getsockopt((SOCKET)sock_, SOL_SOCKET, SO_ERROR, (char*)&connectErr, &elen);
        }

        u_long b = 0;
        ioctlsocket((SOCKET)sock_, FIONBIO, &b); // 恢复阻塞模式

        if (cr != 0)
        {
            const wchar_t* why = L"连接服务器失败";
            if (!running_) why = L"连接已停止";
            else if (connectErr == WSAETIMEDOUT) why = L"连接服务器超时";
            PostStatus(why, false); // 瞬时错误:保持自动重连
            FreeAddrInfoExW(result);
            closesocket((SOCKET)sock_);
            sock_ = (intptr_t)INVALID_SOCKET;
            running_ = false;
            return;
        }
    }
    FreeAddrInfoExW(result);

    // ---- TLS 握手(HTTPS)----
    if (up.secure)
    {
        if (!TlsHandshake(up.host))
        {
            closesocket((SOCKET)sock_);
            sock_ = (intptr_t)INVALID_SOCKET;
            running_ = false;
            return;
        }
    }

    // ---- WebSocket 握手 ----
    std::string wsPath = WideToUtf8(up.path);
    if (!wsPath.empty() && wsPath.back() == '/') wsPath.pop_back();
    wsPath += "/stream?token=" + UrlEncode(WideToUtf8(token_));

    // Host 头:IPv6 字面量需加方括号
    char portStr[16] = {};
    sprintf_s(portStr, "%d", up.port);
    std::string hostHeader = hostA;
    if (strchr(hostA, ':') != nullptr)
    {
        hostHeader = "[" + std::string(hostA) + "]";
    }
    hostHeader += ":";
    hostHeader += portStr;

    std::string wsKey = MakeRandomKey();
    if (wsKey.empty())
    {
        PostStatus(L"生成随机密钥失败", false, true); // 系统 RNG 不可用:永久错误
        closesocket((SOCKET)sock_);
        sock_ = (intptr_t)INVALID_SOCKET;
        if (up.secure) TlsCleanup();
        running_ = false;
        return;
    }
    std::string req =
        "GET " + wsPath + " HTTP/1.1\r\n"
        "Host: " + hostHeader + "\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: " + wsKey + "\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "User-Agent: GotifyInbox/1.0\r\n"
        "\r\n";

    if (!SendAll(req.data(), (int)req.size()))
    {
        PostStatus(L"发送握手请求失败", false);
        closesocket((SOCKET)sock_);
        sock_ = (intptr_t)INVALID_SOCKET;
        if (up.secure) TlsCleanup();
        running_ = false;
        return;
    }

    // ---- 读取响应头 ----
    std::string resp;
    char buf[1024];
    while (resp.find("\r\n\r\n") == std::string::npos)
    {
        if (!WaitReadable(30000))
        {
            PostStatus(L"服务器无响应", false);
            closesocket((SOCKET)sock_);
            sock_ = (intptr_t)INVALID_SOCKET;
            if (up.secure) TlsCleanup();
            running_ = false;
            return;
        }
        int n = 0;
        if (tls_)
        {
            n = TlsRead(buf, sizeof(buf));
        }
        else
        {
            n = recv((SOCKET)sock_, buf, sizeof(buf), 0);
        }
        if (n <= 0)
        {
            PostStatus(L"服务器无响应", false);
            closesocket((SOCKET)sock_);
            sock_ = (intptr_t)INVALID_SOCKET;
            if (up.secure) TlsCleanup();
            running_ = false;
            return;
        }
        resp.append(buf, n);
        if (resp.size() > 65536)
        {
            PostStatus(L"响应头异常", false);
            closesocket((SOCKET)sock_);
            sock_ = (intptr_t)INVALID_SOCKET;
            if (up.secure) TlsCleanup();
            running_ = false;
            return;
        }
    }

    if (resp.rfind("HTTP/1.1 101", 0) != 0 && resp.rfind("HTTP/1.0 101", 0) != 0)
    {
        std::string statusLine = resp.substr(0, resp.find("\r\n"));
        // HTTP 401/403 = 令牌无效或无权访问:认证失败,属永久错误,停止自动重连
        bool authFail = statusLine.find(" 401 ") != std::string::npos ||
                        statusLine.find(" 403 ") != std::string::npos;
        if (authFail)
        {
            PostStatus(L"认证失败:令牌无效或无权访问", false, true);
        }
        else
        {
            PostStatus((L"连接被拒绝: " + Utf8ToWide(statusLine)).c_str(), false);
        }
        closesocket((SOCKET)sock_);
        sock_ = (intptr_t)INVALID_SOCKET;
        if (up.secure) TlsCleanup();
        running_ = false;
        return;
    }

    // 校验 Sec-WebSocket-Accept(RFC 6455:base64(SHA1(key + GUID)))
    {
        std::string accept = GetHeaderValue(resp, "Sec-WebSocket-Accept");
        std::string expected;
        if (!Sha1Base64(wsKey + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11", expected) ||
            accept != expected)
        {
            PostStatus(L"WebSocket 握手校验失败", false, true); // 端点不是 RFC6455 服务器:永久错误
            closesocket((SOCKET)sock_);
            sock_ = (intptr_t)INVALID_SOCKET;
            if (up.secure) TlsCleanup();
            running_ = false;
            return;
        }
    }

    PostStatus(L"已连接", true);

    // ---- 帧接收循环(RFC 6455) ----
    // 空闲保活:45s 无任何数据 → 主动 ping;再 45s 仍无 → 判定死链断开
    std::string fragment;
    bool permanentClose = false; // 服务器以永久性关闭码断开(>=4000 或 1008,如认证失败)
    while (running_ && sock_ != (intptr_t)INVALID_SOCKET)
    {
        if (!WaitReadable(45000))
        {
            unsigned char ping[6] = { 0x89, 0x80, 0, 0, 0, 0 };
            unsigned k = (unsigned)GetTickCount64();
            ping[2] = (unsigned char)(k >> 24);
            ping[3] = (unsigned char)(k >> 16);
            ping[4] = (unsigned char)(k >> 8);
            ping[5] = (unsigned char)k;
            if (!SendAll((const char*)ping, 6)) break;
            if (!WaitReadable(45000)) break; // 90s 无数据 → 死链
        }

        unsigned char hdr[14];
        if (!RecvExact(hdr, 2)) break;

        bool fin = (hdr[0] & 0x80) != 0;
        unsigned opcode = hdr[0] & 0x0F;
        bool masked = (hdr[1] & 0x80) != 0;
        unsigned long long len = hdr[1] & 0x7F;

        if (len == 126)
        {
            if (!RecvExact(hdr + 2, 2)) break;
            len = ((unsigned)hdr[2] << 8) | hdr[3];
        }
        else if (len == 127)
        {
            if (!RecvExact(hdr + 2, 8)) break;
            len = 0;
            for (int i = 0; i < 8; i++) len = (len << 8) | hdr[2 + i];
        }

        unsigned char maskKey[4] = {};
        if (masked)
        {
            if (!RecvExact(maskKey, 4)) break;
        }

        if (len > 64ULL * 1024 * 1024) break; // 防御性上限

        std::string payload((size_t)len, '\0');
        if (len > 0 && !RecvExact((unsigned char*)payload.data(), (int)len)) break;
        if (masked)
        {
            for (size_t i = 0; i < payload.size(); i++) payload[i] ^= maskKey[i & 3];
        }

        if (opcode == 0x1 || opcode == 0x0) // 文本消息或续帧
        {
            // 防御:续帧拼接总量上限 4MB,超限丢弃并重置
            if (fragment.size() + payload.size() > MAX_FRAGMENT_BYTES)
            {
                fragment.clear();
                continue;
            }
            fragment.append(payload);
            if (fin)
            {
                if (hwnd_ && IsWindow(hwnd_))
                {
                    auto* msg = new std::string(std::move(fragment));
                    if (!PostMessageW(hwnd_, WM_APP_WS_MESSAGE, 0, (LPARAM)msg))
                    {
                        delete msg; // 窗口已销毁:释放,避免泄漏
                    }
                }
                fragment.clear();
            }
        }
        else if (opcode == 0x8) // close
        {
            // 回发 close 帧完成关闭握手(掩码 + 2 字节关闭码)
            unsigned char closeFrame[8];
            closeFrame[0] = 0x88;
            closeFrame[1] = 0x82; // masked,payload 长度 2
            unsigned k = (unsigned)GetTickCount64();
            closeFrame[2] = (unsigned char)(k >> 24);
            closeFrame[3] = (unsigned char)(k >> 16);
            closeFrame[4] = (unsigned char)(k >> 8);
            closeFrame[5] = (unsigned char)k;
            unsigned char code0 = (payload.size() >= 2) ? (unsigned char)payload[0] : 0;
            unsigned char code1 = (payload.size() >= 2) ? (unsigned char)payload[1] : 0;
            if (payload.size() >= 2)
            {
                unsigned code = ((unsigned)code0 << 8) | code1;
                if (code >= 4000 || code == 1008) permanentClose = true; // 应用层/策略性关闭
            }
            closeFrame[6] = code0 ^ closeFrame[2];
            closeFrame[7] = code1 ^ closeFrame[3];
            SendAll((const char*)closeFrame, sizeof(closeFrame));
            break;
        }
        else if (opcode == 0x9) // ping -> 回 pong
        {
            unsigned char pong[6] = { 0x8A, 0x80, 0, 0, 0, 0 }; // pong + mask(客户端帧必须掩码)
            unsigned k = (unsigned)GetTickCount64();
            pong[2] = (unsigned char)(k >> 24);
            pong[3] = (unsigned char)(k >> 16);
            pong[4] = (unsigned char)(k >> 8);
            pong[5] = (unsigned char)k;
            SendAll((const char*)pong, 6);
        }
        else if (opcode == 0x2) // 二进制消息:忽略
        {
            fragment.clear();
        }
        // opcode 0xA pong:忽略
    }

    // 主动 Stop 时(running_ 已被清除)不投递"断开"状态,避免触发无谓的自动重连
    if (running_)
    {
        PostStatus(permanentClose ? L"连接被服务器拒绝" : L"连接已断开",
                   false, permanentClose);
    }
    if (sock_ != (intptr_t)INVALID_SOCKET)
    {
        closesocket((SOCKET)sock_);
        sock_ = (intptr_t)INVALID_SOCKET;
    }
    if (up.secure) TlsCleanup();
    running_ = false;
}
