/*
 * Where the C++ unwinder is supposed to find the exception tables.
 *
 * Dead Space links its own copy of libgcc's ARM unwinder, so throwing is
 * entirely the game's business - except for one hole the ABI leaves open. On
 * ARM the unwind data is not walked from a frame pointer, it is looked up in a
 * sorted table (.ARM.exidx) keyed by the return address, and libgcc does not
 * know where that table lives. It asks:
 *
 *     _Unwind_Ptr __gnu_Unwind_Find_exidx(_Unwind_Ptr pc, int *pcount);
 *
 * On a device bionic answers it by searching the dynamic linker's list of
 * loaded objects. Here the host's libgcc answers it instead - and its
 * implementation searches the *host* linker's list, via dl_iterate_phdr.
 * libdeadspace.so is not in that list and never can be: this loader mapped it
 * out of a zip file with mmap, so as far as glibc is concerned those pages are
 * anonymous memory belonging to nothing.
 *
 * The result is not a crash at the lookup. It is worse, because it looks like
 * a bug in the game: the lookup returns "no table", _Unwind_RaiseException
 * gives up with _URC_FAILURE, __cxa_throw treats that as "no handler exists
 * anywhere" and calls std::terminate. Every throw in the module becomes fatal
 * even when the very next frame has a matching catch.
 *
 * That is exactly how Dead Space was dying at M4. xt::TextureLoader::loadTexture2D
 * asks xt::AndroidFileSystem::exists() whether a file is there; exists()
 * answers by constructing an xt::FileInStream inside a try/catch and reporting
 * whether the constructor threw. A perfectly ordinary "file is missing" became
 *
 *     std::terminate -> the default handler -> strb r2,[0xdeadcab1] -> abort
 *
 * (0xdeadcab1 is gabi++'s deliberate poison store, so a tombstone names the
 * cause; here it just surfaced as SIGSEGV at 0xdeadcab1.)
 *
 * A sibling port never needed this. Same engine, same unwinder - it simply never
 * threw on the paths the port reaches, so the hole stayed invisible.
 *
 * The fix is to answer the question ourselves for the modules this loader
 * mapped, and to hand everything else back to the host's implementation, which
 * is still the right answer for a PC inside SDL or the C library.
 */
#include <stdint.h>

#include <vector>

#include "so_util.h"
#include "thunk_gen.h"

/* Every .ARM.exidx entry is a pair of 32-bit words; the count libgcc wants is
 * a number of entries, not bytes. */
#define ARM_EXIDX_ENTRY_SIZE 8

/* so_util.cpp keeps this, and pushes a module onto it before relocating or
 * running its .init_array - which matters, because a constructor is allowed to
 * throw and would otherwise reach us before the module was known. */
extern std::vector<so_module *> loaded_modules;

#if defined(__arm__)

/* The host's own implementation, still the right answer for any PC outside a
 * module this loader mapped. Declared here rather than pulled from <unwind.h>
 * so the signature stays the plain-integer one the ABI actually specifies. */
extern "C" uintptr_t __gnu_Unwind_Find_exidx(uintptr_t pc, int *pcount);

extern "C" uintptr_t deadspace_unwind_find_exidx(uintptr_t pc, int *pcount)
{
    for (so_module *mod : loaded_modules) {
        if (!mod)
            continue;

        /* Only the executable segment can hold a return address. */
        if (pc < mod->text_base || pc >= mod->text_base + mod->text_size)
            continue;

        /* Read from so_module, never from mod->phdr: the header pointers aim
         * into the file buffer that so_load_module() frees as soon as the
         * module is mapped, and a throw happens long after that. so_load()
         * copies the segment's mapped address into arm_exidx for exactly this
         * reason. */
        if (!mod->arm_exidx) {
            /* The PC is ours but the module ships no unwind table. Saying so
             * is the honest answer; falling through to the host would only
             * produce a table belonging to somebody else's code. */
            if (pcount)
                *pcount = 0;
            return 0;
        }

        if (pcount)
            *pcount = (int)(mod->arm_exidx_size / ARM_EXIDX_ENTRY_SIZE);
        return mod->arm_exidx;
    }

    return __gnu_Unwind_Find_exidx(pc, pcount);
}

#endif /* __arm__ */

DynLibFunction symtable_unwind[] = {
#if defined(__arm__)
    /* Two integer arguments, so no soft-float bridge is involved. */
    NO_THUNK("__gnu_Unwind_Find_exidx", (uintptr_t)&deadspace_unwind_find_exidx),
#endif
    { NULL, 0 },
};
