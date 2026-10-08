#pragma once

// In-loop inference of the event transformer (DESIGN.md section 11), weights exported by
// research/transformer.py. Incremental: each event computes one query, key and value per layer
// and attends over a per-symbol ring of the last `window` keys and values, which equals the
// full recompute because training used the same sliding-window causal mask with an ALiBi bias.
// Two paths: scalar reference with libm exp and tanh, and AVX2/FMA float32 with a polynomial
// exp (softmax, and GELU through tanh(z) = 1 - 2 / (exp(2z) + 1)). Single thread, fixed
// reduction order: each path is bit-identical across runs.

#include <immintrin.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace hft::strategy {

struct EventToken {
    int type, side, dist, size;
    float log_dt;
};

class EventTransformer {
   public:
    static constexpr int kTypes = 5, kSides = 2, kDist = 10, kSizes = 8;
    static constexpr int kGen = kTypes * kSides * kDist;

    explicit EventTransformer(const std::string& path) {
        std::ifstream f(path, std::ios::binary);
        std::uint32_t h[8];
        if (!f.read(reinterpret_cast<char*>(h), sizeof h) || h[0] != 0x45564E54)
            throw std::runtime_error("event transformer: bad weights file " + path);
        d_ = static_cast<int>(h[1]), layers_ = static_cast<int>(h[2]), heads_ = static_cast<int>(h[3]);
        hidden_ = static_cast<int>(h[4]), window_ = static_cast<int>(h[5]), nf_ = static_cast<int>(h[7]);
        const std::size_t n = n_ = h[6];
        w_.reset(static_cast<float*>(std::aligned_alloc(64, ((n * 4 + 63) / 64) * 64)));
        if (!f.read(reinterpret_cast<char*>(w_.get()), static_cast<std::streamsize>(n * 4)))
            throw std::runtime_error("event transformer: short weights file");
        float* p = w_.get();
        auto take = [&](std::size_t k) {
            float* q = p;
            p += k;
            return q;
        };
        const auto d = static_cast<std::size_t>(d_), hid = static_cast<std::size_t>(hidden_);
        e_type_ = take(kTypes * d), e_side_ = take(kSides * d), e_dist_ = take(kDist * d);
        e_size_ = take(kSizes * d), e_dt_w_ = take(d), e_dt_b_ = take(d);
        for (int l = 0; l < layers_; ++l) {
            Layer L;
            L.n1 = take(d);
            const float* qw = take(3 * d * d);
            L.qkv = Mat(qw, take(3 * d), 3 * d_, d_);
            const float* ow = take(d * d);
            L.o = Mat(ow, take(d), d_, d_);
            L.n2 = take(d);
            const float* f1 = take(hid * d);
            L.fc1 = Mat(f1, take(hid), hidden_, d_);
            const float* f2 = take(d * hid);
            L.fc2 = Mat(f2, take(d), d_, hidden_);
            layer_.push_back(std::move(L));
        }
        norm_ = take(d);
        const float* fw = take(static_cast<std::size_t>(nf_) * d);
        fh_ = Mat(fw, take(static_cast<std::size_t>(nf_)), nf_, d_);
        const float* gw = take(kGen * d);
        gh_ = Mat(gw, take(kGen), kGen, d_);
        slopes_ = take(static_cast<std::size_t>(heads_));
        if (static_cast<std::size_t>(p - w_.get()) != n || d_ % 8 || hidden_ % 8 || (d_ / heads_) % 8)
            throw std::runtime_error("event transformer: layout mismatch");
    }

    // Per-symbol cache: keys and values of the last `window` events in every layer.
    struct State {
        std::vector<float> k, v;  // [layer][slot][d]
        std::vector<float> kt;    // keys transposed, [layer][d][slot (+8 padding)]: 8 scores per FMA
        std::uint64_t pos = 0;
    };
    State state() const {
        State s;
        const auto n = static_cast<std::size_t>(layers_ * window_ * d_);
        s.k.assign(n, 0), s.v.assign(n, 0);
        s.kt.assign(static_cast<std::size_t>(layers_ * d_ * (window_ + 8)), 0);
        return s;
    }

    int forecasts() const { return nf_; }
    std::size_t weight_bytes() const { return n_ * 4; }

