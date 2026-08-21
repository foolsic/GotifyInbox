#pragma once
#include <vector>
#include "core/models.h"

// 历史消息持久化(exe 同目录 message.json)
// 格式与旧版 .NET 客户端兼容:[{id,appid,message,title,priority,date,appname},...]

// 读取历史消息(无文件/解析失败返回 false)
bool LoadHistory(std::vector<GotifyMessage>& out);

// 全量写历史消息(最新在前)
void SaveHistory(const std::vector<GotifyMessage>& messages);
