#!/usr/bin/env python3
"""Audit the VFP short-vector patch list for completeness.

src/vfp_vector_patch.cpp claims its 40 entries are "exactly the arithmetic
instructions between the 20 FPSCR LEN=3/LEN=7 setup/reset pairs". That claim is
the half the in-process self-test cannot check: the self-test proves every
entry present is expanded correctly, and says nothing about an instruction that
was never listed.

A missed one does not fail anywhere visible. Under qemu-arm, which still
implements FPSCR LEN/STRIDE, it runs as a full vector and the harness stays
green; on the R36S, where LEN reads as zero, the same instruction computes lane
zero and discards the rest. The result is an audio mixer that is quietly wrong
only on the hardware nobody can attach a debugger to.

So the check runs the other way round: reconstruct every LEN region from the
disassembly and require that the patch list and the regions agree in both
directions - nothing inside a region left unpatched, nothing patched outside
one.

Usage:  python3 analysis/vfp_coverage.py [path/to/all.dis]

The disassembly is produced with:
    arm-linux-gnueabihf-objdump -d libEAMGameDeadSpace.so > all.dis
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DIS = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    HERE, "..", "..", "dis", "all.dis")
SRC = os.path.join(HERE, "..", "src", "vfp_vector_patch.cpp")

# ---- the patch list, read from the source so this cannot drift -------------
patched = {}
for m in re.finditer(r"\{0x([0-9a-f]{8}),\s*0x([0-9a-f]{8}),\s*(\d+)\}", open(SRC).read()):
    patched[int(m.group(1), 16)] = (int(m.group(2), 16), int(m.group(3)))

# ---- parse the disassembly -------------------------------------------------
line_re = re.compile(r"^\s*([0-9a-f]+):\t([0-9a-f]{8}) \t(.*)$")
insns = []          # (addr, word, text)
by_addr = {}
with open(DIS) as f:
    for line in f:
        m = line_re.match(line)
        if not m:
            continue
        addr = int(m.group(1), 16)
        word = int(m.group(2), 16)
        text = m.group(3).strip()
        by_addr[addr] = len(insns)
        insns.append((addr, word, text))

vmsr = [i for i, (a, w, t) in enumerate(insns) if t.startswith("vmsr")]
print(f"vmsr fpscr sites: {len(vmsr)}")

# ---- classify each vmsr as setup or reset ----------------------------------
# Setup looks like:  vmrs r0,fpscr ; orr r0,r0,#LEN ; vmsr fpscr,r0
# Reset looks like:  ... mov r0,<saved> ; vmsr fpscr,r0   (no orr of LEN bits)
def preceding_len_value(idx):
    """Scan back a few instructions for an ORR that sets FPSCR LEN/STRIDE."""
    for j in range(idx - 1, max(idx - 8, 0), -1):
        a, w, t = insns[j]
        m = re.match(r"orr\s+r0, r0, #(\d+)", t)
        if m:
            return int(m.group(1))
        m = re.match(r"orr\s+r0, r0, #0x([0-9a-f]+)", t)
        if m:
            return int(m.group(1), 16)
    return None

regions = []
i = 0
while i < len(vmsr):
    idx = vmsr[i]
    val = preceding_len_value(idx)
    if val:
        # find the matching close: the next vmsr
        if i + 1 < len(vmsr):
            regions.append((idx, vmsr[i + 1], val))
            i += 2
            continue
    i += 1

print(f"LEN-setting regions: {len(regions)}\n")

# Condition suffixes matter here: half the vector arithmetic in this binary is
# predicated (vaddvc.f32, vmlami.f32, ...), and a regex that only accepts the
# unconditional spelling silently reports those regions as empty.
COND = r"(?:eq|ne|cs|hs|cc|lo|mi|pl|vs|vc|hi|ls|ge|lt|gt|le|al)?"
VFP_ARITH = re.compile(r"^(vadd|vsub|vmul|vmla|vmls|vnmul|vnmla|vnmls|vdiv|"
                       r"vabs|vneg|vsqrt|vmov)" + COND + r"\.f(32|64)\b")

total_covered = 0
total_uncovered = 0
uncovered_rows = []

for open_i, close_i, lenval in regions:
    a_open = insns[open_i][0]
    a_close = insns[close_i][0]
    lanes = ((lenval >> 16) & 0x7) + 1
    stride = ((lenval >> 20) & 0x3) + 1
    body = insns[open_i + 1:close_i]
    arith = [(a, w, t) for (a, w, t) in body if VFP_ARITH.match(t)]
    cov = [x for x in arith if x[0] in patched]
    unc = [x for x in arith if x[0] not in patched]
    total_covered += len(cov)
    total_uncovered += len(unc)
    flag = "" if not unc else "   <-- UNCOVERED"
    print(f"region +0x{a_open:06x}..+0x{a_close:06x}  LEN={lanes} STRIDE={stride}  "
          f"arith={len(arith)} covered={len(cov)} uncovered={len(unc)}{flag}")
    for a, w, t in unc:
        uncovered_rows.append((a, w, t, lanes))

print()
print(f"TOTAL arithmetic inside LEN regions: {total_covered + total_uncovered}")
print(f"  covered by patch list : {total_covered}")
print(f"  NOT covered           : {total_uncovered}")
print(f"patch list entries      : {len(patched)}")

if uncovered_rows:
    print("\nUNCOVERED instructions (would run 8 lanes on qemu, 1 lane on device):")
    for a, w, t, lanes in uncovered_rows:
        print(f"    {{0x{a:08x}, 0x{w:08x}, {lanes}}},   /* {t} */")

# ---- patch entries that fall outside any region ----------------------------
in_region = set()
for open_i, close_i, _ in regions:
    for a, w, t in insns[open_i + 1:close_i]:
        in_region.add(a)
stray = [a for a in patched if a not in in_region]
if stray:
    print("\nPatch entries NOT inside any LEN region (would corrupt scalar code):")
    for a in sorted(stray):
        j = by_addr.get(a)
        t = insns[j][2] if j is not None else "?"
        print(f"    +0x{a:08x}  {t}")

# ---- expected-word mismatches ---------------------------------------------
bad = []
for a, (expected, lanes) in patched.items():
    j = by_addr.get(a)
    if j is None:
        bad.append((a, expected, None))
    elif insns[j][1] != expected:
        bad.append((a, expected, insns[j][1]))
if bad:
    print("\nExpected-word mismatches:")
    for a, e, f in bad:
        print(f"    +0x{a:08x} expected {e:08x} found {f}")
else:
    print("\nAll patch entries match the binary word-for-word.")

sys.exit(1 if (uncovered_rows or stray or bad) else 0)
