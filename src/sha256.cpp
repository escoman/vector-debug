#include "sha256.h"
#include <cstdio>
#include <cstring>
#include <fstream>

// SHA-256 constants (first 32 bits of fractional parts of cube roots of primes)
static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
static inline uint32_t ch(uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (~x & z); }
static inline uint32_t maj(uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (x & z) ^ (y & z); }
static inline uint32_t Sigma0(uint32_t x) { return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22); }
static inline uint32_t Sigma1(uint32_t x) { return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25); }
static inline uint32_t gamma0(uint32_t x) { return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3); }
static inline uint32_t gamma1(uint32_t x) { return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10); }

static uint32_t readBE32(const uint8_t *p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8)  |  uint32_t(p[3]);
}

static void writeBE32(uint8_t *p, uint32_t v) {
    p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16);
    p[2] = uint8_t(v >> 8);  p[3] = uint8_t(v);
}

static void writeBE64(uint8_t *p, uint64_t v) {
    for (int i = 7; i >= 0; --i) { p[i] = uint8_t(v & 0xFF); v >>= 8; }
}

Sha256::Sha256()
    : bufferLen_(0), totalLen_(0)
{
    state_[0] = 0x6a09e667; state_[1] = 0xbb67ae85;
    state_[2] = 0x3c6ef372; state_[3] = 0xa54ff53a;
    state_[4] = 0x510e527f; state_[5] = 0x9b05688c;
    state_[6] = 0x1f83d9ab; state_[7] = 0x5be0cd19;
}

void Sha256::transform(const uint8_t block[64]) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) w[i] = readBE32(block + i * 4);
    for (int i = 16; i < 64; ++i) w[i] = gamma1(w[i-2]) + w[i-7] + gamma0(w[i-15]) + w[i-16];

    uint32_t a=state_[0], b=state_[1], c=state_[2], d=state_[3];
    uint32_t e=state_[4], f=state_[5], g=state_[6], h=state_[7];

    for (int i = 0; i < 64; ++i) {
        uint32_t t1 = h + Sigma1(e) + ch(e,f,g) + K[i] + w[i];
        uint32_t t2 = Sigma0(a) + maj(a,b,c);
        h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }

    state_[0]+=a; state_[1]+=b; state_[2]+=c; state_[3]+=d;
    state_[4]+=e; state_[5]+=f; state_[6]+=g; state_[7]+=h;
}

void Sha256::update(const uint8_t *data, size_t length) {
    totalLen_ += length;
    while (length > 0) {
        size_t space = 64 - bufferLen_;
        size_t copy = (length < space) ? length : space;
        memcpy(buffer_ + bufferLen_, data, copy);
        bufferLen_ += copy;
        data += copy;
        length -= copy;
        if (bufferLen_ == 64) {
            transform(buffer_);
            bufferLen_ = 0;
        }
    }
}

void Sha256::update(const std::string &data) {
    update(reinterpret_cast<const uint8_t*>(data.data()), data.size());
}

std::string Sha256::finalize() {
    // Save the original message length in bits
    uint64_t bitLen = totalLen_ * 8;

    // Append 0x80 padding byte
    uint8_t pad = 0x80;
    update(&pad, 1);

    // Pad with zeros until buffer is at 56 mod 64
    uint8_t zero = 0;
    while (bufferLen_ != 56)
        update(&zero, 1);

    // Append original length in bits (big-endian 64-bit)
    uint8_t lenBuf[8];
    writeBE64(lenBuf, bitLen);
    update(lenBuf, 8);

    // Produce hex digest
    char hex[65];
    for (int i = 0; i < 8; ++i) {
        uint8_t bytes[4];
        writeBE32(bytes, state_[i]);
        for (int j = 0; j < 4; ++j)
            sprintf(hex + i*8 + j*2, "%02x", bytes[j]);
    }
    hex[64] = '\0';

    // Reset state for reuse
    state_[0] = 0x6a09e667; state_[1] = 0xbb67ae85;
    state_[2] = 0x3c6ef372; state_[3] = 0xa54ff53a;
    state_[4] = 0x510e527f; state_[5] = 0x9b05688c;
    state_[6] = 0x1f83d9ab; state_[7] = 0x5be0cd19;
    bufferLen_ = 0;
    totalLen_ = 0;

    return std::string(hex);
}

std::string Sha256::fileDigest(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return "";

    Sha256 ctx;
    char buf[4096];
    while (f.good()) {
        f.read(buf, sizeof(buf));
        std::streamsize n = f.gcount();
        if (n > 0)
            ctx.update(reinterpret_cast<const uint8_t*>(buf), static_cast<size_t>(n));
    }
    return ctx.finalize();
}

std::string Sha256::bufferDigest(const uint8_t *data, size_t length) {
    Sha256 ctx;
    ctx.update(data, length);
    return ctx.finalize();
}
