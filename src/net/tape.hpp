#pragma once

// JSON parsed into a flat tape of nodes with no allocation once its buffers have grown.
// Stage 1 finds structural characters 64 bytes at a time (AVX2 compares, escaped quotes
// by the odd-backslash-run rule, string interiors by a carry-less multiply prefix xor);
// stage 2 walks only those positions. Strings stay as views into the input with escapes
// unresolved; `decode` resolves them when needed.

#include <immintrin.h>

#include <charconv>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>

namespace hft::net {

class JsonTape {
   public:
    enum class Type : std::uint8_t { Null, Bool, Num, Str, Arr, Obj };
    static constexpr std::uint32_t npos = ~0u;

    // False on malformed input; the tape is then unspecified.
    bool parse(std::string_view text) {
        t_ = text;
        nodes_.clear();
        if (text.size() >= npos - 64) return false;
        pos_.reserve(text.size() + 64);  // at most one token per byte, plus a block of slack
        if (!(simd_ ? stage1_avx2() : stage1_scalar())) return false;
        nodes_.reserve(pos_.size());  // every node starts at its own token
        k_ = 0;
        return value(0) && k_ == pos_.size();
    }

    std::uint32_t root() const { return 0; }
    Type type(std::uint32_t i) const { return nodes_[i].type; }
    // Raw text: string contents without quotes, number or literal text.
    std::string_view raw(std::uint32_t i) const { return t_.substr(nodes_[i].pos, nodes_[i].len); }
    bool boolean(std::uint32_t i) const { return nodes_[i].type == Type::Bool && t_[nodes_[i].pos] == 't'; }
    double num(std::uint32_t i) const {
        double v = 0;
        const std::string_view r = raw(i);
        if (std::from_chars(r.data() + (r[0] == '+'), r.data() + r.size(), v).ec != std::errc())
            v = std::strtod(std::string(r).c_str(), nullptr);  // out of range: same infinity or zero as strtod
        return v;
    }

    // Children of an array, or alternating key and value of an object.
    std::uint32_t first(std::uint32_t i) const { return i + 1; }
    std::uint32_t end(std::uint32_t i) const { return nodes_[i].end; }
    std::uint32_t next(std::uint32_t i) const { return nodes_[i].end; }
    std::uint32_t size(std::uint32_t i) const {
        std::uint32_t n = 0;
        for (std::uint32_t c = first(i); c != end(i); c = next(c)) ++n;
        return nodes_[i].type == Type::Obj ? n / 2 : n;
    }

    // Value of member `key` of object `i`, npos when absent or `i` is not an object.
    std::uint32_t find(std::uint32_t i, std::string_view key) const {
        if (nodes_[i].type != Type::Obj) return npos;
        for (std::uint32_t c = first(i); c != end(i); c = next(next(c)))
            if (key_is(c, key)) return c + 1;
        return npos;
    }
    // Raw string member, empty when absent or not a string.
    std::string_view str(std::uint32_t obj, std::string_view key) const {
        const std::uint32_t v = find(obj, key);
        return v != npos && nodes_[v].type == Type::Str ? raw(v) : std::string_view();
    }

    // String node with escapes resolved.
    std::string decode(std::uint32_t i) const { return unescape(raw(i)); }

    static std::string unescape(std::string_view s) {
        std::string out;
        for (std::size_t i = 0; i < s.size();) {
            const char c = s[i++];
            if (c != '\\') {
                out += c;
                continue;
            }
            switch (const char e = s[i++]) {
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    std::uint32_t cp = hex4(s.substr(i, 4));
                    i += 4;
                    if (cp >= 0xD800 && cp < 0xDC00) cp = 0x10000 + ((cp - 0xD800) << 10) + (hex4(s.substr(i + 2, 4)) - 0xDC00), i += 6;
                    utf8(out, cp);
                    break;
                }
                default: out += e;
            }
        }
        return out;
    }

    void use_simd(bool on) { simd_ = on && __builtin_cpu_supports("avx2") && __builtin_cpu_supports("pclmul"); }

