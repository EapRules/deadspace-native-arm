# Compatibility shim so gmloader's generate_libc.py runs against the LLVM 19
# python bindings shipped by Debian trixie. It only re-exports the real
# package; see enumerations.py for the actual patch.
__path__.append("/usr/lib/python3/dist-packages/clang")
