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
| `android/`, `jni/classes/xt_*`, `src/`, `harness/`, `ports/` | written for this port | GPL-3.0 |

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
The game is Mountain Sheep's; you supply your own APK. The port loads it at
runtime and circumvents no protection.

The cover image and the optional 4:3 splash under `ports/deadspace/` are
promotional artwork belonging to **Mountain Sheep**, included only to identify
the game in the PortMaster menu the way every other PortMaster entry does. They
are not game content and are not required for the port to run — delete them and
it still works. If Mountain Sheep would rather they were not distributed, open
an issue and they come out.
