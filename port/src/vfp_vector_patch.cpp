/*
 * VFP short-vector compatibility for the Dead Space audio mixer.
 *
 * Register decoding and scalar code generation are adapted from VFPVector by
 * Bythos, commit d95ba13 ("Fix mis-identification of VABS and VSQRT"), MIT.
 * The original library traps unsupported instructions on PlayStation Vita and
 * patches them lazily. Linux/ARM cannot use that mechanism: Cortex-A35 accepts
 * the same opcodes but treats FPSCR LEN/STRIDE as RAZ/WI, silently executing
 * only lane zero. This pinned game binary is therefore patched eagerly.
 *
 * Only the five F32 operations actually present in the 20 measured vector
 * regions are decoded. Every source opcode is checked before modification.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "arm32_encodings.h"
#include "so_util.h"
#include "trace.h"

#include "vfp_vector_patch.h"

extern uintptr_t so_alloc_arena(so_module *so, uintptr_t range,
                                uintptr_t dst, size_t size);

enum class VfpOp {
    Move,
    Add,
    Subtract,
    Multiply,
    MultiplyAccumulate,
};

struct DecodedVfp {
    VfpOp op;
    uint8_t condition;
    uint8_t destination;
    uint8_t left;
    uint8_t right;
    bool uses_left;
};

struct VectorPatch {
    uint32_t offset;
    uint32_t expected;
    uint8_t lanes;
};

/*
 * Extracted from the pinned Xperia Play v1.1.33 binary. These are exactly the
 * arithmetic instructions between the 20 FPSCR LEN=3/LEN=7 setup/reset pairs;
 * loads, stores, conversions and core-register transfers remain untouched.
 */
static const VectorPatch kVectorPatches[] = {
    {0x0025e2f0, 0xee288a04, 4},
    {0x0025e2f4, 0xee2aaa04, 4},
    {0x0025e308, 0xee0c8a06, 4},
    {0x0025e31c, 0xee0e8a06, 4},
    {0x0025e330, 0xeeb06a48, 4},
    {0x0025e334, 0xee0caa06, 4},
    {0x0025e348, 0xee0eaa06, 4},
    {0x00262fb4, 0xee24ca00, 8},
    {0x00262fbc, 0xee3cca08, 8},
    {0x00264bc0, 0xee244a00, 8},
    {0x00264bc4, 0xee388a4c, 8},
    {0x00264bc8, 0xee04ca08, 8},
    {0x0026d3b0, 0xee28ca00, 8},
    {0x0026d3b8, 0xee3cca20, 8},
    {0x0026d3bc, 0xee34ca4c, 8},
    {0x0026d3c4, 0xee2cca00, 8},
    {0x0026d3c8, 0xee3cca08, 8},
    {0x0026d3cc, 0xee2c4a01, 8},
    {0x0026d428, 0xee28ca00, 8},
    {0x0026d430, 0xee3cca20, 8},
    {0x0026d434, 0xee34ca4c, 8},
    {0x0026d43c, 0xee2cca00, 8},
    {0x0026d444, 0xee3cca08, 8},
    {0x0026d448, 0xee0c4a01, 8},
    {0x0026f17c, 0x7e244a00, 8},
    {0x0026f180, 0x7e288a00, 8},
    {0x00277d64, 0x7e388a04, 8},
    {0x00277ea0, 0x7e048a00, 8},
    {0x00278508, 0x7e388a04, 8},
    {0x002785cc, 0x7e048a00, 8},
    {0x00278cd4, 0x7e388a04, 8},
    {0x00278dcc, 0x7e048a00, 8},
    {0x00291848, 0x7e388a04, 8},
    {0x00291970, 0x7e048a00, 8},
    {0x00291a28, 0x7e388a04, 8},
    {0x00291ae0, 0x7e048a00, 8},
    {0x00292118, 0x7e388a04, 8},
    {0x00292258, 0x7e388a04, 8},
    {0x002922fc, 0x7e048a00, 8},
    {0x00292344, 0x7e048a00, 8},
};

