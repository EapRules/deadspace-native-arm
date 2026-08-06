/*
 * Host-side check that the ATC decoder puts red in the red byte.
 *
 * The decoder was adapted from a reference that writes BGRA, so the endpoint
 * fields landed in mirrored output bytes and every warm texture came out cold
 * - the bluish tint reported against the ATC (Vita RIP) donor. A pure-red and
 * a pure-blue endpoint are enough to pin the order down, and neither needs a
 * GL context, so this runs with the host compiler:
 *
 *   c++ -std=gnu++20 -I../src -o /tmp/atc_color_test atc_color_test.cpp \
 *       ../src/atc_decompress.cpp && /tmp/atc_color_test
 */
#include "atc_decompress.h"

#include <cstdio>
#include <cstring>

namespace {

/* One ATC_RGB_AMD block: two endpoints and 16 two-bit indices. Index 0 selects
 * c0, index 3 selects c1, so a block of all-zero indices is a flat c0. */
void make_block(std::uint8_t *block, std::uint16_t c0, std::uint16_t c1,
                std::uint32_t indices)
{
    block[0] = static_cast<std::uint8_t>(c0 & 0xff);
    block[1] = static_cast<std::uint8_t>(c0 >> 8);
    block[2] = static_cast<std::uint8_t>(c1 & 0xff);
    block[3] = static_cast<std::uint8_t>(c1 >> 8);
    for (int i = 0; i < 4; ++i)
        block[4 + i] = static_cast<std::uint8_t>((indices >> (i * 8)) & 0xff);
}

int failures = 0;

void expect(const char *what, std::uint16_t c0, std::uint16_t c1,
            std::uint32_t indices, int r, int g, int b)
{
    std::uint8_t block[8];
    make_block(block, c0, c1, indices);

    std::uint8_t rgba[4 * 4 * 4];
    std::memset(rgba, 0, sizeof(rgba));
    if (!atc::decode_rgb(block, sizeof(block), 4, 4, rgba)) {
        std::printf("FAIL %-22s decode_rgb refused the block\n", what);
        failures++;
        return;
    }

    const bool ok = rgba[0] == r && rgba[1] == g && rgba[2] == b &&
                    rgba[3] == 255;
    std::printf("%s %-22s c0=0x%04x c1=0x%04x -> rgba(%3u,%3u,%3u,%3u)"
                "  expected (%3d,%3d,%3d,255)\n",
                ok ? "ok  " : "FAIL", what, c0, c1,
                rgba[0], rgba[1], rgba[2], rgba[3], r, g, b);
    if (!ok)
        failures++;
}

} // namespace

int main(void)
{
    /* c0 is RGB555 with bit 15 clear (the interpolated mode), c1 is RGB565.
     * 5-bit 0x1f and 6-bit 0x3f both expand to 255. */
    expect("c0 pure red",   0x7c00, 0x0000, 0x00000000, 255,   0,   0);
    expect("c0 pure green", 0x03e0, 0x0000, 0x00000000,   0, 255,   0);
    expect("c0 pure blue",  0x001f, 0x0000, 0x00000000,   0,   0, 255);
    expect("c1 pure red",   0x0000, 0xf800, 0xffffffff, 255,   0,   0);
    expect("c1 pure green", 0x0000, 0x07e0, 0xffffffff,   0, 255,   0);
    expect("c1 pure blue",  0x0000, 0x001f, 0xffffffff,   0,   0, 255);

    /* A warm mid-tone, the case the tint was actually visible on: skin and
     * rusty metal have red well above blue and came out the other way round. */
    expect("c0 warm mid",   0x6180, 0x0000, 0x00000000, 198,  99,   0);

    std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
                failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
