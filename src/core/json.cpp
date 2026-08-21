#include "core/json.h"
#include <cctype>
#include <cstdlib>
#include <cmath>

namespace json
{
    namespace
    {
        // 递归下降解析器
        class Parser
        {
        public:
            explicit Parser(const std::string& text) : p_(text.data()), end_(text.data() + text.size()) {}

            bool Run(Value& out, std::string* err)
            {
                SkipWs();
                if (!ParseValue(out))
                {
                    SetErr(err, "unexpected token");
                    return false;
                }
                SkipWs();
                if (p_ != end_)
                {
                    SetErr(err, "trailing data");
                    return false;
                }
                return true;
            }

        private:
            const char* p_;
            const char* end_;
            int depth_ = 0;
            static constexpr int MAX_DEPTH = 512;

            void SetErr(std::string* err, const char* msg)
            {
                if (err) *err = std::string(msg);
            }

            void SkipWs()
            {
                while (p_ != end_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\n' || *p_ == '\r'))
                    ++p_;
            }

            bool ParseValue(Value& out)
            {
                if (++depth_ > MAX_DEPTH)
                {
                    --depth_;
                    return false; // 嵌套过深,拒绝(防栈溢出)
                }
                bool ok = ParseValueInner(out);
                --depth_;
                return ok;
            }

            bool ParseValueInner(Value& out)
            {
                if (p_ == end_) return false;
                switch (*p_)
                {
                case '{': return ParseObject(out);
                case '[': return ParseArray(out);
                case '"': {
                    std::string s;
                    if (!ParseString(s)) return false;
                    out = std::move(s);
                    return true;
                }
                case 't':
                    if (Consume("true")) { out = true; return true; }
                    return false;
                case 'f':
                    if (Consume("false")) { out = false; return true; }
                    return false;
                case 'n':
                    if (Consume("null")) { out = nullptr; return true; }
                    return false;
                default:
                    return ParseNumber(out);
                }
            }

            bool Consume(const char* lit)
            {
                size_t n = strlen(lit);
                if (static_cast<size_t>(end_ - p_) < n) return false;
                if (memcmp(p_, lit, n) != 0) return false;
                p_ += n;
                return true;
            }

            bool ParseObject(Value& out)
            {
                ++p_; // '{'
                Value::Object obj;
                SkipWs();
                if (p_ != end_ && *p_ == '}') { ++p_; out = std::move(obj); return true; }
                while (true)
                {
                    SkipWs();
                    if (p_ == end_ || *p_ != '"') return false;
                    std::string key;
                    if (!ParseString(key)) return false;
                    SkipWs();
                    if (p_ == end_ || *p_ != ':') return false;
                    ++p_;
                    SkipWs();
                    Value val;
                    if (!ParseValue(val)) return false;
                    // 重复键:后者覆盖(与主流解析器一致)
                    obj[std::move(key)] = std::move(val);
                    SkipWs();
                    if (p_ == end_) return false;
                    if (*p_ == ',') { ++p_; continue; }
                    if (*p_ == '}') { ++p_; out = std::move(obj); return true; }
                    return false;
                }
            }

            bool ParseArray(Value& out)
            {
                ++p_; // '['
                Value::Array arr;
                SkipWs();
                if (p_ != end_ && *p_ == ']') { ++p_; out = std::move(arr); return true; }
                while (true)
                {
                    SkipWs();
                    Value val;
                    if (!ParseValue(val)) return false;
                    arr.push_back(std::move(val));
                    SkipWs();
                    if (p_ == end_) return false;
                    if (*p_ == ',') { ++p_; continue; }
                    if (*p_ == ']') { ++p_; out = std::move(arr); return true; }
                    return false;
                }
            }

            // 解析字符串字面量(含转义与 \uXXXX,输出 UTF-8)
            bool ParseString(std::string& out)
            {
                ++p_; // '"'
                out.clear();
                while (p_ != end_)
                {
                    unsigned char c = (unsigned char)*p_;
                    if (c == '"') { ++p_; return true; }
                    if (c == '\\')
                    {
                        ++p_;
                        if (p_ == end_) return false;
                        switch (*p_)
                        {
                        case '"': out += '"'; ++p_; break;
                        case '\\': out += '\\'; ++p_; break;
                        case '/': out += '/'; ++p_; break;
                        case 'b': out += '\b'; ++p_; break;
                        case 'f': out += '\f'; ++p_; break;
                        case 'n': out += '\n'; ++p_; break;
                        case 'r': out += '\r'; ++p_; break;
                        case 't': out += '\t'; ++p_; break;
                        case 'u': {
                            ++p_;
                            unsigned cp = ParseHex4();
                            if (cp == 0xFFFFFFFF) return false;
                            if (cp >= 0xDC00 && cp <= 0xDFFF) return false; // 孤立低代理,拒绝
                            // 代理对
                            if (cp >= 0xD800 && cp <= 0xDBFF)
                            {
                                if (p_ + 1 < end_ && p_[0] == '\\' && p_[1] == 'u')
                                {
                                    p_ += 2;
                                    unsigned lo = ParseHex4();
                                    if (lo >= 0xDC00 && lo <= 0xDFFF)
                                    {
                                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                                    }
                                    else
                                    {
                                        return false;
                                    }
                                }
                                else
                                {
                                    return false;
                                }
                            }
                            AppendUtf8(out, cp);
                            break;
                        }
                        default:
                            return false;
                        }
                    }
                    else if (c < 0x20)
                    {
                        return false; // 控制字符必须转义
                    }
                    else
                    {
                        out += (char)c;
                        ++p_;
                    }
                }
                return false;
            }

            // 读取 4 位十六进制,失败返回 0xFFFFFFFF
            unsigned ParseHex4()
            {
                if (end_ - p_ < 4) return 0xFFFFFFFF;
                unsigned v = 0;
                for (int i = 0; i < 4; ++i)
                {
                    char c = *p_++;
                    v <<= 4;
                    if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
                    else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
                    else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
                    else return 0xFFFFFFFF;
                }
                return v;
            }

            static void AppendUtf8(std::string& out, unsigned cp)
            {
                if (cp < 0x80)
                {
                    out += (char)cp;
                }
                else if (cp < 0x800)
                {
                    out += (char)(0xC0 | (cp >> 6));
                    out += (char)(0x80 | (cp & 0x3F));
                }
                else if (cp < 0x10000)
                {
                    out += (char)(0xE0 | (cp >> 12));
                    out += (char)(0x80 | ((cp >> 6) & 0x3F));
                    out += (char)(0x80 | (cp & 0x3F));
                }
                else
                {
                    out += (char)(0xF0 | (cp >> 18));
                    out += (char)(0x80 | ((cp >> 12) & 0x3F));
                    out += (char)(0x80 | ((cp >> 6) & 0x3F));
                    out += (char)(0x80 | (cp & 0x3F));
                }
            }

            bool ParseNumber(Value& out)
            {
                const char* start = p_;
                if (p_ != end_ && *p_ == '-') ++p_;
                // 整数部分:必须有数字,不允许前导零(JSON 规范)
                if (p_ == end_ || !isdigit((unsigned char)*p_)) return false;
                if (*p_ == '0')
                {
                    ++p_;
                    if (p_ != end_ && isdigit((unsigned char)*p_)) return false;
                }
                else
                {
                    while (p_ != end_ && isdigit((unsigned char)*p_)) ++p_;
                }
                // 小数部分:小数点后必须有数字
                if (p_ != end_ && *p_ == '.')
                {
                    ++p_;
                    if (p_ == end_ || !isdigit((unsigned char)*p_)) return false;
                    while (p_ != end_ && isdigit((unsigned char)*p_)) ++p_;
                }
                // 指数部分:必须有数字
                if (p_ != end_ && (*p_ == 'e' || *p_ == 'E'))
                {
                    ++p_;
                    if (p_ != end_ && (*p_ == '+' || *p_ == '-')) ++p_;
                    if (p_ == end_ || !isdigit((unsigned char)*p_)) return false;
                    while (p_ != end_ && isdigit((unsigned char)*p_)) ++p_;
                }
                // 自实现数字解析,避免 strtod 依赖 C locale(小数逗号区域会解析失败)
                const char* q = start;
                bool neg = false;
                if (*q == '-') { neg = true; ++q; }
                double intPart = 0;
                while (q != p_ && *q != '.' && *q != 'e' && *q != 'E')
                {
                    intPart = intPart * 10 + (*q - '0');
                    ++q;
                }
                double frac = 0;
                double scale = 1;
                if (q != p_ && *q == '.')
                {
                    ++q;
                    while (q != p_ && *q != 'e' && *q != 'E')
                    {
                        frac = frac * 10 + (*q - '0');
                        scale *= 10;
                        ++q;
                    }
                }
                int exp = 0;
                bool expNeg = false;
                if (q != p_ && (*q == 'e' || *q == 'E'))
                {
                    ++q;
                    if (q != p_ && (*q == '+' || *q == '-')) { expNeg = (*q == '-'); ++q; }
                    while (q != p_)
                    {
                        if (exp < 100000) exp = exp * 10 + (*q - '0'); // 防溢出
                        ++q;
                    }
                }
                double value = (intPart + frac / scale) * pow(10.0, expNeg ? -(double)exp : (double)exp);
                out = neg ? -value : value;
                return true;
            }
        };