static bool decode_f32(uint32_t raw, DecodedVfp *decoded)
{
    /* VFP data-processing encoding, single precision only. */
    if ((raw & 0x0f000e10) != 0x0e000a00 || (raw & 0x00000100))
        return false;

    int opc1 = (raw & 0x00b00000) >> 20;
    int opc2 = (raw & 0x000f0000) >> 16;
    int opc3 = (raw & 0x00000040) >> 6;
    decoded->uses_left = true;

    switch (opc1) {
    case 0b0000:
        if (opc3)
            return false;
        decoded->op = VfpOp::MultiplyAccumulate;
        break;
    case 0b0010:
        if (opc3)
            return false;
        decoded->op = VfpOp::Multiply;
        break;
    case 0b0011:
        decoded->op = opc3 ? VfpOp::Subtract : VfpOp::Add;
        break;
    case 0b1011:
        if (!opc3)
            return false; /* VMOV immediate is not used here. */
        opc3 = (raw & 0x000000c0) >> 6;
        if (opc2 != 0 || opc3 != 0b01)
            return false;
        decoded->op = VfpOp::Move;
        decoded->uses_left = false;
        break;
    default:
        return false;
    }

    decoded->condition = raw >> 28;
    decoded->destination =
        ((raw & 0x0000f000) >> 11) | ((raw & 0x00400000) >> 22);
    decoded->left =
        ((raw & 0x000f0000) >> 15) | ((raw & 0x00000080) >> 7);
    decoded->right =
        ((raw & 0x0000000f) << 1) | ((raw & 0x00000020) >> 5);
    return true;
}

static uint32_t encode_f32(const DecodedVfp &decoded,
                           uint32_t destination, uint32_t left,
                           uint32_t right)
{
    uint32_t instruction;
    switch (decoded.op) {
    case VfpOp::Move:               instruction = 0xeeb00a40; break;
    case VfpOp::Add:                instruction = 0xee300a00; break;
    case VfpOp::Subtract:           instruction = 0xee300a40; break;
    case VfpOp::Multiply:           instruction = 0xee200a00; break;
    case VfpOp::MultiplyAccumulate: instruction = 0xee000a00; break;
    }

    instruction |= ((destination & 0x1e) << 11) |
                   ((destination & 0x01) << 22);
    if (decoded.uses_left) {
        instruction |= ((left & 0x1e) << 15) |
                       ((left & 0x01) << 7);
    }
    instruction |= ((right & 0x1e) >> 1) |
                   ((right & 0x01) << 5);
    return instruction;
}

static uint32_t conditional_branch(uintptr_t source, uintptr_t destination,
                                   uint8_t condition)
{
    intptr_t words = ((intptr_t)destination - (intptr_t)source) / 4 - 2;
    return ((uint32_t)condition << 28) | 0x0a000000 |
           ((uint32_t)words & 0x00ffffff);
}

/*
 * Expand one short-vector instruction into its lanes.
 *
 * Kept in one helper so installation and any future opcode self-test cannot
 * drift into two different lane-wrapping implementations.
 *
 * Lane iteration follows VFP: the register file is four banks of eight, a
 * vector wraps inside its own bank, and an operand that lives in bank 0 is a
 * scalar and does not advance. Only the second operand can be scalar in the
 * forms this binary uses.
 */
static void generate_lanes(const DecodedVfp &decoded, unsigned int lanes,
                           uint32_t *out)
{
    int destination = decoded.destination;
    int left = decoded.left;
    int right = decoded.right;
    int destination_bank = destination & 0x18;
    int left_bank = left & 0x18;
    int right_bank = right & 0x18;
    int right_stride = right_bank == 0 ? 0 : 1;

    for (unsigned int lane = 0; lane < lanes; lane++) {
        *out++ = encode_f32(decoded, destination, left, right);
        destination = ((destination + 1) & 0x7) | destination_bank;
        left = ((left + 1) & 0x7) | left_bank;
        right = ((right + right_stride) & 0x7) | right_bank;
    }
}

