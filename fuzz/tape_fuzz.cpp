// The tape and the tree reader must accept the same inputs, and the SIMD and scalar stage 1
// must find the same structure.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string_view>

#include "net/json.hpp"
#include "net/tape.hpp"

namespace {

bool same(const hft::net::JsonTape& a, const hft::net::JsonTape& b, std::uint32_t i) {
    if (a.type(i) != b.type(i) || a.raw(i) != b.raw(i) || a.end(i) != b.end(i)) return false;
    for (std::uint32_t c = a.first(i); c != a.end(i); c = a.next(c))
        if (!same(a, b, c)) return false;
    return true;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string_view s(reinterpret_cast<const char*>(data), size);
    bool tree = true;
    try {
        hft::net::parse_json(s);
    } catch (const std::runtime_error&) {
        tree = false;
    }
    hft::net::JsonTape simd, scalar;
    scalar.use_simd(false);
    const bool a = simd.parse(s), b = scalar.parse(s);
    if (a != b || a != tree) std::abort();
    if (a && !same(simd, scalar, simd.root())) std::abort();
    return 0;
}
