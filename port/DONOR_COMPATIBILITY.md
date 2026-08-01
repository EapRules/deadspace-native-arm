# Dead Space — verified donor matrix and texture formats

State verified on 2026-08-01. This note corrects the earlier assumption that
`deadspace-full.zip` and the original complete tree were the same repacked
PVRTC donor.

## Result

| Tested archive | SHA-256 | Detected profile | Actual textures | Current status |
|---|---|---|---|---|
| Original Full 7z | `11a98fbb228783d8d084c1c5ca682b3e59f72731410ac424988df33ed037923d` | Original Full | PVRTC `0x8c00` / `0x8c02` | Supported; Isaac and the environment render correctly |
| `deadspace-rip.zip` | `04a71b975d0a9aa81b0388aee080f927903507cfbebddc8b57b8959f3eae2d31` | Vita RIP | ATC `0x8c92` / `0x8c93` | Supported via software decoding to RGBA8888 |
| `deadspace-full.zip` | `48d12821638d168ab4009571f4fb6fb8af29de11f51f52f88a842c58055bfe5a` | Converted/repacked Full | ETC1 inside `JSR184` / `MPP-M3G-TXCNV` containers | **Not supported yet**; models and the environment render black |

File names do not identify the format reliably. Classification must be done by
correlated asset hashes, as the eapx recipe does, not by an archive being
called `full`, `rip` or `deadspace.zip`.

## How the naming confusion happened

The port was originally developed against the **original Full PVRTC** 7z
archive, and that is the donor that worked first. Later an archive named
`deadspace-full.zip` appeared; because of its name it was assumed to contain
the same Full tree repacked. That assumption was false: its textures are ETC1.

As a consequence, the internal identifier `full-pvrtc-repacked` is also an
incorrect inherited name. Until a coordinated migration happens, it must be
read as "the unsupported ETC1 profile", never as a second Full PVRTC donor.

Canonical naming for documentation and discussion:

1. **Original Full PVRTC (7z)**: supported.
2. **Vita RIP ATC (ZIP)**: supported.
3. **ETC1 repack `deadspace-full.zip`**: not supported yet.

Do not use "Full ZIP" as shorthand for the first donor.

## Correct installation of the two supported donors

The Vita RIP ATC donor can stay compressed:

```text
ports/deadspace/deadspace.zip
```

The firmware and eapx do not read 7z. For the original Full PVRTC donor,
extract the 7z on a computer, locate the folder containing `assets/` and
`lib/`, and copy it as:

```text
ports/deadspace/gamedata/donor/
├── assets/
└── lib/
    └── armeabi/
        └── libEAMGameDeadSpace.so
```

"The name does not matter" means that eapx identifies APKs, ZIPs and
directories by content, not by their visible label. It does not mean it can
read any container: a 7z must be extracted first.

## Quantified evidence

Each run was installed from scratch from the immutable archive; no previously
extracted tree was restored. The tester used the same local save and captured
the real framebuffer at frames 3000, 3300 and 3600, after the intro animation.

### Original Full PVRTC

```text
donor profile=full-pvrtc-original
pvrtc_decoded=443
rgba=25
failed=0
gl_errors=0
draws=118853
```

All three captures show Isaac with correct materials and lighting.

### Vita RIP ATC

```text
donor profile=vita-rip-atc
atc_decoded=498
rgba=18
failed=0
gl_errors=0
draws=114113
```

All three captures show Isaac correctly textured.

### `deadspace-full.zip` ETC1

```text
donor profile=full-pvrtc-repacked   # inherited, incorrect internal identifier
atc_decoded=0
pvrtc_native=0
pvrtc_decoded=0
rgba=74
failed=0
gl_errors=0
draws=114330
```

Isaac's visual region measured roughly `mean_luma=0.102` and
`ssim=0.001–0.004`: the model and the environment were black even though the
HUD and the draw calls kept working. The tester detects this case and does not
accept a merely non-black framebuffer as proof of compatibility.

## Root cause

The ETC1 archive is not corrupt. Its textures carry the standard `JSR184`
signature and the `MPP-M3G-TXCNV` extension. The public research script
`tex_DeadSpaceMobile.py` reads width, height and payload from offset `0x60`
and processes it with `imageDecodeETC`, confirming it is not PVRTC:

<https://github.com/sleepyzay/Maxscript-Projects/blob/main/Dead%20Space%20Mobile/tex_DeadSpaceMobile.py>

In the current context, the game library allocates RGB/RGBA textures with
`glTexImage2D(..., pixels=NULL)` but never delivers the ETC1 payload to any
compressed-upload call. That is why OpenGL reports no error: it draws geometry
referencing empty textures.

## Compatibility the port must advertise

- Original Full Xperia Play PVRTC: supported.
- Vita RIP ATC: supported.
- The ETC1 archive with SHA-256 `48d128…bfe5a`: detected, but not supported
  until a visually verified ETC1 path exists.
- Mixed trees: rejected; never combine assets from these profiles.

Supporting ETC1 properly requires advertising an ETC1 capability the runtime
actually implements, making the loader emit the `GL_ETC1_RGB8_OES` payload,
and decoding it in software to RGB/RGBA when the driver rejects it. Final
acceptance requires the same three gameplay captures and zero regressions on
PVRTC and ATC.

## Estimated effort and release decision

ETC1 is a **medium**-sized task, not a rewrite of the port:

1. add an ETC1 decoder with a compatible licence;
2. advertise `GL_OES_compressed_ETC1_RGB8_texture` only once the runtime can
   actually honour that capability;
3. intercept `GL_ETC1_RGB8_OES` and upload decoded RGB/RGBA;
4. repeat the deep matrix for ETC1, PVRTC and ATC.

The favourable path should take about 4–8 hours of implementation and testing.
The uncertainty is before OpenGL: if the loader still emits no payload after
ETC1 is advertised, the M3G containers will have to be transformed during
installation; that path can take 1–3 days.

The safe decision for an immediate release is to keep the two confirmed donors
and specifically reject the ETC1 profile with a clear message. The original
Full PVRTC donor must not be withdrawn: only the ETC1 archive that was once
mistaken for it. A short ETC1-advertising spike is worth trying first; if it
produces `GL_ETC1_RGB8_OES` calls, completing the support is reasonable.

Implementation note: in the current state of the code, eapx still recognises
the ETC1 profile under the inherited identifier and can import it; the runtime
then shows black models. The recommended early rejection is not implemented
yet.