static bool install_vector_patch(so_module *mod, const VectorPatch &patch)
{
    uint32_t *instruction =
        (uint32_t *)(mod->text_base + patch.offset);
    if (*instruction != patch.expected) {
        warning("VFP vector patch at +0x%08x: expected %08x, found %08x - "
                "skipped\n", patch.offset, patch.expected, *instruction);
        return false;
    }

    DecodedVfp decoded = {};
    if (!decode_f32(*instruction, &decoded)) {
        warning("VFP vector patch at +0x%08x: opcode %08x is outside the "
                "validated F32 subset\n", patch.offset, *instruction);
        return false;
    }

    /*
     * push {r4,r5}; vmrs r4,FPSCR; mov r5,r4;
     * bic r4,r4,#0x370000; vmsr FPSCR,r4.
     *
     * Clearing LEN/STRIDE is essential under qemu, which still implements the
     * old mode; it is harmless on ARMv8 where those fields read as zero.
     */
    static const uint32_t prologue[] = {
        0xe92d0030,
        0xeef14a10,
        0xe1a05004,
        0xe3c44837,
        0xeee14a10,
    };
    /*
     * vmsr FPSCR,r5; pop {r4,r5}; ldr pc,[pc,#-4]; .word return_address.
     */
    static const uint32_t epilogue[] = {
        0xeee15a10,
        0xe8bd0030,
        0xe51ff004,
        0x00000000,
    };

    const size_t words =
        sizeof(prologue) / sizeof(prologue[0]) +
        patch.lanes +
        sizeof(epilogue) / sizeof(epilogue[0]);
    uintptr_t trampoline_address =
        so_alloc_arena(mod, B_RANGE, B_OFFSET((uintptr_t)instruction),
                       words * sizeof(uint32_t));
    if (!trampoline_address) {
        warning("VFP vector patch at +0x%08x: no nearby trampoline space\n",
                patch.offset);
        return false;
    }

    uint32_t trampoline[5 + 8 + 4] = {};
    uint32_t *out = trampoline;
    memcpy(out, prologue, sizeof(prologue));
    out += sizeof(prologue) / sizeof(prologue[0]);

    generate_lanes(decoded, patch.lanes, out);
    out += patch.lanes;

    memcpy(out, epilogue, sizeof(epilogue));
    out[3] = (uint32_t)((uintptr_t)instruction + sizeof(uint32_t));
    memcpy((void *)trampoline_address, trampoline, words * sizeof(uint32_t));

    *instruction = conditional_branch((uintptr_t)instruction,
                                      trampoline_address,
                                      decoded.condition);
    __builtin___clear_cache((char *)trampoline_address,
                            (char *)(trampoline_address +
                                     words * sizeof(uint32_t)));
    __builtin___clear_cache((char *)instruction,
                            (char *)(instruction + 1));
    return true;
}

/* ------------------------------------------------------------------------
 * Self-test
 *
 * The patch list was hand-derived from a disassembly, and every part of it -
 * which registers an opcode names, how a vector wraps inside its bank, whether
 * the second operand is a scalar - is a place to be quietly wrong. Wrong here
 * does not crash. It detunes the audio mixer on a console that cannot be
 * attached to a debugger, in a way that sounds like a bad port.
 *
 * qemu-arm still implements FPSCR LEN/STRIDE, so on the verification host the
 * original instruction and the scalar expansion can both be executed, on
 * identical inputs, and compared register for register. That is the strongest
 * statement available without the hardware: not "the expansion looks right"
 * but "the expansion computes what the vector instruction computed".
 *
 * Trying to infer this from the game instead does not work, and the failed
 * attempt is worth recording: the mixer's output depends on how much game time
 * elapsed, so two runs of the same build already disagree once the audio stops
 * being silence. There is nothing to compare against.
 * ------------------------------------------------------------------------ */

/* push {r4,r5,lr} / vldmia r0,{s0-s31} / vmrs r4,fpscr */
static const uint32_t kStubHead[] = {
    0xe92d4030,
    0xec900a20,
    0xeef14a10,
};
/* vmsr fpscr,r4 / vstmia r1,{s0-s31} / pop {r4,r5,pc} */
static const uint32_t kStubTail[] = {
    0xeee14a10,
    0xec810a20,
    0xe8bd8030,
};

typedef void (*StubFn)(const uint32_t *in, uint32_t *out);

/*
 * Assemble and run one stub. `configure` is the single instruction that
 * derives the working FPSCR in r5 from the saved one in r4, and `body` is what
 * runs under it.
 */
static bool run_stub(void *page, uint32_t configure,
                     const uint32_t *body, unsigned int body_words,
                     const uint32_t *in, uint32_t *out)
{
    uint32_t *code = (uint32_t *)page;
    unsigned int n = 0;

    for (unsigned int i = 0; i < sizeof(kStubHead) / sizeof(kStubHead[0]); i++)
        code[n++] = kStubHead[i];
    code[n++] = configure;
    code[n++] = 0xeee15a10;                 /* vmsr fpscr, r5 */
    for (unsigned int i = 0; i < body_words; i++)
        code[n++] = body[i];
    for (unsigned int i = 0; i < sizeof(kStubTail) / sizeof(kStubTail[0]); i++)
        code[n++] = kStubTail[i];

    __builtin___clear_cache((char *)code, (char *)(code + n));
    ((StubFn)(void *)code)(in, out);
    return true;
}

