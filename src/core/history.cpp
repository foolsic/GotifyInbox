#include "core/history.h"
#include "core/config.h"
#include "core/json.h"
#include <windows.h>
#include <fstream>

namespace
{
    std::wstring HistoryPath()
    {
        return DataDir() + L"\\message.json";
    }
}

bool LoadHistory(std::vector<GotifyMessage>& out)
{
    std::ifstream f(HistoryPath(), std::ios::binary);
    if (!f)
    {
        return false;
    }

    std::string text;
    text.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    json::Value root;
    if (!json::Parse(text, root) || !root.IsArray())
    {
        return false;
    }

    for (const auto& v : root.AsArray())
    {
        GotifyMessage m;
        if (GotifyMessage::ParseFromJson(v, m))
        {
            out.push_back(m);
        }
    }
    return !out.empty();
}

void SaveHistory(const std::vector<GotifyMessage>& messages)
{
    json::Value::Array arr;
    arr.reserve(messages.size());
    for (const auto& m : messages)
    {
        json::Value::Object obj;
        obj.emplace("id", json::Value((double)m.id));
        obj.emplace("appid", json::Value((double)m.appid));
        obj.emplace("message", json::Value(m.message));
        obj.emplace("title", json::Value(m.title));
        obj.emplace("priority", json::Value((double)m.priority));
        obj.emplace("date", json::Value(m.date));
        obj.emplace("appname", json::Value(m.appname));
        arr.emplace_back(std::move(obj));
    }

    // 原子写入:先写临时文件再改名,崩溃/断电中断不会破坏原文件
    std::wstring tmp = HistoryPath() + L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return;
        std::string text = json::Serialize(json::Value(std::move(arr)));
        f.write(text.data(), (std::streamsize)text.size());
        f.close();
        if (!f) return;
    }
    if (!MoveFileExW(tmp.c_str(), HistoryPath().c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        DeleteFileW(tmp.c_str());
    }
}
