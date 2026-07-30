# Dead Space — triage previo al port (2026-07-30)

Todo verificado contra el binario y el firmware, nada supuesto.

## El juego

| | |
|---|---|
| Librería | `libEAMGameDeadSpace.so`, 4.499.468 bytes |
| sha1 | `0ed42b611415015807f759ec9b5457857143ce39` |
| Arquitectura | **ARMv5TE + VFPv2** (los anteriores eran v7) |
| GL | **GLES 1.1 puro** — 13 imports de función fija, 0 de GLES2 |
| Imports | solo `libc`, `libstdc++`, `libm`, `liblog`, `libGLESv1_CM` |
| Assets | 1770 archivos, 303 MB, **fuera del APK** |
| Origen | `NeededFiles.7z`, que trae la `.so` y los assets que EA ya no sirve |

## Lo que NO es problema (verificado, no asumido)

**GLES 1.1**: el firmware ya trae `/usr/lib/arm-linux-gnueabihf/libGLESv1_CM.so.1`
con las 13 funciones que el juego importa, 159 símbolos, vía glvnd. Y
`libmali.so.1` armhf **también exporta `glMatrixMode`**, o sea función fija
acelerada por hardware. No hay que escribir thunks ni meter GL4ES. La Vita
necesitó vitaGL porque su GPU no lo tiene; acá viene de fábrica.

**ARMv5TE**: cero `swp`/`swpb`, cero `setend`, cero `ldrt`/`strt`. Nada de lo que
ARMv8 deprecó. Los 17 `cdp` que aparecen son literal pools dentro de `.text` que
objdump desensambla como código — apuntan a coprocesadores que no existen.

## Lo que SÍ cambia respecto de Minigore 2 e Ice Rage

### 1. No es NativeActivity. Nosotros somos el Java.

Los dos ports anteriores importaban `libandroid.so` y el juego se manejaba solo:
nosotros le dábamos el ciclo de vida y él corría su propio bucle.

Dead Space **no importa `libandroid.so`**. Tiene `JNI_OnLoad` y exporta **68
símbolos `Java_*`**. Es el modelo GLSurfaceView + JNI, donde una Activity de Java
crea el contexto GL y llama al nativo en cada frame.

Los puntos de entrada que importan:

| Para qué | Símbolo |
|---|---|
| arranque | `Java_com_ea_EAMGameDeadSpace_EAMGameDeadSpace_runEntryPoint` |
| superficie GL | `Java_com_ea_blast_AndroidRenderer_NativeOnSurfaceCreated` |
| resize | `Java_com_ea_blast_AndroidRenderer_NativeOnSurfaceChanged` |
| **cada frame** | `Java_com_ea_blast_AndroidRenderer_NativeOnDrawFrame` |
| teclas | `Java_com_ea_blast_KeyboardAndroid_NativeOnKeyDown` / `NativeOnKeyUp` |
| audio | `Java_com_ea_EAMAudio_EAMAudioCoreWrapper_NativeStartup` |
| I/O | `Java_com_ea_EAIO_EAIO_Startup` |

**Consecuencia sobre el scaffold**: se reusa el loader, los thunks de libc y la
maquinaria del JNI falso. **No** se reusa `main.cpp` ni `android/platform.cpp`,
que están escritos para conducir un NativeActivity. Hay que escribir un driver
nuevo: crear la ventana y el contexto, llamar `JNI_OnLoad`, `runEntryPoint`,
`NativeOnSurfaceCreated`, y después `NativeOnDrawFrame` en bucle.

En cierto sentido es **más simple**: el bucle es nuestro, no hay que emular un
`ALooper` ni una cola de eventos. Pero es código nuevo, no un rename.

### 2. Los assets viven en el filesystem, no en el APK

`data/deadspace/assets/published/`. El juego los abre por ruta, no por
`AAssetManager`. Hay que ver qué ruta espera antes de escribir nada — puede que
haya que montarlos donde el juego los busca, o interceptar `fopen`.

### 3. Es GLES 1.1

Nuestros thunks de GL están escritos para GLES2 (`android/egl_shim.cpp`,
`thunks/khronos/gles2.cpp`). Para función fija hay que enlazar contra
`libGLESv1_CM` y armar la tabla de símbolos correspondiente. No es difícil —los
símbolos existen— pero es una tabla nueva.

## Referencia externa

`github.com/v-atamanenko/deadspace-vita` (MIT, 96★): 21.635 líneas, **839
entradas `gl*` ya mapeadas** y el JNI falso resuelto. Del mismo autor están
`FalsoJNI` y `soloader-boilerplate`.

Su capa GL no se copia tal cual (usa vitaGL), pero **la tabla de símbolos y el
orden de llamadas al JNI sí**.

## Estimación

**3-5 días.** Las dos incógnitas que la inflaban a 1-2 semanas —GLES 1.1 y
ARMv5TE— resultaron ser ninguna. Lo que queda de trabajo real es el driver
JNI/GLSurfaceView, que es código nuevo pero acotado.