static unsigned int selftest_vfp_expansion(void)
{
    /* Two pages: one holds the stub being assembled, and it is rewritten for
     * every case rather than kept around. */
    void *page = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) {
        warning("VFP self-test: no executable scratch page\n");
        return 0;
    }

    /* Distinct, exactly representable, mixed-sign inputs. Distinct matters:
     * identical register contents would hide a lane that reads the wrong
     * register, which is the most likely mistake in a hand-decoded list. */
    uint32_t input[32];
    for (int i = 0; i < 32; i++) {
        float v = (float)(i + 1) * ((i & 1) ? -0.25f : 0.5f);
        memcpy(&input[i], &v, sizeof(v));
    }

    unsigned int agreed = 0, disagreed = 0, vector_capable = 0;

    for (const VectorPatch &patch : kVectorPatches) {
        DecodedVfp decoded = {};
        if (!decode_f32(patch.expected, &decoded))
            continue;

        uint32_t lanes[8] = {};
        generate_lanes(decoded, patch.lanes, lanes);

        /* The condition code is carried by the branch that reaches the
         * trampoline, so the arithmetic itself is compared unconditionally. */
        uint32_t original = (patch.expected & 0x0fffffff) | 0xe0000000;

        uint32_t len_bits = (uint32_t)(patch.lanes - 1) << 16;
        uint32_t set_len = 0xe3845800 | (len_bits >> 16);   /* orr r5,r4,#LEN */
        uint32_t clear   = 0xe3c45837;             /* bic r5,r4,#0x370000 */

        uint32_t vector_out[32], scalar_out[32], single_out[32];
        run_stub(page, set_len, &original, 1, input, vector_out);
        run_stub(page, clear, lanes, patch.lanes, input, scalar_out);
        run_stub(page, clear, &original, 1, input, single_out);

        /* Does this host implement short vectors at all? If the vector run and
         * the deliberately-scalar run of the *same* opcode agree, LEN was
         * ignored - and then a mismatch below would say nothing about our
         * expansion. Reported rather than assumed. */
        if (memcmp(vector_out, single_out, sizeof(vector_out)) != 0)
            vector_capable++;

        if (memcmp(vector_out, scalar_out, sizeof(vector_out)) == 0) {
            agreed++;
            continue;
        }

        disagreed++;
        for (int i = 0; i < 32; i++) {
            if (vector_out[i] == scalar_out[i])
                continue;
            warning("VFP self-test +0x%08x (%08x, %u lanes): s%d vector=%08x "
                    "expansion=%08x\n", patch.offset, patch.expected,
                    patch.lanes, i, vector_out[i], scalar_out[i]);
        }
    }

    munmap(page, 4096);

    if (!vector_capable) {
        warning("VFP self-test: this host ignores FPSCR LEN, so the reference "
                "side of the comparison is not a vector - result is not "
                "meaningful here\n");
        return 0;
    }

    trace("VFP self-test: %u/%u expansions reproduce short-vector arithmetic "
          "exactly (%u disagreed, %u opcodes confirmed vectorising on this "
          "host)", agreed, agreed + disagreed, disagreed, vector_capable);
    return disagreed;
}

unsigned int patch_vfp_short_vectors(so_module *mod)
{
    const char *selftest = getenv("DEADSPACE_VFP_SELFTEST");
    if (selftest && *selftest && *selftest != '0')
        selftest_vfp_expansion();

    /*
     * The escape hatch exists to make the expansion falsifiable.
     *
     * qemu-arm still implements FPSCR LEN/STRIDE, so with the patch disabled
     * the mixer runs the game's original short vectors and produces the
     * reference PCM. With it enabled the same mixer runs our scalar
     * trampolines. The AudioTrack digest of the two runs has to match, and if
     * it does not, the bug is here rather than on a console nobody can attach
     * a debugger to.
     *
     * Off by default: the R36S is ARMv8, where LEN/STRIDE read as zero and
     * unpatched vector arithmetic silently computes only lane zero.
     */
    const char *disabled = getenv("DEADSPACE_NO_VFP_PATCH");
    if (disabled && *disabled && *disabled != '0') {
        trace("VFP short vectors: patch disabled by DEADSPACE_NO_VFP_PATCH; "
              "relying on the host implementing LEN/STRIDE (qemu only)");
        return 0;
    }

    unsigned int patched = 0;
    for (const VectorPatch &patch : kVectorPatches)
        if (install_vector_patch(mod, patch))
            patched++;

    trace("VFP short vectors: expanded %u/%zu audio instructions",
          patched, sizeof(kVectorPatches) / sizeof(kVectorPatches[0]));
    return patched;
}
