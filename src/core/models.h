#pragma once
#include <string>
#include "core/json.h"

// gotify 消息(文本字段为 UTF-8)
struct GotifyMessage
{
    int id = 0;
    int appid = 0;
    std::string message;
    std::string title;
    int priority = 0;
    std::string date;
    std::string appname;

    // 从 JSON 解析(兼容裸消息与 {event,message} 事件包装)
    static bool ParseFromJson(const json::Value& root, GotifyMessage& out)
    {
        const json::Value* msg = &root;

        // 事件包装形式: {"event":"message","message":{...}}
        if (const json::Value* inner = root.Get("message"); inner && inner->IsObject())
        {
            msg = inner;
        }

        if (!msg->IsObject() || !msg->Get("id"))
        {
            return false;
        }

        out.id = (int)msg->GetNumber("id");
        out.appid = (int)msg->GetNumber("appid");
        out.message = msg->GetString("message");
        out.title = msg->GetString("title");
        out.priority = (int)msg->GetNumber("priority");
        out.date = msg->GetString("date");
        out.appname = msg->GetString("appname");
        return true;
    }
};