    // One event: forecast logits (forecasts()) and generator logits (kGen; skipped if gen is null).
    template <bool Avx>
    void step(State& s, const EventToken& t, float* forecast, float* gen) const {
        const int d = d_;
        float h[kMaxD], x[kMaxD], qkv[3 * kMaxD], y[kMaxD], a[kMaxH], sc[kMaxW];
        for (int i = 0; i < d; ++i)
            h[i] = e_type_[t.type * d + i] + e_side_[t.side * d + i] + e_dist_[t.dist * d + i] +
                   e_size_[t.size * d + i] + (e_dt_w_[i] * t.log_dt + e_dt_b_[i]);
        const int dh = d / heads_;
        const float scale = 1.0f / std::sqrt(static_cast<float>(dh));
        const int slot = static_cast<int>(s.pos % static_cast<std::uint64_t>(window_));
        const int span = static_cast<int>(std::min<std::uint64_t>(s.pos + 1, static_cast<std::uint64_t>(window_)));
        const int first = (slot - span + 1 + window_) % window_;  // oldest slot in the window
        for (int l = 0; l < layers_; ++l) {
            const Layer& L = layer_[static_cast<std::size_t>(l)];
            rmsnorm<Avx>(h, L.n1, x, d);
            mv<Avx>(L.qkv, x, qkv);
            float* K = s.k.data() + static_cast<std::size_t>(l * window_ * d);
            float* V = s.v.data() + static_cast<std::size_t>(l * window_ * d);
            std::memcpy(K + slot * d, qkv + d, sizeof(float) * static_cast<std::size_t>(d));
            std::memcpy(V + slot * d, qkv + 2 * d, sizeof(float) * static_cast<std::size_t>(d));
            const int wt = window_ + 8;
            float* Kt = s.kt.data() + static_cast<std::size_t>(l * d * wt);
            for (int i = 0; i < d; ++i) Kt[i * wt + slot] = qkv[d + i];
            for (int hd = 0; hd < heads_; ++hd) {
                const float* q = qkv + hd * dh;
                float mx = -INFINITY;
                if constexpr (Avx) {
                    // Valid slots are 0..span-1 (the ring is full, or has not wrapped yet).
                    alignas(32) float raw[kMaxW + 8];
                    scores_avx2(q, Kt + hd * dh * wt, wt, dh, span, raw);
                    // Oldest first: the ring from `first` to its end, then from slot 0.
                    const int run1 = std::min(span, window_ - first);
                    bias_avx2(raw + first, sc, run1, static_cast<float>(span - 1), scale, slopes_[hd], mx);
                    bias_avx2(raw, sc + run1, span - run1, static_cast<float>(span - 1 - run1), scale,
                              slopes_[hd], mx);
                } else {
                    // Oldest first, the order of the training mask's row, for the same rounding.
                    for (int j = 0, sl = first; j < span; ++j, sl = sl + 1 == window_ ? 0 : sl + 1) {
                        const int age = span - 1 - j;
                        sc[j] = dot<false>(q, K + sl * d + hd * dh, dh) * scale - slopes_[hd] * static_cast<float>(age);
                        mx = std::max(mx, sc[j]);
                    }
                }
                if constexpr (Avx) {
                    const float inv = softmax_avx2(sc, span, mx);
                    accumulate_avx2(sc, inv, V, first, span, d, hd * dh, dh, y + hd * dh);
                } else {
                    float sum = 0;
                    for (int j = 0; j < span; ++j) sum += (sc[j] = std::exp(sc[j] - mx));
                    for (int i = 0; i < dh; ++i) y[hd * dh + i] = 0;
                    for (int j = 0, sl = first; j < span; ++j, sl = sl + 1 == window_ ? 0 : sl + 1) {
                        const float pj = sc[j] / sum;
                        for (int i = 0; i < dh; ++i) y[hd * dh + i] += pj * V[sl * d + hd * dh + i];
                    }
                }
            }
            mv<Avx>(L.o, y, x);
            for (int i = 0; i < d; ++i) h[i] += x[i];
            rmsnorm<Avx>(h, L.n2, x, d);
            mv<Avx>(L.fc1, x, a);
            if constexpr (Avx)
                gelu_avx2(a, hidden_);
            else
                for (int i = 0; i < hidden_; ++i) a[i] = gelu(a[i]);
            mv<Avx>(L.fc2, a, x);
            for (int i = 0; i < d; ++i) h[i] += x[i];
        }
        rmsnorm<Avx>(h, norm_, x, d);
        mv<Avx>(fh_, x, forecast);
        if (gen) mv<Avx>(gh_, x, gen);
        ++s.pos;
    }

