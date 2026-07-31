# Third-party code and licensing

This port is released under **GPL-3.0** (see `LICENSE`). It carries code from
several upstream projects; this file records where each part came from, because
the licences differ and the originals must keep their attribution.

## Where the code comes from

| Part | Origin | Licence |
|---|---|---|
| `loader/` — bionic ELF loader, relocations, symbol hooking | [gmloader-next](https://github.com/JohnnyonFlame/gmloader-next) by JohnnyonFlame, itself derived from the Vita so-loader by **Andy Nguyen** | GPL (see note) |
| `thunks/libc/` — bionic→glibc thunks | gmloader-next | GPL |
| `thunks/libc/time64.cpp`, `thunk_time64.h` | `y2038` by **Michael G Schwern** | MIT / Artistic |
| `thunks/libc/fortify.cpp` | The Android Open Source Project (parts © Regents of the University of California) | Apache-2.0 / BSD |
| `jni/jni.h` | The Android Open Source Project | Apache-2.0 |
| `loader/leb128.h` | Free Software Foundation (binutils) | GPL |
| `thunks/khronos/` | glad generator, Khronos headers | MIT / Apache-2.0 |
| `third_party/powervr/PVRTDecompress.*` | [PowerVR SDK](https://github.com/powervr-graphics/Native_SDK) by Imagination Technologies | MIT |
| `src/vfp_vector_patch.cpp` register decoding/scalar emission | adapted from VFPVector by **Bythos14** | MIT |
| `tools/eapx.py` — transactional first-boot donor extractor | written by **EapRules** | GPL-3.0 |
| `android/`, `jni/classes/xt_*`, `src/`, `harness/`, `ports/` | written for this port | GPL-3.0 |

The ARM shared libraries under `libs.armhf/` are copied from Debian armhf
packages by `tools/collect_libs.sh`. The release includes the exact Debian
copyright file for every bundled SONAME under `licenses/libraries/`.

## Note on the GPL version

Upstream is not self-consistent, so this is worth stating plainly rather than
leaving for someone to trip over:

- The gmloader-next README says the project is released under **GPLv2**.
- But `loader/so_util.cpp` carries Andy Nguyen's original header, which says
  **GPLv3**, and the bundled licence text includes the customary *"either
  version 2 of the License, or (at your option) any later version"*.

GPL-3.0 is the version that satisfies both readings, so that is what this
repository uses. `loader/LICENSE-gmloader.md` is kept verbatim as it was
received. If JohnnyonFlame states that gmloader-next is GPLv2-**only**, this
repository will relicense to match — open an issue and it will be corrected.

## What is *not* in here

No game code, assets, or data from Dead Space are distributed by this project.
The supported binary is EA/IronMonkey's Xperia Play release; you supply your
own copy. The port loads it at runtime and circumvents no protection.

The 4:3 screenshot under `ports/deadspace/` was captured from the user's own
copy through this loader. The 4:3 cover is an original promotional composition
created for this port from user-directed/generated artwork and a gameplay
scene. Both are included only to identify the game in the PortMaster menu and
are not required for the port to run. If a rights holder would rather they
were not distributed, open an issue and they come out.
