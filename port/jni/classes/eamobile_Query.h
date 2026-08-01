#pragma once
#include "jni.h"
#include "jni_internals.h"

/*
 * com/eamobile/Query — EA's downloadable-content gate.
 *
 * On Android this is a helper class in the launcher APK that answers whether
 * the ~300 MB asset payload has finished downloading and unpacking. Unlike
 * com/ea/EAIO/EAIO its methods are *not* native, so there is no implementation
 * inside libEAMGameDeadSpace.so to forward to: it has to be answered here.
 *
 * Our assets are already on disk before the process starts, so the answer is
 * unconditionally yes.
 */
class Query : public Object {
public:
    static Class clazz;
    Class *_getClass() { return &clazz; }
};
