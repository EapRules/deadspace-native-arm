# Technical deep dive

**How a 2011 Android ARM binary runs natively on modern ARM Linux — no
emulator, no Android runtime, no recompilation.**

The R36S is the reference device, but nothing here is specific to it. This is
an architecture-level result: the techniques apply to any ARMv8 Linux system
with an OpenGL driver, and to a whole class of JNI-driven Android games whose
source code is gone.

`libEAMGameDeadSpace.so` is a 4.5 MB ARMv5TE **softfp** library compiled with
a 2011 NDK against Android 2.x's bionic libc, importing GLES 1.1 fixed
function and expecting a Java virtual machine above it. The host is an ARMv8
**hardfp** glibc Linux with a shader-based GL driver and no JVM. Every layer
of that sentence is an incompatibility. This document is about how each one
was closed, measurably, without touching the game's code on disk.

## 1. The loading problem: bionic ELF on a glibc host

The library is loaded by a bionic-compatible ELF loader (derived from
gmloader-next, itself derived from the Vita so-loader lineage): manual
mapping, relocation processing and symbol resolution against a controlled
symbol table instead of the host dynamic linker. Every libc import the game
makes is answered by a *thunk* — a function with bionic's contract on the
outside and glibc (or a reimplementation) on the inside.

That indirection is where the real danger lives, because the two worlds
disagree about far more than symbol names:

- **Struct layouts.** Bionic's `pthread_mutex_t` is 4 bytes; glibc's is much
  larger. Passing the game's 4-byte handle to a glibc function silently
  overwrites 20 bytes of adjacent game memory. The same class of mismatch
  affects `pthread_attr_t` offsets (writing a scheduling parameter erased the
  game's DETACHED flag and leaked threads) and `timespec` width in
  `pthread_cond_timedwait` (8 vs 16 bytes — a thread that waits forever or
  spins, with no log either way).
- **Calling conventions.** The game is softfp ARMv5TE; the loader is hardfp.
  Floating-point arguments cross the boundary in core registers in one
  direction and VFP registers in the other. Every callback that crosses the
  boundary — the touch-pointer callback, for instance — must use base AAPCS
  explicitly.
- **Compiler-era undefined behavior.** `va_arg` with types narrower than
  `int` is UB that a 2011 compiler tolerated and a modern one turns into an
  undefined instruction. A `ldrd` on the game's `va_list` demands 8-byte
  alignment the game never guaranteed (`SIGBUS`). JNI dispatch had to promote
  variadic types and walk argument lists with explicit alignment barriers.

Seven distinct crashes traced back to this single root cause: *the shim
machinery assumed the caller had the host compiler's guarantees, and the
caller is the output of another compiler from fourteen years ago.* None of
them failed cleanly — every one presented as memory corruption far from the
fault. The transferable rule: when a loaded foreign binary "corrupts memory",
suspect an ABI disagreement in a shim before suspecting the binary.

## 2. Being the JVM: the game is not self-hosting

Unlike NativeActivity games, this engine does not run its own loop. It
exports `JNI_OnLoad` and 68 `Java_*` entry points and expects a Java layer to
create it, hand it a GL surface and call it once per frame:

```text
JNI_OnLoad(vm, NULL)
EAAudioCore.Init(env, activity, AudioTrack, 1MB, 2ch, 44100)
MainActivity.NativeOnCreate()
AndroidRenderer.NativeOnSurfaceCreated()
KeyboardAndroid.NativeOnVisibilityChanged(...)
loop: AndroidRenderer.NativeOnDrawFrame(); swap
```

There is no Java here, so the port *is* the Java layer: a minimal JVM surface
implemented in C++ — `JNIEnv` with a class registry, method registration and
dispatch (including the subtle difference between virtual and non-virtual
registration arity: one builds four parameters where the caller passes
three), real UTF-8 ↔ UTF-16 conversion with surrogate pairs, reference
management, and fake Java objects where the engine expects them.

The deepest of those fakes is `android.media.AudioTrack`: the engine pushes
PCM through JNI method calls on a Java object that does not exist. The port
implements the object, honours the engine's 1 MiB producer ring, and feeds a
bounded hardware period through SDL — after distinguishing the engine's
buffer *accounting* from the hardware buffer *size*, which are four thousand
frames apart in latency.

Asset I/O goes the same way: `AssetManager.list()` semantics (level-only
names, no trailing slashes — the engine concatenates its own), string
ownership rules (a constructor that takes ownership of a buffer the caller
also frees is a double-free that looks like heap corruption), and path
translation from Android's world to the host filesystem.

## 3. Fixed-function graphics on shader-era drivers

The game imports 190 GLES 1.1 fixed-function entry points and exactly zero
shader functions. Modern stacks make this harder than it looks:

- `libGLESv1_CM.so.1` being present means nothing. On glvnd systems it is a
  vendor-neutral *dispatch stub*; if the vendor driver was built without
  GLES1 (Debian's Mesa is), `eglCreateContext` fails with `EGL_BAD_ALLOC`
  for client version 1 while version 2 succeeds — a failure that reads like
  an EGL attribute problem and is not.
- The reliable provider of fixed function is the **desktop GL
  compatibility profile**: GLES 1.1 is essentially a subset of desktop GL
  1.x, which Mesa and Mali both still export. The port resolves all 190
  imports against whichever table the machine actually provides.

Texture compression needed its own investigation. The game's assets exist in
the wild in three donor variants, distinguishable only by content:

| Donor profile | Texture format | Status |
|---|---|---|
| Original Full | PVRTC1 4bpp (`0x8c00/0x8c02`) | Supported |
| Vita RIP | ATC (`0x8c92/0x8c93`) | Supported, software-decoded |
| Repacked ETC1 | ETC1 in `JSR184`/M3G containers | Detected, rejected |

Neither llvmpipe nor Mali-G31 accepts PVRTC; one tested Mali driver
*advertises* PVRTC and then renders the uploads black. The loader therefore
decodes PVRTC and ATC to RGBA8888 in software (Imagination's decoder for
PVRTC) — but only when the driver does not provide a working native path,
because unconditional decoding regressed the donor whose native path worked.
Getting that policy right required an A/B matrix across donors and drivers
with framebuffer-level scoring, not just "did it draw".

The ETC1 variant is the cautionary tale: the engine allocates its textures
with `glTexImage2D(..., NULL)` and never submits the ETC1 payload at all, so
GL reports **zero errors** while drawing black geometry referencing empty
textures. A pipeline that only checked `glGetError` would call it working.

## 4. ARM archaeology: VFP short vectors

The most subtle incompatibility is not an instruction — it is a *mode*.
VFPv2 supported "short vectors": setting the FPSCR `LEN`/`STRIDE` fields
makes ordinary scalar FP instructions operate on register banks of 4 or 8.
The game's audio mixer uses exactly this, in 40 instructions.

From VFPv3 onward those FPSCR fields are RAZ/WI: the write is silently
ignored and the instructions execute **scalar**. No trap, no fault — code
expecting 4 results gets 1. Worse, qemu still emulates the vector semantics,
so an emulation-based test pipeline is *optimistically wrong* about real
hardware: the worst possible divergence.

The fix expands all 40 short-vector instructions into validated scalar
sequences at load time. Two proofs back it: a register-level self-test that
executes every expansion against the original vector semantics and compares
results, and a disassembly audit that proves the 40-instruction list covers
every arithmetic opcode inside every `LEN`-modified region of the binary.

## 5. Patching a binary you cannot recompile

One startup bug lived in the game itself: its virtual filesystem mounts a
provider at the root node, but the VFS lookup only selects providers by
first-named path component, so `/published/...` never resolved and the
engine idled forever on frame 1. The fix is a load-time patch with the
paranoia such a patch deserves:

- the call site is validated by its exact instruction word before anything
  is written;
- a single startup `BL` is replaced by a two-word ARM trampoline;
- the wrapper runs the original mount, then registers one additional mount
  for `/published`, cloning the five-word metadata layout of the engine's
  EASTL UTF-16 strings;
- nothing global is rehooked and no code is toggled at runtime.

The same discipline applies to every patch: validate the exact bytes,
change the minimum, keep the original call path alive.

## 6. Verification as the arbiter

Every claim above is backed by a measurement, because this class of work
fails silently and lies optimistically.

The primary gate is an immutable harness that runs the real binary under
qemu-arm + llvmpipe and asserts seven milestones — load, init, surface,
frames, asset opens, texture uploads, and input-driven scene progression.
The final milestone matters most: a game that only *draws* proves rendering,
not gameplay. The harness feeds synthetic input through the same JNI entry
points a device uses and requires measured scene changes in response. Its
latest accepted run: 570 frames, 84 asset opens, 151 texture uploads, 35,511
draw calls, 4 scene changes after 11 synthetic inputs.

Around the harness sit the tools that found the bugs the harness only
detects: per-call GL diagnostics with immediate error checking, framebuffer
probes scored by mean luma and SSIM against known-good captures (which is
how "renders, but black" and "renders, but white" were caught and
root-caused), traced allocator and pthread shims, and an interactive
emulator that exposes cursor, input, screenshots and logs over a scriptable
control channel — the same failure seen on hardware was reproduced and fixed
locally through it.

## 7. Shipping without shipping the game

The release contains no EA content, so installation has to survive whatever
archive the user actually owns. `eapx`, the first-boot importer, identifies
donors **by content** — correlated hashes of the tree, never filenames —
stages the extraction, validates the complete layout, and only then
publishes it atomically. The donor matrix above exists because archives
circulate under interchangeable names with non-interchangeable contents;
naming a file `full` does not make it the Full donor.

Input, finally, is an emulation problem of its own: the game has no
`AInputQueue` — menus are touch-only and gameplay expects an accelerometer.
The port synthesizes both through the game's JNI input entry points: a
software cursor driven by the D-pad for touch, virtual touch-sticks for
movement and aim, and accelerometer gestures (weapon-mode rotation, Zero-G
jumps) replayed from measured motion samples, mapped to L2/R2.

## What this adds up to

A JNI-driven Android game from 2011 — softfp ARMv5 code, bionic ABI, GLES
1.1, dead texture formats, a deprecated FP vector mode and a missing JVM —
runs natively at full speed on current ARM Linux, verified milestone by
milestone, with its donor data imported safely from whatever the user owns.
None of the techniques are game-specific: the loader, the JNI surface, the
fixed-function bridge, the texture-format policy, the VFP expansion and the
verification method form a reusable path for resurrecting this entire
generation of Android-native games on modern hardware.
