#pragma once

// ---------------------------------------------------------------------------
// Minimal PNG encoder (Stage 6.27: screen snapshot for MCP/Agent)
//
// Encodes an RGBA8888 byte buffer into a PNG image held entirely in memory.
//
// Deliberately DEPENDENCY-FREE (no SDL2_image, no libpng, no zlib): the
// debugger_agent library is also compiled into the mock-backed unit tests
// which must not pull in SDL/image system libraries. IDAT is emitted as
// stored (uncompressed) DEFLATE blocks, which are a valid zlib stream per the
// PNG spec. Output is larger than a compressed PNG but is a correct image.
//
// This is a read-only conversion helper — it never touches the emulator.
// ---------------------------------------------------------------------------

#include <cstdint>
#include <cstddef>
#include <vector>
#include <string>

namespace png {

// Encode `width` x `height` pixels supplied as packed RGBA bytes
// (4 bytes per pixel, row-major, no padding) into an in-memory PNG.
// Returns the full PNG file bytes (starting with the 8-byte PNG signature).
std::vector<uint8_t> encodeRGBA(const uint8_t *rgba, int width, int height);

// Convenience: base64-encode arbitrary bytes (standard alphabet, '=' pad).
std::string base64Encode(const uint8_t *data, size_t len);
inline std::string base64Encode(const std::vector<uint8_t> &v) {
    return base64Encode(v.data(), v.size());
}

} // namespace png
