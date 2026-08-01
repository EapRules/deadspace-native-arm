#pragma once
#include "so_util.h"
#include "gl_diag.h"
#include "thunk_gen.h"
template<typename D, typename R, typename... Args>
struct ThunkFloatImplPtr;

template<typename D, typename R, typename... Args>
struct ThunkFloatImplPtr<D, R(*)(Args...)>
{
    __attribute__((noinline)) ABI_ATTR static R bridge(Args... args)
    {
        return D::template bridge_impl<Args...>(args...);
    }

    static constexpr bool has_float_args = has_float_arg<R, Args...>::value;
};

template<typename D, typename R, typename... Args>
struct ThunkFloatImplPtr<D, R(*)(Args...) noexcept>
{
    __attribute__((noinline)) ABI_ATTR static R bridge(Args... args)
    {
        return D::template bridge_impl<Args...>(args...);
    }

    static constexpr bool has_float_args = has_float_arg<R, Args...>::value;
};

template<auto Def, typename PFN>
struct ThunkFloatPtr : ThunkFloatImplPtr<ThunkFloatPtr<Def, PFN>, PFN>
{
public:
    static inline PFN func;
    static inline const char *symname = NULL;
#if defined(TRACE_GL)
    static inline char *trace_symname = NULL;
#endif
    template<typename... Args>
    static auto bridge_impl(Args... args)
    {
#if defined(TRACE_GL)
        if (trace_symname) {
            std::cout << trace_symname << "(";
            ((std::cout << args << ", "), ...);
            std::cout << ")\n";
        }
#endif
        const bool diagnose = gl_diag_enabled();
        if (diagnose)
            gl_diag_before(symname);

        using Result = std::invoke_result_t<PFN, Args...>;
        if constexpr (std::is_void_v<Result>) {
            func(args...);
            if (diagnose)
                gl_diag_after(symname);
        } else {
            Result result = func(args...);
            if (diagnose)
                gl_diag_after(symname);
            return result;
        }
    }
};

// Templates are specialized referencing the static function pointer we're trying
// to resolve, since for these functions we _don't_ want to share thunks.
template <auto F, class T = ThunkFloatPtr<F, std::remove_pointer_t<decltype(F)>>>
uintptr_t select_either_ptr(void *fn, const char *symname)
{
    T::func = (decltype(T::func))fn;
    T::symname = symname;
    
#if defined(TRACE_GL)
    // Always thunk when tracing GL - so we can trace gl calls
    if (strcmp(symname, "gl") == 0 || strcmp(symname, "egl") == 0) {
        T::trace_symname = strdup(symname);
        return (uintptr_t)T::bridge;
    }
#endif

    /*
     * Diagnostic mode deliberately intercepts every call so the GL error can
     * be attributed to the operation that produced it. glGetError itself must
     * remain raw: wrapping it would consume the value before the game sees it.
     */
    if (gl_diag_enabled() && strcmp(symname, "glGetError") != 0)
        return (uintptr_t)T::bridge;

    // When we have ABI_ATTR, we will thunk these so that they are called in the
    // correct ABI.
#ifndef NO_ABI_ATTR
    if constexpr (T::has_float_args)
        return (uintptr_t)T::bridge;
#endif

    // No float/double arguments - just use the original function.
    return (uintptr_t)T::func;
}

template <auto F>
void *resolve_thunked(const char *symbol, int &index, DynLibFunction tab[], void *(*resolve)(const char *symbol))
{
    void *f = (void*)resolve(symbol);
    if (f) {
        tab[index++] = (DynLibFunction){symbol, select_either_ptr<F>(f, symbol)};
        tab[index] = {NULL};
    }

    return f;
}
