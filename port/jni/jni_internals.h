#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <tuple>
#include <functional>
#include <type_traits>
#include "jni.h"

// Forwards definitions
struct ManagedMethod;
struct NativeMethod;
struct FieldId;
struct Class;
struct Object;
struct ArrayObject;
struct String;

/*
 * The type va_arg has to be asked for, given the type the method declares.
 *
 * C's default argument promotions apply to everything passed through `...`:
 * anything narrower than int arrives as an int, and a float arrives as a
 * double. Asking va_arg for the narrow type is undefined behaviour, and gcc
 * does not merely warn about it - it stops generating code and emits an
 * undefined instruction in place of the function body.
 *
 * That failure is invisible until the call happens, and then it is a SIGILL at
 * an address inside this loader with the game's return address in lr, which
 * reads like memory corruption rather than a compile-time mistake. The three
 * functions it first appeared in were two bytes apart in the disassembly -
 * bodies of `udf #255` - which is the tell, because real functions cannot be.
 *
 * jboolean is unsigned char, so every JNI method taking a "(Z)" argument hit
 * this. None existed in the inherited classes, which is why the machinery
 * shipped this way; com/ea/blast's sensor delegates are full of them.
 */
template <typename T>
struct va_promoted { using type = T; };

template <> struct va_promoted<bool>           { using type = int; };
template <> struct va_promoted<char>           { using type = int; };
template <> struct va_promoted<signed char>    { using type = int; };
template <> struct va_promoted<unsigned char>  { using type = int; };
template <> struct va_promoted<short>          { using type = int; };
template <> struct va_promoted<unsigned short> { using type = int; };
template <> struct va_promoted<float>          { using type = double; };

template <typename T>
using va_promoted_t = typename va_promoted<T>::type;

/*
 * One argument off the va_list, with a barrier so the compiler cannot merge two
 * of them into a single ldrd.
 *
 * ldrd loads two words at once and requires the address to be 8-byte aligned.
 * gcc emits it freely for consecutive va_arg reads because AAPCS promises an
 * 8-aligned stack - and the promise holds for code gcc compiled. It does not
 * hold here: the va_list belongs to the *game*, built with a 2011 NDK that
 * aligns to 4, so the pair load faults.
 *
 * What that looks like is a SIGBUS at an odd-looking address inside this
 * loader, reached from the game, on a JNI method that merely happens to take
 * two adjacent integers:
 *
 *     3b390:  ldrd r3, r4, [r3]     ; r3 = the va_list
 *
 * AudioTrack.write([SII)I was the first method to hit it because it is the
 * first with two int arguments in a row. Every other overload was one merge
 * away from the same fault, so this is fixed at the template rather than at the
 * call site.
 *
 * The barrier is the whole mechanism: it costs nothing at runtime and it stops
 * the ldrd peephole from seeing the two loads as adjacent. Same family as the
 * va_promoted fix above - the machinery was written for a caller with the
 * host's guarantees and the caller is a different compiler's output.
 */
template <typename T>
static inline T va_next(va_list &va)
{
    T v = (T)va_arg(va, va_promoted_t<T>);
    __asm__ __volatile__("" ::: "memory");
    return v;
}

// Automatic generator for function dispatch
template <auto F, class... Prelude>
struct dispatch
{
    // The following nested structure does two things:
    // 1: Unwraps details about the function (e.g. argument type list,
    //    return type) with the template.
    // 2: Implements the dispatcher as needed (will extract from va_list and push)
    //    into the function call that is being wrapped, taking care to also return
    //    the value if applicable.
    template<typename S>
    struct unwrap;

    template<typename R, typename... Args>
    struct unwrap<R(Prelude..., Args...)>
    {
        // brace-initialization is necessary to ensure the parameters are
        // evaluated in left-to-right order on gcc...
        struct BraceCall
        {
            R ret;
            template <typename... Arg>
            BraceCall(Arg... args) : ret( F(args...) ) { };
        };

        struct BraceCallVoid
        {
            template <typename... Arg>
            BraceCallVoid(Arg... args) { F(args...); };
        };
        
