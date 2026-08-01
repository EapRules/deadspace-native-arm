#pragma once
#include "jni.h"
#include "jni_internals.h"

/*
 * com/ea/EAIO/EAIO — the engine's file I/O front door.
 *
 * On Android this class is nothing but a declaration: its methods are native,
 * so calling EAIO.Startup() through JNI lands straight back in the same .so, at
 * Java_com_ea_EAIO_EAIO_Startup. The class exists only so the C++ side has
 * something to call by name.
 *
 * It therefore has to exist here too, and forward rather than emulate.
 */
class EAIO : public Object {
public:
    static Class clazz;
    Class *_getClass() { return &clazz; }
};
