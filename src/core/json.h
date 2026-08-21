#pragma once
#include <string>
#include <vector>
#include <map>
#include <variant>

// 最小可用 JSON 解析/序列化(UTF-8 输入输出,内部 std::string 存 UTF-8)
namespace json
{
    struct Value
    {
        using Object = std::map<std::string, Value>;
        using Array = std::vector<Value>;

        std::variant<std::nullptr_t, bool, double, std::string, Array, Object> data;

        Value() : data(nullptr) {}
        Value(std::nullptr_t) : data(nullptr) {}
        Value(bool b) : data(b) {}
        Value(double d) : data(d) {}
        Value(const char* s) : data(std::string(s)) {}
        Value(const std::string& s) : data(s) {}
        Value(Array a) : data(std::move(a)) {}
        Value(Object o) : data(std::move(o)) {}

        bool IsNull() const { return std::holds_alternative<std::nullptr_t>(data); }
        bool IsBool() const { return std::holds_alternative<bool>(data); }
        bool IsNumber() const { return std::holds_alternative<double>(data); }
        bool IsString() const { return std::holds_alternative<std::string>(data); }
        bool IsArray() const { return std::holds_alternative<Array>(data); }
        bool IsObject() const { return std::holds_alternative<Object>(data); }

        bool AsBool() const { return IsBool() && std::get<bool>(data); }
        double AsNumber() const { return IsNumber() ? std::get<double>(data) : 0; }
        const std::string& AsString() const
        {
            static const std::string empty;
            return IsString() ? std::get<std::string>(data) : empty;
        }
        const Array& AsArray() const
        {
            static const Array empty;
            return IsArray() ? std::get<Array>(data) : empty;
        }
        const Object& AsObject() const
        {
            static const Object empty;
            return IsObject() ? std::get<Object>(data) : empty;
        }

        // 对象成员访问(非对象或键不存在返回 nullptr)
        const Value* Get(const std::string& key) const
        {
            if (!IsObject()) return nullptr;
            auto& obj = std::get<Object>(data);
            auto it = obj.find(key);
            return it == obj.end() ? nullptr : &it->second;
        }

        std::string GetString(const std::string& key, const std::string& def = {}) const
        {
            const Value* v = Get(key);
            return (v && v->IsString()) ? v->AsString() : def;
        }
        double GetNumber(const std::string& key, double def = 0) const
        {
            const Value* v = Get(key);
            return (v && v->IsNumber()) ? v->AsNumber() : def;
        }
        bool GetBool(const std::string& key, bool def = false) const
        {
            const Value* v = Get(key);
            return (v && v->IsBool()) ? v->AsBool() : def;
        }
    };

    // 解析 UTF-8 文本,成功返回 true
    bool Parse(const std::string& text, Value& out, std::string* err = nullptr);

    // 序列化为 UTF-8(紧凑格式)
    std::string Serialize(const Value& v);
}
