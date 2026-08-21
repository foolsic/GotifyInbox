#pragma once
#include <string>

// UTF-8 <-> UTF-16 转换(界面与文件均为 UTF-8/16 互转)
std::wstring Utf8ToWide(const std::string& s);
std::string WideToUtf8(const std::wstring& s);
