#pragma once

// Minimal JSON reader: enough for exchange REST responses, not a general library.
// Depth is capped, strings are decoded to UTF-8, numbers are doubles.

#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hft::net {

struct Json {
    enum class Type { Null, Bool, Num, Str, Arr, Obj };
    Type type = Type::Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Json> a;
    std::vector<std::pair<std::string, Json>> o;

    const Json* find(std::string_view key) const {
        for (const auto& kv : o)
            if (kv.first == key) return &kv.second;
        return nullptr;
    }
    // Empty / zero when the key is missing or has another type.
    std::string str(std::string_view key) const {
        const Json* j = find(key);
        return j && j->type == Type::Str ? j->s : std::string();
    }
    double num(std::string_view key) const {
        const Json* j = find(key);
        return j && j->type == Type::Num ? j->n : 0.0;
    }
    bool flag(std::string_view key) const {
        const Json* j = find(key);
        return j && j->type == Type::Bool && j->b;
    }
};

namespace detail {

class JsonReader {
   public:
    explicit JsonReader(std::string_view t) : t_(t) {}

    Json parse() {
        Json v = value(0);
        skip();
        if (i_ != t_.size()) fail("trailing characters");
        return v;
    }

   private:
    [[noreturn]] void fail(const char* what) const {
        throw std::runtime_error(std::string("json: ") + what + " at byte " + std::to_string(i_));
    }
    void skip() {
        while (i_ < t_.size() && (t_[i_] == ' ' || t_[i_] == '\t' || t_[i_] == '\n' || t_[i_] == '\r')) ++i_;
    }
    char peek() {
        skip();
        if (i_ >= t_.size()) fail("unexpected end");
        return t_[i_];
    }
    void expect(char c) {
        if (peek() != c) fail("unexpected character");
        ++i_;
    }
    bool literal(std::string_view w) {
        if (t_.compare(i_, w.size(), w) != 0) return false;
        i_ += w.size();
        return true;
    }

    Json value(int depth) {
        if (depth > 64) fail("nesting too deep");
        Json v;
        switch (peek()) {
            case '{': {
                ++i_;
                v.type = Json::Type::Obj;
                if (peek() == '}') return ++i_, v;
                for (;;) {
                    if (peek() != '"') fail("object key expected");
                    std::string k = string();
                    expect(':');
                    v.o.emplace_back(std::move(k), value(depth + 1));
                    if (peek() == ',') {
                        ++i_;
                        continue;
                    }
                    expect('}');
                    return v;
                }
            }
            case '[': {
                ++i_;
                v.type = Json::Type::Arr;
                if (peek() == ']') return ++i_, v;
                for (;;) {
                    v.a.push_back(value(depth + 1));
                    if (peek() == ',') {
                        ++i_;
                        continue;
                    }
                    expect(']');
                    return v;
                }
            }
            case '"':
                v.type = Json::Type::Str;
                v.s = string();
                return v;
            case 't':
                if (!literal("true")) fail("bad literal");
                v.type = Json::Type::Bool, v.b = true;
                return v;
            case 'f':
                if (!literal("false")) fail("bad literal");
                v.type = Json::Type::Bool;
                return v;
            case 'n':
                if (!literal("null")) fail("bad literal");
                return v;
            default: {
                const std::size_t start = i_;
                while (i_ < t_.size() && (t_[i_] == '-' || t_[i_] == '+' || t_[i_] == '.' || t_[i_] == 'e' ||
                                          t_[i_] == 'E' || (t_[i_] >= '0' && t_[i_] <= '9')))
                    ++i_;
                if (i_ == start) fail("unexpected character");
                const std::string num(t_.substr(start, i_ - start));
                char* end = nullptr;
                v.n = std::strtod(num.c_str(), &end);
                if (end != num.c_str() + num.size()) fail("bad number");
                v.type = Json::Type::Num;
                return v;
            }
        }
    }

    static void utf8(std::string& out, std::uint32_t c) {
        if (c < 0x80) {
            out += static_cast<char>(c);
        } else if (c < 0x800) {
            out += static_cast<char>(0xC0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += static_cast<char>(0xE0 | (c >> 12));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (c >> 18));
            out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    std::uint32_t hex4() {
        if (i_ + 4 > t_.size()) fail("bad \\u escape");
        std::uint32_t v = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = t_[i_++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<std::uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<std::uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<std::uint32_t>(c - 'A' + 10);
            else fail("bad \\u escape");
        }
        return v;
    }

    std::string string() {
        ++i_;  // opening quote
        std::string out;
        for (;;) {
            if (i_ >= t_.size()) fail("unterminated string");
            const char c = t_[i_++];
            if (c == '"') return out;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i_ >= t_.size()) fail("unterminated string");
            switch (const char e = t_[i_++]) {
                case '"': case '\\': case '/': out += e; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    std::uint32_t cp = hex4();
                    if (cp >= 0xD800 && cp < 0xDC00) {
                        if (t_.compare(i_, 2, "\\u") != 0) fail("lone surrogate");
                        i_ += 2;
                        const std::uint32_t lo = hex4();
                        if (lo < 0xDC00 || lo > 0xDFFF) fail("bad surrogate pair");
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    utf8(out, cp);
                    break;
                }
                default: fail("bad escape");
            }
        }
    }

    std::string_view t_;
    std::size_t i_ = 0;
};

}  // namespace detail

inline Json parse_json(std::string_view text) { return detail::JsonReader(text).parse(); }

}  // namespace hft::net
