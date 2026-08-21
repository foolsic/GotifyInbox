#pragma once
#include <string>

// 应用配置(serverUrl/clientToken 持久化;行为固定项不落盘:
// 启动即最小化到托盘,关闭窗口隐藏到托盘,不播放提示音,断线自动重连)
struct Config
{
    std::wstring serverUrl;
    std::wstring clientToken;

    void Load();
    void Save() const;
};

extern Config g_config;

// 可执行文件所在目录
std::wstring ExeDir();

// 数据目录:优先 exe 所在目录;不可写(如安装到 Program Files)时回退 %APPDATA%\GotifyInbox
std::wstring DataDir();
