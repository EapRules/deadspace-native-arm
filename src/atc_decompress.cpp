/*
 * AMD ATC texture decoder.
 *
 * The block layout follows AMD_compressed_ATC_texture and the public-domain
 * compatible decoder published by Perfare's AssetStudio Texture2DDecoder.
 * ATC_RGB_AMD is an 8-byte 4x4 block; ATC_RGBA_EXPLICIT_ALPHA_AMD prefixes
 * the same color block with an 8-byte, DXT3-style four-bit alpha block.
 */
#include "atc_decompress.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace atc {
namespace {

static std::uint8_t expand(std::uint32_t value, int bits)
{
    value <<= 8 - bits;
    return static_cast<std::uint8_t>(value | (value >> bits));
}

static void decode_color_block(const std::uint8_t *src,
                               std::uint8_t *rgba)
{
    const std::uint32_t c0 = static_cast<std::uint32_t>(src[0]) |
                             (static_cast<std::uint32_t>(src[1]) << 8);
    const std::uint32_t c1 = static_cast<std::uint32_t>(src[2]) |
                             (static_cast<std::uint32_t>(src[3]) << 8);

    std::uint8_t colors[4][4] = {};
    if ((c0 & 0x8000u) == 0) {
        colors[0][0] = expand((c0 >> 0) & 0x1f, 5);
        colors[0][1] = expand((c0 >> 5) & 0x1f, 5);
        colors[0][2] = expand((c0 >> 10) & 0x1f, 5);
        colors[3][0] = expand((c1 >> 0) & 0x1f, 5);
        colors[3][1] = expand((c1 >> 5) & 0x3f, 6);
        colors[3][2] = expand((c1 >> 11) & 0x1f, 5);
        for (int channel = 0; channel < 3; ++channel) {
            colors[1][channel] = static_cast<std::uint8_t>(
                (5 * colors[0][channel] + 3 * colors[3][channel]) / 8);
            colors[2][channel] = static_cast<std::uint8_t>(
                (3 * colors[0][channel] + 5 * colors[3][channel]) / 8);
        }
    } else {
        /* ATC's alternate mode uses black, a clamped subtraction, and two
         * endpoints instead of the two interpolated colors. */
        colors[2][0] = expand((c0 >> 0) & 0x1f, 5);
        colors[2][1] = expand((c0 >> 5) & 0x1f, 5);
        colors[2][2] = expand((c0 >> 10) & 0x1f, 5);
        colors[3][0] = expand((c1 >> 0) & 0x1f, 5);
        colors[3][1] = expand((c1 >> 5) & 0x3f, 6);
        colors[3][2] = expand((c1 >> 11) & 0x1f, 5);
        for (int channel = 0; channel < 3; ++channel) {
            colors[1][channel] = static_cast<std::uint8_t>(std::max(
                0, static_cast<int>(colors[2][channel]) -
                   static_cast<int>(colors[3][channel]) / 4));
        }
    }

    const std::uint32_t indices = static_cast<std::uint32_t>(src[4]) |
                                  (static_cast<std::uint32_t>(src[5]) << 8) |
                                  (static_cast<std::uint32_t>(src[6]) << 16) |
                                  (static_cast<std::uint32_t>(src[7]) << 24);
    for (int pixel = 0; pixel < 16; ++pixel) {
        const int index = static_cast<int>((indices >> (pixel * 2)) & 3u);
        std::memcpy(rgba + pixel * 4, colors[index], 3);
        rgba[pixel * 4 + 3] = 255;
    }
}

static void decode_explicit_alpha(const std::uint8_t *src,
                                  std::uint8_t *rgba)
{
    for (int pixel = 0; pixel < 16; ++pixel) {
        const std::uint8_t packed = src[pixel >> 1];
        const std::uint8_t nibble = (pixel & 1) ? (packed >> 4) :
                                                    (packed & 0x0f);
        rgba[pixel * 4 + 3] = static_cast<std::uint8_t>(nibble * 17);
    }
}

template <typename BlockDecoder>
static bool decode_image(const void *data, std::size_t image_size,
                         int width, int height, std::uint8_t *rgba,
                         std::size_t bytes_per_block, BlockDecoder decode)
{
    if (!data || !rgba || width <= 0 || height <= 0)
        return false;
    const std::size_t blocks_x = (static_cast<std::size_t>(width) + 3) / 4;
    const std::size_t blocks_y = (static_cast<std::size_t>(height) + 3) / 4;
    if (blocks_x > std::numeric_limits<std::size_t>::max() / blocks_y ||
        blocks_x * blocks_y > image_size / bytes_per_block)
        return false;

    const auto *bytes = static_cast<const std::uint8_t *>(data);
    std::uint8_t block[16 * 4];
    for (std::size_t by = 0; by < blocks_y; ++by) {
        for (std::size_t bx = 0; bx < blocks_x; ++bx) {
            decode(bytes, block);
            bytes += bytes_per_block;
            for (int y = 0; y < 4; ++y) {
                const std::size_t dst_y = by * 4 + static_cast<std::size_t>(y);
                if (dst_y >= static_cast<std::size_t>(height))
                    continue;
                const std::size_t copy_pixels = std::min<std::size_t>(
                    4, static_cast<std::size_t>(width) - bx * 4);
                if (copy_pixels == 0)
                    continue;
                std::memcpy(rgba + (dst_y * static_cast<std::size_t>(width) + bx * 4) * 4,
                            block + y * 4 * 4, copy_pixels * 4);
            }
        }
    }
    return true;
}

} // namespace

bool decode_rgb(const void *data, std::size_t image_size,
                int width, int height, std::uint8_t *rgba)
{
    return decode_image(data, image_size, width, height, rgba, 8,
                        [](const std::uint8_t *src, std::uint8_t *block) {
                            decode_color_block(src, block);
                        });
}

bool decode_rgba_explicit(const void *data, std::size_t image_size,
                          int width, int height, std::uint8_t *rgba)
{
    return decode_image(data, image_size, width, height, rgba, 16,
                        [](const std::uint8_t *src, std::uint8_t *block) {
                            decode_color_block(src + 8, block);
                            decode_explicit_alpha(src, block);
                        });
}

} // namespace atc