   private:
    // Growable array whose push does not check capacity; callers reserve first.
    template <class T>
    class Buf {
       public:
        void reserve(std::size_t n) {
            if (n <= cap_) return;
            auto d = std::make_unique_for_overwrite<T[]>(n);
            if (n_) std::memcpy(d.get(), d_.get(), n_ * sizeof(T));
            d_ = std::move(d), cap_ = n;
        }
        void clear() { n_ = 0; }
        void push_back(const T& v) { d_[n_++] = v; }
        std::size_t size() const { return n_; }
        T& operator[](std::size_t i) { return d_[i]; }
        const T& operator[](std::size_t i) const { return d_[i]; }

       private:
        std::unique_ptr<T[]> d_;
        std::size_t n_ = 0, cap_ = 0;
    };

    struct Node {
        Type type;
        std::uint32_t pos, len, end;  // end: index past this node and its children
    };

    bool key_is(std::uint32_t c, std::string_view key) const {
        const std::string_view r = raw(c);
        if (r.size() == key.size()) return std::memcmp(r.data(), key.data(), r.size()) == 0;
        // An escaped key is longer than its decoded form.
        return any_escape_ && r.size() > key.size() && r.find('\\') != std::string_view::npos && unescape(r) == key;
    }

    static std::uint32_t hex4(std::string_view h) {
        std::uint32_t v = 0;
        for (const char c : h) v = v << 4 | static_cast<std::uint32_t>(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
        return v;
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

    static bool is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
    static bool is_op(char c) { return c == '{' || c == '}' || c == '[' || c == ']' || c == ':' || c == ','; }

    // Bits of `backslash` runs' escaped successors (simdjson's find_escaped).
    static std::uint64_t escaped(std::uint64_t backslash, std::uint64_t& carry) {
        backslash &= ~carry;
        const std::uint64_t follows = backslash << 1 | carry;
        constexpr std::uint64_t even = 0x5555555555555555ull;
        const std::uint64_t odd_starts = backslash & ~even & ~follows;
        std::uint64_t even_seq;
        carry = __builtin_add_overflow(odd_starts, backslash, &even_seq);
        return (even ^ (even_seq << 1)) & follows;
    }

    // Records one 64-byte block's token positions: operators and quotes outside strings,
    // plus the first byte of every run of other non-blank bytes (a number, a literal or
    // garbage), so stage 2 never has to look at the blanks in between.
    [[gnu::always_inline]] void emit(std::uint64_t quote, std::uint64_t bs, std::uint64_t op, std::uint64_t ws,
                                     std::uint64_t (*prefix)(std::uint64_t), std::size_t base) {
        const std::uint64_t valid = t_.size() - base >= 64 ? ~0ull : (1ull << (t_.size() - base)) - 1;
        any_escape_ |= bs;
        quote &= ~escaped(bs, esc_carry_);
        const std::uint64_t in_str = prefix(quote) ^ str_carry_;
        str_carry_ = static_cast<std::uint64_t>(static_cast<std::int64_t>(in_str) >> 63);
        const std::uint64_t scal = ~(op | ws | quote | in_str) & valid;
        const std::uint64_t starts = scal & ~(scal << 1 | run_carry_);
        run_carry_ = scal >> 63;
        std::uint64_t s = (op & ~in_str) | quote | starts;
        while (s) {
            pos_.push_back(static_cast<std::uint32_t>(base + static_cast<std::size_t>(__builtin_ctzll(s))));
            s &= s - 1;
        }
    }

    void reset_stage1() {
        pos_.clear();
        esc_carry_ = str_carry_ = run_carry_ = any_escape_ = 0;
    }

    // The last partial block, zero-padded; bytes past the end are masked off in emit.
    const char* block_at(std::size_t base, char* pad) const {
        if (t_.size() - base >= 64) return t_.data() + base;
        std::memset(pad, 0, 64);
        std::memcpy(pad, t_.data() + base, t_.size() - base);
        return pad;
    }

    static std::uint64_t prefix_xor_scalar(std::uint64_t x) {
        x ^= x << 1, x ^= x << 2, x ^= x << 4, x ^= x << 8, x ^= x << 16, x ^= x << 32;
        return x;
    }

    bool stage1_scalar() {
        reset_stage1();
        char pad[64];
        for (std::size_t base = 0; base < t_.size(); base += 64) {
            const char* p = block_at(base, pad);
            std::uint64_t q = 0, b = 0, o = 0, w = 0;
            for (int i = 0; i < 64; ++i) {
                const char c = p[i];
                const std::uint64_t bit = 1ull << i;
                if (c == '"') q |= bit;
                else if (c == '\\') b |= bit;
                else if (is_op(c)) o |= bit;
                else if (is_ws(c)) w |= bit;
            }
            emit(q, b, o, w, prefix_xor_scalar, base);
        }
        return str_carry_ == 0;
    }

    __attribute__((target("pclmul,sse2"))) static std::uint64_t prefix_clmul(std::uint64_t q) {
        const __m128i v = _mm_clmulepi64_si128(_mm_set_epi64x(0, static_cast<long long>(q)), _mm_set1_epi8(-1), 0);
        return static_cast<std::uint64_t>(_mm_cvtsi128_si64(v));
    }

    __attribute__((target("avx2"))) static std::uint32_t eq32(__m256i x, char c) {
        return static_cast<std::uint32_t>(_mm256_movemask_epi8(_mm256_cmpeq_epi8(x, _mm256_set1_epi8(c))));
    }

    // Classes by nibble lookup: the low nibble picks a candidate byte, one compare confirms
    // it. Unused entries hold a byte whose low nibble differs from their index.
    __attribute__((target("avx2"))) static std::uint32_t lookup32(__m256i x, __m256i tbl) {
        const __m256i nib = _mm256_and_si256(x, _mm256_set1_epi8(0x0F));
        return static_cast<std::uint32_t>(_mm256_movemask_epi8(_mm256_cmpeq_epi8(_mm256_shuffle_epi8(tbl, nib), x)));
    }

    __attribute__((target("avx2,pclmul"))) bool stage1_avx2() {
        reset_stage1();
        // , = 0x2C  : = 0x3A  { = 0x7B  } = 0x7D;  [ = 0x5B  ] = 0x5D;  blanks 0x20 0x09 0x0A 0x0D
        const __m256i ops1 = _mm256_setr_epi8(1, 0, 0, 0, 0, 0, 0, 0, 0, 0, ':', '{', ',', '}', 0, 0,
                                              1, 0, 0, 0, 0, 0, 0, 0, 0, 0, ':', '{', ',', '}', 0, 0);
        const __m256i ops2 = _mm256_setr_epi8(1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, '[', 0, ']', 0, 0,
                                              1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, '[', 0, ']', 0, 0);
        const __m256i blank = _mm256_setr_epi8(' ', 0, 0, 0, 0, 0, 0, 0, 0, '\t', '\n', 0, 0, '\r', 0, 0,
                                               ' ', 0, 0, 0, 0, 0, 0, 0, 0, '\t', '\n', 0, 0, '\r', 0, 0);
        alignas(32) char pad[64];
        for (std::size_t base = 0; base < t_.size(); base += 64) {
            const char* p = block_at(base, pad);
            const __m256i lo = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p));
            const __m256i hi = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p + 32));
            auto join = [](std::uint32_t a, std::uint32_t b) { return a | static_cast<std::uint64_t>(b) << 32; };
            const std::uint64_t o = join(lookup32(lo, ops1) | lookup32(lo, ops2), lookup32(hi, ops1) | lookup32(hi, ops2));
            const std::uint64_t w = join(lookup32(lo, blank), lookup32(hi, blank));
            emit(join(eq32(lo, '"'), eq32(hi, '"')), join(eq32(lo, '\\'), eq32(hi, '\\')), o, w, prefix_clmul, base);
        }
        return str_carry_ == 0;
    }

    static bool is_hex(char c) { return (c >= '0' && c <= '9') || ((c | 0x20) >= 'a' && (c | 0x20) <= 'f'); }
    static bool hex_at(std::string_view s, std::size_t i) {
        return i + 4 <= s.size() && is_hex(s[i]) && is_hex(s[i + 1]) && is_hex(s[i + 2]) && is_hex(s[i + 3]);
    }

    // Escapes as JSON allows them; a high surrogate must be followed by a low one.
    static bool escapes_ok(std::string_view s) {
        for (std::size_t i = 0; i < s.size(); ++i) {
            if (s[i] != '\\') continue;
            if (++i >= s.size()) return false;
            const char e = s[i];
            if (e == 'u') {
                if (!hex_at(s, i + 1)) return false;
                const std::uint32_t cp = hex4(s.substr(i + 1, 4));
                i += 4;
                if (cp >= 0xD800 && cp < 0xDC00) {
                    if (s.compare(i + 1, 2, "\\u") != 0 || !hex_at(s, i + 3)) return false;
                    const std::uint32_t lo = hex4(s.substr(i + 3, 4));
                    if (lo < 0xDC00 || lo > 0xDFFF) return false;
                    i += 6;
                }
            } else if (std::string_view("\"\\/bfnrt").find(e) == std::string_view::npos) {
                return false;
            }
        }
        return true;
    }

    // Token at pos_[k_] is the first byte of a run that is not an operator or a string.
    bool scalar() {
        const std::size_t a = pos_[k_++];
        std::size_t b = a + 1;
        while (b < t_.size() && !is_ws(t_[b]) && !is_op(t_[b]) && t_[b] != '"') ++b;
        const std::string_view s = t_.substr(a, b - a);
        Type ty;
        if (s == "true" || s == "false") {
            ty = Type::Bool;
        } else if (s == "null") {
            ty = Type::Null;
        } else {
            for (const char c : s)
                if (!(c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E' || (c >= '0' && c <= '9'))) return false;
            double v;
            const char* first = s.data() + (s[0] == '+');
            if (first != s.data() && first < s.data() + s.size() && *first == '-') return false;
            const auto r = std::from_chars(first, s.data() + s.size(), v);
            if (r.ec == std::errc::invalid_argument || r.ptr != s.data() + s.size()) return false;
            ty = Type::Num;
        }
        nodes_.push_back({ty, static_cast<std::uint32_t>(a), static_cast<std::uint32_t>(b - a),
                          static_cast<std::uint32_t>(nodes_.size() + 1)});
        return true;
    }

    char next_tok() const { return k_ < pos_.size() ? t_[pos_[k_]] : 0; }

    bool value(int depth) {
        if (depth > 64 || k_ >= pos_.size()) return false;
        const std::uint32_t p = pos_[k_];
        const char c = t_[p];
        if (c == '"') {
            if (k_ + 1 >= pos_.size()) return false;
            const std::uint32_t q = pos_[k_ + 1];  // stage 1 masks everything inside a string
            if (any_escape_ && std::memchr(t_.data() + p + 1, '\\', q - p - 1) &&
                !escapes_ok(t_.substr(p + 1, q - p - 1)))
                return false;
            nodes_.push_back({Type::Str, p + 1, q - p - 1, static_cast<std::uint32_t>(nodes_.size() + 1)});
            k_ += 2;
            return true;
        }
        if (c != '{' && c != '[') return !is_op(c) && scalar();
        const bool obj = c == '{';
        const std::size_t me = nodes_.size();
        nodes_.push_back({obj ? Type::Obj : Type::Arr, p, 0, 0});
        ++k_;
        const char close = obj ? '}' : ']';
        if (next_tok() == close) {
            ++k_;
        } else {
            for (;;) {
                if (obj) {
                    if (next_tok() != '"' || !value(depth + 1) || next_tok() != ':') return false;
                    ++k_;
                }
                if (!value(depth + 1)) return false;
                const char d = next_tok();
                ++k_;
                if (d == close) break;
                if (d != ',') return false;
            }
        }
        nodes_[me].len = pos_[k_ - 1] + 1 - p;
        nodes_[me].end = static_cast<std::uint32_t>(nodes_.size());
        return true;
    }

    std::string_view t_;
    Buf<std::uint32_t> pos_;
    Buf<Node> nodes_;
    std::size_t k_ = 0;
    std::uint64_t esc_carry_ = 0, str_carry_ = 0, run_carry_ = 0, any_escape_ = 0;
    bool simd_ = __builtin_cpu_supports("avx2") && __builtin_cpu_supports("pclmul");
};

}  // namespace hft::net
