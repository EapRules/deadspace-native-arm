# PowerVR PVRTC decompressor

`PVRTDecompress.cpp` and `PVRTDecompress.h` are vendored unmodified from the
official PowerVR SDK:

- repository: https://github.com/powervr-graphics/Native_SDK
- path: `framework/PVRCore/texture/`
- commit: `2b1bf2f14d3365d0bb801e2a6a131a319d3a2e48`

The code is used to convert PVRTC textures to RGBA8888 when the active GPU
does not support `GL_IMG_texture_compression_pvrtc`. See `LICENSE.md`.
