#include "core/config.h"
#include "core/json.h"
#include "util/utf8.h"
#include <windows.h>
#include <shlobj.h>
#include <fstream>

Config g_config;

std::wstring ExeDir()
{
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring path = buf;
    auto slash = path.find_last_of(L'\\');
    if (slash != std::wstring::npos)
    {
        path = path.substr(0, slash);
    }
    return path;
}

std::wstring DataDir()
{
    static std::wstring cached;
    static bool init = false;
    if (!init)
    {
        std::wstring exe = ExeDir();
        // 探测 exe 目录可写性:尝试创建临时文件
        std::wstring probe = exe + L"\\__write_probe.tmp";
        HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr);
        if (h != INVALID_HANDLE_VALUE)
        {
            CloseHandle(h);
            DeleteFileW(probe.c_str());
            cached = exe;
        }
        else
        {
            wchar_t appData[MAX_PATH] = {};
            if (SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, appData) == S_OK)
            {
                cached = std::wstring(appData) + L"\\GotifyInbox";
                CreateDirectoryW(cached.c_str(), nullptr);
            }
            else
            {
                cached = exe; // 兜底仍用 exe 目录
            }
        }
        init = true;
    }
    return cached;
}

namespace
{
    std::wstring ConfigPath()
    {
        return DataDir() + L"\\config.json";
    }

    // 读取 UTF-8 文本文件
    bool ReadTextFile(const std::wstring& path, std::string& out)
    {
        std::ifstream f(path, std::ios::binary);
        if (!f) return false;
        out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        return true;
    }

    // 原子写入:先写临时文件再改名,崩溃/断电中断不会破坏原文件
    bool WriteTextFile(const std::wstring& path, const std::string& text)
    {
        std::wstring tmp = path + L".tmp";
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            if (!f) return false;
            f.write(text.data(), (std::streamsize)text.size());
            f.close();
            if (!f) return false; // 检查 flush 状态
        }
        if (!MoveFileExW(tmp.c_str(), path.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            DeleteFileW(tmp.c_str());
            return false;
        }
        return true;
    }
}

void Config::Load()
{
    std::string text;
    if (!ReadTextFile(ConfigPath(), text))
    {
        return; // 无配置文件,使用默认值
    }

    json::Value root;
    if (!json::Parse(text, root) || !root.IsObject())
    {
        return;
    }

    serverUrl = Utf8ToWide(root.GetString("ServerUrl"));
    clientToken = Utf8ToWide(root.GetString("ClientToken"));
}

void Config::Save() const
{
    json::Value::Object obj;
    obj.emplace("ServerUrl", std::string(WideToUtf8(serverUrl)));
    obj.emplace("ClientToken", std::string(WideToUtf8(clientToken)));

    WriteTextFile(ConfigPath(), json::Serialize(json::Value(std::move(obj))));
}