        public:
        /*
         * Read each argument at its promoted width, then narrow back to what
         * the method declared. Promotion is the identity for int, pointers and
         * the 64-bit types, so this only changes behaviour where it was
         * previously undefined - see va_promoted above.
         *
         * dispatch_a below deliberately does NOT promote: it reads from a
         * jvalue array, where every member is already stored at its exact
         * declared width.
         */
        static R dispatch_v(Prelude... pre_args, va_list va)
        {
            if constexpr (std::is_same_v<R, void>) {
                BraceCallVoid{pre_args..., va_next<Args>(va)...};
            } else {
                return BraceCall{pre_args..., va_next<Args>(va)...}.ret;
            }
        }

        static R dispatch_a(Prelude... pre_args, jvalue *arr)
        {
            // Fixes unsequenced nonsense...
            auto get = [&]() { return arr++; };

            if constexpr (std::is_same_v<R, void>) {
                BraceCallVoid{pre_args..., (*((Args*)get()))...};
            } else {
                return BraceCall{pre_args..., *((Args*)get())...}.ret;
            }
        }
    };

    // Unwrap the function
    using sig = unwrap<typename std::remove_pointer<decltype(F)>::type>;

    // Expose the dispatch function :)
    static constexpr auto vargs = sig::dispatch_v;
    static constexpr auto aargs = sig::dispatch_a;
};

template <auto F>
struct get_function_args
{
    // The following nested structure does two things:
    // 1: Unwraps details about the function (e.g. argument type list,
    //    return type) with the template.
    // 2: Implements the dispatcher as needed (will extract from va_list and push)
    //    into the function call that is being wrapped, taking care to also return
    //    the value if applicable.
    template<typename S>
    struct unwrap;

    template<typename R, typename... Args>
    struct unwrap<R(Args...)>
    {
        using args = std::tuple<Args...>;
    };

    using sig = unwrap<typename std::remove_pointer<decltype(F)>::type>;
};

struct Class {
    const char *classpath;
    const char *classname;
    const ManagedMethod *managed_methods;
    const NativeMethod *native_methods;
    const FieldId *fields;
    jsize instance_size;
};

struct Object {
    virtual Class *_getClass() = 0;
};

class ArrayObject : public Object {
public:
    Class *_getClass() { return NULL; } // TODO
    Class *instance_clazz;
    jsize count;
    jsize element_size;
    void *elements;
};

struct ManagedMethod {
    Class *clazz;
    const char *name;
    const char *signature;
    const void *addr_variadic; // For <...> and <va_list> 
    const void *addr_array; // For arrays
    const bool is_static_method;
    /*
     * Whether the dispatcher expects a jclass between the object and the
     * va_list. RegisterNonVirtual builds one that does; Register does not.
     *
     * This has to be recorded rather than inferred, because both kinds are
     * reachable through the same JNI entry points. CallIntMethod goes to
     * iface_CallMethodV, which knows only the method id - and calling a
     * four-argument dispatcher with three arguments does not fail, it reads one
     * register too far and uses whatever is there as the va_list. See the
     * comment on AudioTrack.write(short[]) for what that looks like.
     */
    const bool takes_class = false;

    template <auto *F>
    static const ManagedMethod Register(Class &clazz, const char *name, const char *signature)
    {
        using disp = dispatch<F, JNIEnv *, jobject>;
        using args = typename get_function_args<F>::sig::args;

        static_assert(std::tuple_size_v<args> >= 2, "Invalid number of arguments, expect 2 or more.");
        static_assert(std::is_same_v<JNIEnv *, std::tuple_element_t<0, args>>, "First Method argument expected JNIEnv *.");
        static_assert(std::is_same_v<jobject, std::tuple_element_t<1, args>>, "Second Method argument expected jobject.");

        return ManagedMethod {
            .clazz = &clazz,
            .name = name,
            .signature = signature,
            .addr_variadic = (void*)disp::vargs,
            .addr_array = (void*)disp::aargs,
            .is_static_method = false,
        };
    }

    template <auto *F>
    static const ManagedMethod RegisterStatic(Class &clazz, const char *name, const char *signature)
    {
        using disp = dispatch<F, JNIEnv *, jclass>;
        using args = typename get_function_args<F>::sig::args;

        static_assert(std::tuple_size_v<args> >= 2, "Invalid number of arguments, expect 2 or more.");
        static_assert(std::is_same_v<JNIEnv *, std::tuple_element_t<0, args>>, "First Method argument expected JNIEnv *.");
        static_assert(std::is_same_v<jclass, std::tuple_element_t<1, args>>, "Second Method argument expected jclass.");

        return ManagedMethod {
            .clazz = &clazz,
            .name = name,
            .signature = signature,
            .addr_variadic = (void*)disp::vargs,
            .addr_array = (void*)disp::aargs,
            .is_static_method = true,
        };
    }

