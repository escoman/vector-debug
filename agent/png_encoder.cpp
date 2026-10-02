#include "png_encoder.h"

#include <cstring>

namespace png {

// ---------------------------------------------------------------------------
// CRC32 (PNG chunk checksum, polynomial 0xEDB88320) and Adler32 (zlib
// checksum) implemented locally to keep the encoder free of zlib/libpng.
// ---------------------------------------------------------------------------

namespace {

uint32_t crc32Compute(const uint8_t *data, size_t len)
{
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t n = 0; n < 256; ++n) {
            uint32_t c = n;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[n] = c;
        }
        ready = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i)
        c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

uint32_t adler32Compute(const uint8_t *data, size_t len)
{
    const uint32_t MOD = 65521u;
    uint32_t a = 1, b = 0;
    size_t i = 0;
    while (i < len) {
        size_t block = len - i;
        if (block > 5552) block = 5552;   // keep b within 32-bit before mod
        for (size_t end = i + block; i < end; ++i) {
            a += data[i];
            b += a;
        }
        a %= MOD;
        b %= MOD;
    }
    return (b << 16) | a;
}

void pushBE32(std::vector<uint8_t> &out, uint32_t v)
{
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8)  & 0xFF));
    out.push_back(static_cast<uint8_t>( v        & 0xFF));
}

// Append one PNG chunk: 4-byte length, type, data, 4-byte CRC (over type+data).
void appendChunk(std::vector<uint8_t> &out, const char type[4],
                 const uint8_t *data, size_t len)
{
    pushBE32(out, static_cast<uint32_t>(len));
    size_t crcStart = out.size();
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(type[i]));
    out.insert(out.end(), data, data + len);
    pushBE32(out, crc32Compute(out.data() + crcStart, out.size() - crcStart));
}

} // namespace

// ---------------------------------------------------------------------------
// encodeRGBA
// ---------------------------------------------------------------------------

std::vector<uint8_t> encodeRGBA(const uint8_t *rgba, int width, int height)
{
    std::vector<uint8_t> out;
    if (width <= 0 || height <= 0 || !rgba) return out;

    // PNG signature
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    out.insert(out.end(), sig, sig + 8);

    // IHDR: width, height, bit depth 8, color type 6 (RGBA),
    //        compression 0, filter 0, interlace 0
    uint8_t ihdr[13];
    ihdr[0] = static_cast<uint8_t>((width  >> 24) & 0xFF);
    ihdr[1] = static_cast<uint8_t>((width  >> 16) & 0xFF);
    ihdr[2] = static_cast<uint8_t>((width  >> 8)  & 0xFF);
    ihdr[3] = static_cast<uint8_t>( width         & 0xFF);
    ihdr[4] = static_cast<uint8_t>((height >> 24) & 0xFF);
    ihdr[5] = static_cast<uint8_t>((height >> 16) & 0xFF);
    ihdr[6] = static_cast<uint8_t>((height >> 8)  & 0xFF);
    ihdr[7] = static_cast<uint8_t>( height        & 0xFF);
    ihdr[8]  = 8;   // bit depth
    ihdr[9]  = 6;   // color type RGBA
    ihdr[10] = 0;   // compression
    ihdr[11] = 0;   // filter
    ihdr[12] = 0;   // interlace
    appendChunk(out, "IHDR", ihdr, sizeof(ihdr));

    // Raw image data: each scanline is prefixed with a filter-type byte (0=None).
    const size_t rowBytes   = static_cast<size_t>(width) * 4;
    const size_t stride     = rowBytes + 1;
    std::vector<uint8_t> raw(stride * static_cast<size_t>(height));
    for (int y = 0; y < height; ++y) {
        uint8_t *dst = raw.data() + static_cast<size_t>(y) * stride;
        dst[0] = 0; // filter: None
        std::memcpy(dst + 1, rgba + static_cast<size_t>(y) * rowBytes, rowBytes);
    }

    // IDAT: zlib stream wrapping STORED (uncompressed) DEFLATE blocks.
    std::vector<uint8_t> idat;
    idat.push_back(0x78); // CMF: deflate, 32K window
    idat.push_back(0x01); // FLG: no dict, fastest (CMF*256+FLG % 31 == 0)

    const size_t MAX_BLOCK = 65535;
    size_t off = 0;
    do {
        size_t remaining = raw.size() - off;
        size_t n = remaining > MAX_BLOCK ? MAX_BLOCK : remaining;
        bool final = (remaining <= MAX_BLOCK);
        idat.push_back(final ? 0x01 : 0x00);        // BFINAL + BTYPE=00 (stored)
        idat.push_back(static_cast<uint8_t>(n & 0xFF));
        idat.push_back(static_cast<uint8_t>((n >> 8) & 0xFF));
        uint16_t nlen = static_cast<uint16_t>(~n);
        idat.push_back(static_cast<uint8_t>(nlen & 0xFF));
        idat.push_back(static_cast<uint8_t>((nlen >> 8) & 0xFF));
        idat.insert(idat.end(), raw.begin() + off, raw.begin() + off + n);
        off += n;
    } while (off < raw.size());

    pushBE32(idat, adler32Compute(raw.data(), raw.size()));
    appendChunk(out, "IDAT", idat.data(), idat.size());

    // IEND
    appendChunk(out, "IEND", nullptr, 0);

    return out;
}

// ---------------------------------------------------------------------------
// base64Encode
// ---------------------------------------------------------------------------

std::string base64Encode(const uint8_t *data, size_t len)
{
    static const char *ALPHABET =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    size_t i = 0;
    while (i + 3 <= len) {
        uint32_t n = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out.push_back(ALPHABET[(n >> 18) & 0x3F]);
        out.push_back(ALPHABET[(n >> 12) & 0x3F]);
        out.push_back(ALPHABET[(n >> 6)  & 0x3F]);
        out.push_back(ALPHABET[ n        & 0x3F]);
        i += 3;
    }
    if (i < len) {
        uint32_t n = data[i] << 16;
        bool two = (i + 1 < len);
        if (two) n |= data[i + 1] << 8;
        out.push_back(ALPHABET[(n >> 18) & 0x3F]);
        out.push_back(ALPHABET[(n >> 12) & 0x3F]);
        out.push_back(two ? ALPHABET[(n >> 6) & 0x3F] : '=');
        out.push_back('=');
    }
    return out;
}

} // namespace png
