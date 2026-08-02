#pragma once

#include <cstddef>
#include <cstdint>

namespace atc {

/* Decode AMD ATC blocks to tightly packed RGBA8888 pixels. */
bool decode_rgb(const void *data, std::size_t image_size,
                int width, int height, std::uint8_t *rgba);
bool decode_rgba_explicit(const void *data, std::size_t image_size,
                          int width, int height, std::uint8_t *rgba);

} // namespace atc
