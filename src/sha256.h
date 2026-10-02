#pragma once

#include <cstdint>
#include <cstddef>
#include <string>

// ---------------------------------------------------------------------------
// Minimal SHA-256 implementation for ROM identity verification.
// Standard NIST FIPS 180-4 — not optimized for speed, just correctness.
// ---------------------------------------------------------------------------

class Sha256
{
public:
    Sha256();

    void update(const uint8_t *data, size_t length);
    void update(const std::string &data);

    // Finalize and return 32-byte digest.  Object is reset after this call.
    std::string finalize();

    // Convenience: compute hex digest of a file.  Returns empty string on error.
    static std::string fileDigest(const std::string &path);

    // Convenience: compute hex digest of a byte buffer.
    static std::string bufferDigest(const uint8_t *data, size_t length);

private:
    void transform(const uint8_t block[64]);

    uint32_t state_[8];
    uint8_t  buffer_[64];
    size_t   bufferLen_;
    uint64_t totalLen_;
};