    template <auto *F>
    static const ManagedMethod RegisterNonVirtual(Class &clazz, const char *name, const char *signature)
    {
        using disp = dispatch<F, JNIEnv *, jobject, jclass>;
        using args = typename get_function_args<F>::sig::args;

        static_assert(std::tuple_size_v<args> >= 3, "Invalid number of arguments, expect 3 or more.");
        static_assert(std::is_same_v<JNIEnv *, std::tuple_element_t<0, args>>, "First Method argument expected JNIEnv *.");
        static_assert(std::is_same_v<jobject, std::tuple_element_t<1, args>>, "Second Method argument expected jobject.");
        static_assert(std::is_same_v<jclass, std::tuple_element_t<2, args>>, "Third Method argument expected jclass.");

        return ManagedMethod {
            .clazz = &clazz,
            .name = name,
            .signature = signature,
            .addr_variadic = (void*)disp::vargs,
            .addr_array = (void*)disp::aargs,
            .is_static_method = false,
            .takes_class = true,
        };
    }
};

struct NativeMethod {
    Class *clazz;
    const char *name;
    const char *soname;
    void **ptr;
};

struct FieldId {
    Class *clazz; // Keep a reference back to who owns this
    const char *name;
    const char *signature;
    uintptr_t offset; // Direct address for static fields, offsets for instance fields
    int is_static;
};

class ClassRegistry {
public:
    static std::vector<const Class*> &get_class_registry();
    static int register_class(const Class &clazz);
};

class String : public Object {
public:
    static Class clazz;
    Class *_getClass() { return &clazz; }

    char *str;
    String(char *str);
    String(const char *str);
    
    friend String* operator&(_jstring& jstr) { return (String*)&jstr; };
};

extern "C" {
    extern void jni_resolve_native(struct so_module *so);
};

/*
    To help us clear the ambiguity on overloaded class methods,
    we use a handy-dandy trick to optionally generate the function type
    that we want to use optionally.
    - The template parameter V is always void just so we can exploit the ##__VA_ARGS__ GNU extension
    to make the list optional.
    - When R and ...Args are present, we get a type using this information.
    - When R and ...Args aren't present, the compiler thankfully finds a way to deduce which type it wants
    from the argument, which will be a specific function and not some overloaded one.
    - There are overloads for both static and non-static calls (since they have their own preambles)
*/
template <typename V = void, typename R, typename ...Args>
auto DEDUCE_METHOD_HELPER(R (*f)(JNIEnv *, jobject, jclass, Args...))
{
    return f;
}

template <typename V = void, typename R, typename ...Args>
auto DEDUCE_METHOD_HELPER(R (*f)(JNIEnv *, jclass, Args...))
{
    return f;
}

#define DEDUCE_METHOD_TYPE(method, ...) static_cast<decltype(DEDUCE_METHOD_HELPER<void, ##__VA_ARGS__>(method))>(method)

#define REGISTER_STATIC_FIELD(clz, field) \
    {.clazz = &clz::clazz, .name = #field, .offset = (uintptr_t)&clz::field, .is_static = 1}

#define REGISTER_FIELD(clz, field) \
    {.clazz = &clz::clazz, .name = #field, .offset = (uintptr_t)&(((clz*)0x0)->field), .is_static = 0}

#define REGISTER_INIT_METHOD(clz, method, sig, ...) ManagedMethod::RegisterNonVirtual<DEDUCE_METHOD_TYPE(method, ##__VA_ARGS__)>(clz::clazz, "<init>", sig)

#define REGISTER_STATIC_METHOD(clz, method, sig, ...) ManagedMethod::RegisterStatic<DEDUCE_METHOD_TYPE(clz::method, ##__VA_ARGS__)>(clz::clazz, #method, sig)

#define REGISTER_METHOD(clz, method, sig, ...) ManagedMethod::Register<DEDUCE_METHOD_TYPE(clz::method, ##__VA_ARGS__)>(clz::clazz, #method, sig)

#define REGISTER_NONVIRTUAL(clz, method, sig, ...) ManagedMethod::RegisterNonVirtual<DEDUCE_METHOD_TYPE(clz::method, ##__VA_ARGS__)>(clz::clazz, #method, sig)
