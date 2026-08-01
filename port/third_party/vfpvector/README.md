# VFPVector attribution

`src/vfp_vector_patch.cpp` adapts the A32 VFP register decoder and scalar code
generator from [Bythos14/VFPVector](https://github.com/bythos14/VFPVector),
commit `d95ba13` ("Fix mis-identification of VABS and VSQRT").

The upstream project is MIT-licensed. Its exception-handler integration is
Vita-specific; this port uses a validated eager patch list for the pinned Dead
Space Xperia Play binary.