        void SerializeString(std::string& out, const std::string& s)
        {
            out += '"';
            for (unsigned char c : s)
            {
                switch (c)
                {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (c < 0x20)
                    {
                        char buf[8];
                        snprintf(buf, sizeof(buf), "\\u%04x", c);
                        out += buf;
                    }
                    else
                    {
                        out += (char)c;
                    }
                }
            }
            out += '"';
        }

        void SerializeValue(std::string& out, const Value& v)
        {
            if (v.IsNull())
            {
                out += "null";
            }
            else if (v.IsBool())
            {
                out += std::get<bool>(v.data) ? "true" : "false";
            }
            else if (v.IsNumber())
            {
                double d = std::get<double>(v.data);
                if (std::floor(d) == d && std::abs(d) < 1e15)
                {
                    char buf[32];
                    snprintf(buf, sizeof(buf), "%lld", (long long)d);
                    out += buf;
                }
                else
                {
                    char buf[32];
                    snprintf(buf, sizeof(buf), "%.17g", d);
                    out += buf;
                }
            }
            else if (v.IsString())
            {
                SerializeString(out, std::get<std::string>(v.data));
            }
            else if (v.IsArray())
            {
                out += '[';
                bool first = true;
                for (const auto& item : std::get<Value::Array>(v.data))
                {
                    if (!first) out += ',';
                    first = false;
                    SerializeValue(out, item);
                }
                out += ']';
            }
            else // object
            {
                out += '{';
                bool first = true;
                for (const auto& [key, val] : std::get<Value::Object>(v.data))
                {
                    if (!first) out += ',';
                    first = false;
                    SerializeString(out, key);
                    out += ':';
                    SerializeValue(out, val);
                }
                out += '}';
            }
        }
    }

    bool Parse(const std::string& text, Value& out, std::string* err)
    {
        Parser parser(text);
        return parser.Run(out, err);
    }

    std::string Serialize(const Value& v)
    {
        std::string out;
        SerializeValue(out, v);
        return out;
    }
}
