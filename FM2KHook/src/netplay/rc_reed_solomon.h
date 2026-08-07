// rc_reed_solomon.h -- compact systematic Reed-Solomon erasure code over GF(256).
//
// For the ReliableChannel FEC: K data blocks + M parity blocks; ANY K of the
// (K+M) recover all K data. Unlike single-XOR-parity (recovers 1 loss/group),
// RS recovers up to M losses per group INSTANTLY (no retransmit round-trip) --
// which is what lets the live spectator stream keep pace under multi-loss bursts.
//
// Cauchy generator matrix (every square submatrix invertible -> MDS -> any K
// survivors decode). Header-only, no deps. GF(256) with primitive poly 0x11D.
#pragma once
#include <cstdint>
#include <cstring>
#include <vector>

namespace fm2k { namespace rc { namespace rs {

struct GF {
    uint8_t exp[512];
    uint8_t log[256];
    GF() {
        uint16_t x = 1;
        for (int i = 0; i < 255; i++) {
            exp[i] = static_cast<uint8_t>(x);
            log[x] = static_cast<uint8_t>(i);
            x <<= 1;
            if (x & 0x100) x ^= 0x11D;
        }
        for (int i = 255; i < 512; i++) exp[i] = exp[i - 255];
        log[0] = 0;
    }
    inline uint8_t mul(uint8_t a, uint8_t b) const {
        if (!a || !b) return 0;
        return exp[log[a] + log[b]];
    }
    inline uint8_t inv(uint8_t a) const {  // a != 0
        return exp[255 - log[a]];
    }
    inline uint8_t divide(uint8_t a, uint8_t b) const {  // b != 0
        if (!a) return 0;
        return exp[(log[a] + 255 - log[b]) % 255];
    }
};

inline const GF& gf() { static GF g; return g; }

// Cauchy coefficient for parity row j (x=j) over data col i (y=M+i): 1/(x^y).
inline uint8_t cauchy(int j, int i, int M) {
    uint8_t xj = static_cast<uint8_t>(j);
    uint8_t yi = static_cast<uint8_t>(M + i);
    return gf().inv(static_cast<uint8_t>(xj ^ yi));
}

// Encode: data[K] each of length L -> parity[M] each of length L.
// data/parity are pointers to K/M byte-buffers of length L.
inline void encode(int K, int M, int L,
                   const uint8_t* const* data, uint8_t* const* parity) {
    const GF& g = gf();
    for (int j = 0; j < M; j++) {
        std::memset(parity[j], 0, L);
        for (int i = 0; i < K; i++) {
            uint8_t c = cauchy(j, i, M);
            if (!c) continue;
            const uint8_t* d = data[i];
            uint8_t* p = parity[j];
            for (int b = 0; b < L; b++) p[b] ^= g.mul(c, d[b]);
        }
    }
}

// In-place GF(256) matrix inverse (n x n), row-reduced. Returns false if singular.
inline bool invert(std::vector<uint8_t>& A, int n) {
    const GF& g = gf();
    std::vector<uint8_t> I(n * n, 0);
    for (int i = 0; i < n; i++) I[i * n + i] = 1;
    for (int col = 0; col < n; col++) {
        int piv = -1;
        for (int r = col; r < n; r++) if (A[r * n + col]) { piv = r; break; }
        if (piv < 0) return false;
        if (piv != col)
            for (int c = 0; c < n; c++) {
                std::swap(A[piv * n + c], A[col * n + c]);
                std::swap(I[piv * n + c], I[col * n + c]);
            }
        uint8_t inv = g.inv(A[col * n + col]);
        for (int c = 0; c < n; c++) { A[col*n+c] = g.mul(A[col*n+c], inv); I[col*n+c] = g.mul(I[col*n+c], inv); }
        for (int r = 0; r < n; r++) {
            if (r == col) continue;
            uint8_t f = A[r * n + col];
            if (!f) continue;
            for (int c = 0; c < n; c++) {
                A[r*n+c] ^= g.mul(f, A[col*n+c]);
                I[r*n+c] ^= g.mul(f, I[col*n+c]);
            }
        }
    }
    A = I;
    return true;
}

// Decode: recover all K data blocks from >= K received blocks.
// present_rows[t] = the generator-matrix row index of the t-th received block
//   (0..K-1 => data block i; K..K+M-1 => parity block j=row-K).
// present_blocks[t] = pointer to that received block (length L).
// out_data[i] = recovered data block i (length L). Returns false if undecodable.
inline bool decode(int K, int M, int L,
                   const std::vector<int>& present_rows,
                   const std::vector<const uint8_t*>& present_blocks,
                   uint8_t* const* out_data) {
    if (static_cast<int>(present_rows.size()) < K) return false;
    const GF& g = gf();
    // Build KxK matrix from the first K present rows' generator coefficients.
    std::vector<uint8_t> A(K * K, 0);
    std::vector<const uint8_t*> b(K);
    for (int t = 0; t < K; t++) {
        int row = present_rows[t];
        b[t] = present_blocks[t];
        if (row < K) {            // data row = identity
            A[t * K + row] = 1;
        } else {                  // parity row
            int j = row - K;
            for (int i = 0; i < K; i++) A[t * K + i] = cauchy(j, i, M);
        }
    }
    if (!invert(A, K)) return false;  // A now = A^-1
    // data = A^-1 * b  (each output block = sum over t of A^-1[i][t] * b[t]).
    for (int i = 0; i < K; i++) {
        std::memset(out_data[i], 0, L);
        for (int t = 0; t < K; t++) {
            uint8_t c = A[i * K + t];
            if (!c) continue;
            const uint8_t* bt = b[t];
            uint8_t* o = out_data[i];
            for (int z = 0; z < L; z++) o[z] ^= g.mul(c, bt[z]);
        }
    }
    return true;
}

}}}  // namespace fm2k::rc::rs