   private:
    static constexpr int kMaxD = 64, kMaxH = 256, kMaxW = 256 + 8;
    // A linear layer: row-major weights for the scalar path, and for the AVX2 path a copy
    // transposed with rows padded to a multiple of 8, so outputs accumulate 8 at a time.
    struct Mat {
        const float *w = nullptr, *b = nullptr;
        int rows = 0, cols = 0, rows8 = 0;
        std::vector<float> t, b8;
        Mat() = default;
        Mat(const float* w_, const float* b_, int r, int c) : w(w_), b(b_), rows(r), cols(c), rows8((r + 7) & ~7) {
            t.assign(static_cast<std::size_t>(rows8 * cols), 0.0f);
            b8.assign(static_cast<std::size_t>(rows8), 0.0f);
            for (int i = 0; i < rows; ++i) {
                b8[static_cast<std::size_t>(i)] = b[i];
                for (int j = 0; j < cols; ++j) t[static_cast<std::size_t>(j * rows8 + i)] = w[i * cols + j];
            }
        }
    };
    struct Layer {
        const float *n1, *n2;
        Mat qkv, o, fc1, fc2;
    };
    struct Free {
        void operator()(float* p) const { std::free(p); }
    };

    template <bool Avx>
    static void rmsnorm(const float* x, const float* w, float* out, int n) {
        if constexpr (Avx) return rmsnorm_avx2(x, w, out, n);
        float ss = 0;
        for (int i = 0; i < n; ++i) ss += x[i] * x[i];
        const float r = 1.0f / std::sqrt(ss / static_cast<float>(n) + 1e-5f);
        for (int i = 0; i < n; ++i) out[i] = x[i] * r * w[i];
    }
    static float gelu(float x) {
        return 0.5f * x * (1.0f + std::tanh(0.7978845608028654f * (x + 0.044715f * x * x * x)));
    }
    template <bool Avx>
    static float dot(const float* a, const float* b, int n) {
        if constexpr (Avx) return dot_avx2(a, b, n);
        float s = 0;
        for (int i = 0; i < n; ++i) s += a[i] * b[i];
        return s;
    }
    __attribute__((target("avx2,fma"))) static float dot_avx2(const float* a, const float* b, int n) {
        __m256 acc = _mm256_setzero_ps();
        for (int i = 0; i < n; i += 8) acc = _mm256_fmadd_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i), acc);
        const __m128 s4 = _mm_add_ps(_mm256_castps256_ps128(acc), _mm256_extractf128_ps(acc, 1));
        const __m128 s2 = _mm_add_ps(s4, _mm_movehl_ps(s4, s4));
        return _mm_cvtss_f32(_mm_add_ss(s2, _mm_shuffle_ps(s2, s2, 1)));
    }
    // exp on 8 lanes: 2^k * p(r) with x = k ln 2 + r, |r| <= ln2 / 2, degree-6 polynomial
    // (relative error below 2e-7 on [-87, 0], the softmax range after max subtraction).
    __attribute__((target("avx2,fma"))) static __m256 exp_avx2(__m256 x) {
        x = _mm256_max_ps(x, _mm256_set1_ps(-87.0f));
        x = _mm256_min_ps(x, _mm256_set1_ps(88.0f));
        const __m256 k = _mm256_round_ps(_mm256_mul_ps(x, _mm256_set1_ps(1.4426950408889634f)),
                                         _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC);
        __m256 r = _mm256_fnmadd_ps(k, _mm256_set1_ps(0.693145751953125f), x);
        r = _mm256_fnmadd_ps(k, _mm256_set1_ps(1.428606765330187e-06f), r);
        __m256 p = _mm256_set1_ps(1.0f / 720);
        p = _mm256_fmadd_ps(p, r, _mm256_set1_ps(1.0f / 120));
        p = _mm256_fmadd_ps(p, r, _mm256_set1_ps(1.0f / 24));
        p = _mm256_fmadd_ps(p, r, _mm256_set1_ps(1.0f / 6));
        p = _mm256_fmadd_ps(p, r, _mm256_set1_ps(0.5f));
        p = _mm256_fmadd_ps(p, r, _mm256_set1_ps(1.0f));
        p = _mm256_fmadd_ps(p, r, _mm256_set1_ps(1.0f));
        const __m256i e = _mm256_slli_epi32(_mm256_add_epi32(_mm256_cvtps_epi32(k), _mm256_set1_epi32(127)), 23);
        return _mm256_mul_ps(p, _mm256_castsi256_ps(e));
    }
    __attribute__((target("avx2,fma"))) static void rmsnorm_avx2(const float* x, const float* w, float* out, int n) {
        __m256 acc = _mm256_setzero_ps();
        for (int i = 0; i < n; i += 8) acc = _mm256_fmadd_ps(_mm256_loadu_ps(x + i), _mm256_loadu_ps(x + i), acc);
        const __m128 s4 = _mm_add_ps(_mm256_castps256_ps128(acc), _mm256_extractf128_ps(acc, 1));
        const __m128 s2 = _mm_add_ps(s4, _mm_movehl_ps(s4, s4));
        const float ss = _mm_cvtss_f32(_mm_add_ss(s2, _mm_shuffle_ps(s2, s2, 1)));
        const __m256 r = _mm256_set1_ps(1.0f / std::sqrt(ss / static_cast<float>(n) + 1e-5f));
        for (int i = 0; i < n; i += 8)
            _mm256_storeu_ps(out + i, _mm256_mul_ps(_mm256_mul_ps(_mm256_loadu_ps(x + i), r), _mm256_loadu_ps(w + i)));
    }
    // Unnormalized softmax of sc[0..n) in place, returning 1 / sum; lanes past n are padded
    // with -inf.
    __attribute__((target("avx2,fma"))) static float softmax_avx2(float* sc, int n, float mx) {
        const int n8 = (n + 7) & ~7;
        for (int j = n; j < n8; ++j) sc[j] = -INFINITY;
        __m256 acc = _mm256_setzero_ps();
        const __m256 m = _mm256_set1_ps(mx);
        for (int j = 0; j < n8; j += 8) {
            const __m256 e = exp_avx2(_mm256_sub_ps(_mm256_loadu_ps(sc + j), m));
            _mm256_storeu_ps(sc + j, e);
            acc = _mm256_add_ps(acc, e);
        }
        const __m128 s4 = _mm_add_ps(_mm256_castps256_ps128(acc), _mm256_extractf128_ps(acc, 1));
        const __m128 s2 = _mm_add_ps(s4, _mm_movehl_ps(s4, s4));
        return 1.0f / _mm_cvtss_f32(_mm_add_ss(s2, _mm_shuffle_ps(s2, s2, 1)));
    }
    // raw[sl] = q . K[sl] for slots 0..n-1, eight slots per vector from the transposed keys;
    // four blocks at a time so the FMA chains overlap instead of waiting on each other.
    __attribute__((target("avx2,fma"))) static void scores_avx2(const float* q, const float* Kt, int wt, int dh,
                                                                int n, float* raw) {
        const int n8 = (n + 7) & ~7;
        int j = 0;
        for (; j + 32 <= n8; j += 32) {
            __m256 a0 = _mm256_setzero_ps(), a1 = a0, a2 = a0, a3 = a0;
            for (int i = 0; i < dh; ++i) {
                const __m256 qi = _mm256_set1_ps(q[i]);
                const float* row = Kt + i * wt + j;
                a0 = _mm256_fmadd_ps(qi, _mm256_loadu_ps(row), a0);
                a1 = _mm256_fmadd_ps(qi, _mm256_loadu_ps(row + 8), a1);
                a2 = _mm256_fmadd_ps(qi, _mm256_loadu_ps(row + 16), a2);
                a3 = _mm256_fmadd_ps(qi, _mm256_loadu_ps(row + 24), a3);
            }
            _mm256_store_ps(raw + j, a0), _mm256_store_ps(raw + j + 8, a1);
            _mm256_store_ps(raw + j + 16, a2), _mm256_store_ps(raw + j + 24, a3);
        }
        for (; j < n8; j += 8) {
            __m256 acc = _mm256_setzero_ps();
            for (int i = 0; i < dh; ++i)
                acc = _mm256_fmadd_ps(_mm256_set1_ps(q[i]), _mm256_loadu_ps(Kt + i * wt + j), acc);
            _mm256_store_ps(raw + j, acc);
        }
    }
    // dst[k] = src[k] * scale - slope * (age0 - k), and the running maximum of dst.
    __attribute__((target("avx2,fma"))) static void bias_avx2(const float* src, float* dst, int n, float age0,
                                                              float scale, float slope, float& mx) {
        const __m256 vs = _mm256_set1_ps(scale), vsl = _mm256_set1_ps(slope);
        const __m256 step = _mm256_setr_ps(0, 1, 2, 3, 4, 5, 6, 7);
        __m256 vmx = _mm256_set1_ps(mx);
        int k = 0;
        for (; k + 8 <= n; k += 8) {
            const __m256 age = _mm256_sub_ps(_mm256_set1_ps(age0 - static_cast<float>(k)), step);
            const __m256 v = _mm256_sub_ps(_mm256_mul_ps(_mm256_loadu_ps(src + k), vs), _mm256_mul_ps(vsl, age));
            _mm256_storeu_ps(dst + k, v);
            vmx = _mm256_max_ps(vmx, v);
        }
        const __m128 m4 = _mm_max_ps(_mm256_castps256_ps128(vmx), _mm256_extractf128_ps(vmx, 1));
        const __m128 m2 = _mm_max_ps(m4, _mm_movehl_ps(m4, m4));
        mx = _mm_cvtss_f32(_mm_max_ss(m2, _mm_shuffle_ps(m2, m2, 1)));
        for (; k < n; ++k) {
            dst[k] = src[k] * scale - slope * (age0 - static_cast<float>(k));
            mx = std::max(mx, dst[k]);
        }
    }
    // y = inv * sum_j p[j] V[slot j], over the ring as two contiguous runs, with four partial
    // sums per output block so consecutive FMAs do not depend on each other.
    __attribute__((target("avx2,fma"))) void accumulate_avx2(const float* p, float inv, const float* V, int first,
                                                             int span, int d, int off, int dh, float* y) const {
        const int run1 = std::min(span, window_ - first);
        for (int i = 0; i < dh; i += 8) {
            __m256 a[4] = {_mm256_setzero_ps(), _mm256_setzero_ps(), _mm256_setzero_ps(), _mm256_setzero_ps()};
            accumulate_run(p, V + first * d + off + i, run1, d, a);
            accumulate_run(p + run1, V + off + i, span - run1, d, a);
            const __m256 sum = _mm256_add_ps(_mm256_add_ps(a[0], a[1]), _mm256_add_ps(a[2], a[3]));
            _mm256_storeu_ps(y + i, _mm256_mul_ps(sum, _mm256_set1_ps(inv)));
        }
    }
    __attribute__((target("avx2,fma"), always_inline)) static inline void accumulate_run(const float* p,
                                                                                         const float* v, int n,
                                                                                         int d, __m256* a) {
        int j = 0;
        for (; j + 4 <= n; j += 4, v += 4 * d) {
            a[0] = _mm256_fmadd_ps(_mm256_set1_ps(p[j]), _mm256_loadu_ps(v), a[0]);
            a[1] = _mm256_fmadd_ps(_mm256_set1_ps(p[j + 1]), _mm256_loadu_ps(v + d), a[1]);
            a[2] = _mm256_fmadd_ps(_mm256_set1_ps(p[j + 2]), _mm256_loadu_ps(v + 2 * d), a[2]);
            a[3] = _mm256_fmadd_ps(_mm256_set1_ps(p[j + 3]), _mm256_loadu_ps(v + 3 * d), a[3]);
        }
        for (; j < n; ++j, v += d) a[0] = _mm256_fmadd_ps(_mm256_set1_ps(p[j]), _mm256_loadu_ps(v), a[0]);
    }
    // GELU with tanh(z) = 1 - 2 / (exp(2z) + 1).
    __attribute__((target("avx2,fma"))) static void gelu_avx2(float* a, int n) {
        const __m256 c = _mm256_set1_ps(0.7978845608028654f), c3 = _mm256_set1_ps(0.044715f);
        const __m256 one = _mm256_set1_ps(1.0f), half = _mm256_set1_ps(0.5f), two = _mm256_set1_ps(2.0f);
        for (int i = 0; i < n; i += 8) {
            const __m256 x = _mm256_loadu_ps(a + i);
            const __m256 z = _mm256_mul_ps(c, _mm256_fmadd_ps(_mm256_mul_ps(c3, x), _mm256_mul_ps(x, x), x));
            const __m256 t = _mm256_sub_ps(one, _mm256_div_ps(two, _mm256_add_ps(exp_avx2(_mm256_mul_ps(two, z)), one)));
            _mm256_storeu_ps(a + i, _mm256_mul_ps(_mm256_mul_ps(half, x), _mm256_add_ps(one, t)));
        }
    }
    template <bool Avx>
    static void mv(const Mat& m, const float* x, float* out) {
        if constexpr (Avx) {
            mv_avx2(m, x, out);
        } else {
            for (int r = 0; r < m.rows; ++r) out[r] = dot<false>(m.w + r * m.cols, x, m.cols) + m.b[r];
        }
    }
    // Eight, then four row blocks at a time: independent FMA chains hide the FMA latency and
    // each broadcast of x[c] feeds several of them.
    __attribute__((target("avx2,fma"))) static void mv_avx2(const Mat& m, const float* x, float* out) {
        alignas(32) float tmp[kMaxH + 32];
        const float* t = m.t.data();
        int r = 0;
        for (; r + 64 <= m.rows8; r += 64) {
            __m256 a[8];
            for (int k = 0; k < 8; ++k) a[k] = _mm256_loadu_ps(m.b8.data() + r + 8 * k);
            for (int c = 0; c < m.cols; ++c) {
                const __m256 xc = _mm256_set1_ps(x[c]);
                const float* row = t + c * m.rows8 + r;
                for (int k = 0; k < 8; ++k) a[k] = _mm256_fmadd_ps(xc, _mm256_loadu_ps(row + 8 * k), a[k]);
            }
            for (int k = 0; k < 8; ++k) _mm256_store_ps(tmp + r + 8 * k, a[k]);
        }
        for (; r + 32 <= m.rows8; r += 32) {
            __m256 a0 = _mm256_loadu_ps(m.b8.data() + r), a1 = _mm256_loadu_ps(m.b8.data() + r + 8);
            __m256 a2 = _mm256_loadu_ps(m.b8.data() + r + 16), a3 = _mm256_loadu_ps(m.b8.data() + r + 24);
            for (int c = 0; c < m.cols; ++c) {
                const __m256 xc = _mm256_set1_ps(x[c]);
                const float* row = t + c * m.rows8 + r;
                a0 = _mm256_fmadd_ps(xc, _mm256_loadu_ps(row), a0);
                a1 = _mm256_fmadd_ps(xc, _mm256_loadu_ps(row + 8), a1);
                a2 = _mm256_fmadd_ps(xc, _mm256_loadu_ps(row + 16), a2);
                a3 = _mm256_fmadd_ps(xc, _mm256_loadu_ps(row + 24), a3);
            }
            _mm256_store_ps(tmp + r, a0), _mm256_store_ps(tmp + r + 8, a1);
            _mm256_store_ps(tmp + r + 16, a2), _mm256_store_ps(tmp + r + 24, a3);
        }
        switch ((m.rows8 - r) / 8) {
            case 3: mv_tail<3>(m, x, r, tmp); break;
            case 2: mv_tail<2>(m, x, r, tmp); break;
            case 1: mv_tail<1>(m, x, r, tmp); break;
        }
        std::memcpy(out, tmp, sizeof(float) * static_cast<std::size_t>(m.rows));
    }
    // The last one to three row blocks, sharing each broadcast.
    template <int N>
    __attribute__((target("avx2,fma"), always_inline)) static inline void mv_tail(const Mat& m, const float* x, int r,
                                                                                  float* tmp) {
        const float* t = m.t.data() + r;
        __m256 a[N];
        for (int k = 0; k < N; ++k) a[k] = _mm256_loadu_ps(m.b8.data() + r + 8 * k);
        for (int c = 0; c < m.cols; ++c) {
            const __m256 xc = _mm256_set1_ps(x[c]);
            for (int k = 0; k < N; ++k) a[k] = _mm256_fmadd_ps(xc, _mm256_loadu_ps(t + c * m.rows8 + 8 * k), a[k]);
        }
        for (int k = 0; k < N; ++k) _mm256_store_ps(tmp + r + 8 * k, a[k]);
    }

    int d_ = 0, layers_ = 0, heads_ = 0, hidden_ = 0, window_ = 0, nf_ = 0;
    std::size_t n_ = 0;
    std::unique_ptr<float, Free> w_;
    const float *e_type_, *e_side_, *e_dist_, *e_size_, *e_dt_w_, *e_dt_b_;
    std::vector<Layer> layer_;
    const float *norm_, *slopes_;
    Mat fh_, gh_;
};

}  // namespace hft::strategy
