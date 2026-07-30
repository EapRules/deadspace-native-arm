# Dead Space — traspaso técnico

Estado al momento de escribir esto: **M4 de 7**. El motor carga su contenido,
dibuja **un frame completo**, entra al segundo y no vuelve.

Este documento es para que otro agente continúe sin repetir nada. Lo que está en
`HALLAZGOS.md` es el triage del juego; esto es el estado de la investigación.

---

## 1. Qué es este port y en qué se diferencia de los anteriores

`libEAMGameDeadSpace.so`, sha1 `0ed42b611415015807f759ec9b5457857143ce39`
(Xperia Play v1.1.33). ARMv5TE + VFPv2, **softfp**, compilado con un NDK de 2011.
Se mapea con el loader de bionic y se le falsifica Android alrededor. Sin
emulador.

Los dos ports anteriores (Minigore 2, Ice Rage) eran **NativeActivity**: el motor
importaba `libandroid.so` y corría su propio bucle. **Dead Space no.** No importa
libandroid, exporta `JNI_OnLoad` y 68 `Java_*`, y espera que una capa Java lo
maneje frame a frame. **Esa capa somos nosotros** — `src/main.cpp` es el driver.

Consecuencia: `android/platform.cpp`, `android/asset_manager.cpp` y
`android/opensles.cpp` heredados **no se usan**. El audio va por JNI contra un
`AudioTrack` falso; los assets salen del filesystem.

### Secuencia de arranque (copiada del port de Vita, no deducida)

```
JNI_OnLoad(vm, NULL)
EAAudioCore Init(env, 0x42424242, AudioTrack real, 1MB, 2 canales, 44100)
Java_com_ea_blast_MainActivity_NativeOnCreate()
Java_com_ea_blast_AndroidRenderer_NativeOnSurfaceCreated()
Java_com_ea_blast_KeyboardAndroid_NativeOnVisibilityChanged(env, 0x42424242, 600, 1)
loop: Java_com_ea_blast_AndroidRenderer_NativeOnDrawFrame(); SDL_GL_SwapWindow()
```

Dos trampas: el punto de entrada es **`NativeOnCreate`**, no `runEntryPoint`
(existe, el nombre es tentador, el port que funciona no lo llama nunca). Y
`NativeOnSurfaceChanged` no se llama: la resolución se toma una sola vez.

---

## 2. El estado exacto, hoy

```
M1 compila                        OK
M2 0 símbolos sin resolver de 393 OK
M3 JNI_OnLoad + NativeOnCreate    OK
M4 superficie GL                  OK
M5 60 frames                      1 frame
```

El log llega hasta acá:

```
TRACE: -> NativeOnDrawFrame #1
TRACE: <- NativeOnDrawFrame #1 returned
TRACE: -> NativeOnDrawFrame #2          <- no hay "returned"
FATAL: SIGSEGV at 0x00000000
```

**El primer frame se completa.** Durante horas esto se leyó como "el motor no
llega a dibujar", porque el contador de frames imprimía cada 10 y una corrida que
hacía uno no imprimía nada. Separar "no entró al frame 2" de "entró y no volvió"
costó tres líneas de traza y cambió el problema entero.

### El crash

```
pc = +0x0030a744    ldr r1, [r3]     con r3 = 0
lr = +0x00257150
sp = 0x407fde08                       <- stack del hilo PRINCIPAL
ip = 0x407fe2a8
[ip] = 0x00000000 0x00000000 0x00000000 ...
```

`ip` es el segundo argumento de la función en `+0x30a54c`, y es un
`std::vector` cuyos tres words (begin, end, capacity) están **en cero**. La
función arma un path en un buffer de 500 bytes recorriendo sus elementos, y
después de manejar el caso vacío al principio, al final lo desreferencia igual.

Hay un segundo sitio, `+0x5f384`, que aparece ~1 de cada 6 corridas y tiene la
misma forma: un contenedor vacío desreferenciado. Ése cae en un **worker**.

---

## 3. La cadena de llamadas, ya desensamblada — no la rehagas

```
+0x30aec0   push {r4,r5,lr}; sub sp,#20
            mov r0,sp ; bl +0x3130dc      <- llena el vector
            mov r1,sp ; bl +0x30a54c      <- lo usa  ** CRASH ADENTRO **
            mov r0,sp ; bl +0x5d1dc       <- lo destruye
```

- `+0x30aec0` tiene **13 llamadores**. El otro candidato, `+0x30ae40`, tiene
  **0** — está muerto, no lo mires.
- `+0x3130dc` lee un global; si es no-null construye un string desde él, si es
  null llama a `+0x2dd9b8` para un default. **Las dos ramas hacen push**
  (`bl +0x2dd454`).
- `+0x2dd454` pide 44 bytes a un allocator (`bl +0x3007bc` = singleton de Meyers,
  después `ldr pc,[ip,#12]` = `allocate`) y **chequea NULL** en `subs r5,r0,#0`.

O sea: el push está guardado por una asignación que puede fallar. **Pero la
memoria no falla** — ver descartados.

---

## 4. Descartado con evidencia. NO lo repitas.

| Hipótesis | Cómo se descartó |
|---|---|
| **Falta de memoria** | `DEADSPACE_TRACE_MMAP=1`: todos los `mmap` del allocator tienen éxito (16 MB, 13 MB, 19 MB…), cero `MAP_FAILED` |
| **Guards de C++** | `DEADSPACE_TRACE_GUARDS=1`: `acquire` devuelve 1, deja `0x100`, `release` deja `0x1` — el bit exacto que el juego testea con `ands #1`. Ningún guard se adquiere dos veces |
| **Clases JNI faltantes** | Las 14 que el juego pide están registradas. `FindClass` y `GetMethodID` imprimen el descriptor pedido; el log no reporta ninguna faltante |
| **`rwfilesystem` en los símbolos** | Ruido: es el símbolo global más cercano, ~310 KB antes. No tiene relación |
| **`data.zip` que falla al abrir** | Benigno. No está en los strings de la `.so`, ni en `EAMCore.ini`, ni en el port de Vita |
| **El asset manager** | El motor abre 8 archivos correctamente por `fopen` tras los parches al binario |

---

## 5. La familia de bugs que domina este port

**Siete bugs arreglados hoy, y todos son el mismo bug de fondo:** *la maquinaria
de shims asume que el llamador tiene las garantías del compilador del host, y el
llamador es la salida de otro compilador de hace catorce años.*

Ninguno se encontró buscándolo. Aparecieron de a uno, por crash, y **ninguno
falla limpio** — todos se ven como corrupción de memoria.

| Bug | Se veía como | Arreglado en |
|---|---|---|
| `va_arg` con tipos < `int` (`jboolean` es `unsigned char`) — UB, gcc emite instrucción indefinida como cuerpo | `SIGILL` adentro del loader. Delator: funciones a 2 bytes una de otra | `jni/jni_internals.h` (`va_promoted_t`) |
| `ldrd` sobre `va_list` del juego (exige alineación 8, el juego alinea a 4) | `SIGBUS` | `jni/jni_internals.h` (`va_next` con barrera) |
| Aridad del despacho: `RegisterNonVirtual` arma 4 params, `CallIntMethod` pasa 3 | Argumentos basura, crash lejos | `jni/jni.cpp` (flag `takes_class`) |
| `pthread_mutex_trylock` directo a glibc sobre el handle de 4 bytes de bionic | Pisa 20 bytes del objeto del juego | `src/symtab_pthread.cpp` |
| `pthread_mutexattr_setpshared` le prende el bit 31 a nuestro puntero | `SIGSEGV` en `0x8xxxxxxx` | idem |
| `pthread_attr_setstack`/`setschedparam` escriben en offsets de glibc sobre layout bionic | Borra el flag DETACHED (fuga de hilos), descarta el stack pedido | idem |
| `pthread_cond_timedwait` con `timespec` de 8 vs 16 bytes | Hilo colgado o girando, sin log | idem |

Más: creación perezosa de condvars sin serializar (el motor **nunca** llama
`pthread_cond_init` — cero relocaciones), y `host_mutex()` con double-checked
locking sin barreras sobre la tabla de 32 mutexes estáticos que EAThread usa para
emular atómicos de 64 bits.

**La regla que sale de esto:** cuando veas un crash que parece corrupción de
memoria, sospechá primero de un desacuerdo de ABI en un shim, no del motor.

---

## 6. Herramientas que existen. Usalas antes de escribir código.

| Variable | Qué hace |
|---|---|
| `DEADSPACE_TRACE_GUARDS=1` | cada `__cxa_guard_acquire/release` con el word del guard |
| `DEADSPACE_TRACE_MMAP=1` | cada `mmap` del allocator con su resultado |
| `DEADSPACE_LATE_THREAD_MS` | atrasa los hilos del motor (default 1500). `0` desactiva |
| `DEADSPACE_LATE_THREAD_AT` | atrasa sólo un entry point, por offset |
| `DEADSPACE_SCREEN_W/H/DPI` | tamaño y dpi reportados al motor |
| `DEADSPACE_FRAME_LIMIT` | corta tras N frames |

Y en el log, siempre:

- cada `pthread_create` con su offset relativo al módulo **y su rango de stack**
- cada `open` con la ruta original y la traducida
- `FindClass` y `GetMethodID` imprimen el **descriptor exacto** que pidió el motor
  (la búsqueda es `strcmp` sobre nombre **y** firma: una firma plausible pero
  equivocada falla igual que una clase ausente)
- reportes de crash con **tid**, registros, **cuatro words de lo que apunta cada
  registro**, y un stack filtrado por "¿el word anterior es una instrucción de
  llamada?" (sin ese filtro imprime constantes de `.rodata` como si fueran frames
  — me hizo perseguir una recursión infinita inexistente)

### Cómo identificar qué hilo revienta

Cruzá el `sp` del crash contra los rangos que loguea `pthread_create`. El motor
arranca **6 hilos**: `+0x24c7f8` (×2), `+0x24c914` (×3, workers de EAThread) y
`+0x360d9c` (×1, `arg=NULL`).

El crash de `+0x5f384` cayó en el **tercer** worker `+0x24c914`.
El de `+0x30a744` cae en el **hilo principal**.

---

## 7. Lo que yo haría ahora

1. **El frame 2 se entra y no vuelve, en el hilo principal.** Esa es la pista
   fresca y no la exploté. ¿Qué hace `NativeOnDrawFrame` la segunda vez que no
   hizo la primera? Probablemente entra a un camino que el frame 1 no toca —
   cargar la escena, o consultar algo que el frame 1 dejó pedido.

2. **El vector vacío tiene tres words en cero, o sea está *construido* y vacío**,
   no es basura. Alguien lo construyó y nadie le hizo push. El push está en
   `+0x2dd454` tras un `allocate` que se chequea contra NULL. La memoria no
   falla, así que **la rama que hace push no se está tomando** — vale instrumentar
   qué global lee `+0x3130dc` y qué valor tiene.

3. **Quedan hallazgos del enjambre sin aplicar**: `getaddrinfo` (bionic invierte
   `ai_canonname` y `ai_addr`), y `__sF` (nuestro `BIONIC_FILE` mide 88 bytes y el
   juego tiene hardcodeado el stride de 84 de bionic, así que `stderr` apunta 8
   bytes antes de donde debería). El segundo importa: si salta un assert del
   juego, no vas a ver el mensaje sino un segfault adentro de `vfprintf`.

---

## 8. Deuda conocida, nada bloquea un hito

- **`GetAppDataDirectory` devuelve el árbol del juego.** Ahí irían los saves y un
  update los pisaría. **Arreglar antes de empaquetar.**
- **`patch.cpp` no verifica el sha1 en runtime.** El harness sí, pero eso no corre
  en la consola: con otro build del juego los cuatro parches caen en direcciones
  arbitrarias. Cada parche valida la instrucción que espera, así que avisa y se
  saltea, pero el gate explícito falta.
- **El atraso de hilos son 9 segundos de arranque.** No es publicable. Sirve para
  reproducibilidad, no para shippear. Acotarlo a un solo hilo con
  `DEADSPACE_LATE_THREAD_AT`.
- **`gmtime` y `utime`** tienen el mismo problema de `time_t` de 64 bits sin
  thunkear. Nadie las llama todavía.
- **Los short vectors de VFP.** 40 escrituras a `FPSCR` con `LEN=3`/`LEN=7`, todas
  en el bloque de audio. **qemu los emula y la R36S no** (RAZ/WI desde VFPv3), o
  sea el harness es *más capaz* que el hardware acá. Verificar el audio en la
  consola antes de darlo por bueno.

---

## 9. Reglas del proyecto

- **`port/harness/verify.sh` es el árbitro y es de solo lectura.** Modificarlo o
  bajar sus umbrales invalida la corrida.
- **Zona congelada:** `port/thunks/khronos/gles1.cpp`, `gles1_funcs.hpp`,
  `port/tools/`. La capa de GL está resuelta y verificada; reportá, no toques.
- **Nunca se distribuye la `.so` ni los assets.** Los parches se aplican en
  memoria, en runtime, sobre la copia del usuario.
- Código, comentarios y commits en **inglés**. Sin trailers de `Co-Authored-By`.
- **Verificá que compile y que el harness no baje de hito antes de commitear.**

---

## 10. La lección de método, si te sirve

Lo más caro de hoy no fue ningún bug: fue **perseguir el crash equivocado durante
horas porque tres posibilidades distintas estaban colapsadas en un mismo
silencio**. "surface created" y después nada podía significar que moría armando
la superficie, que moría en el primer frame, o que moría en nuestro propio bucle.
Tres líneas de traza lo resolvieron.

La pregunta *"¿dónde exactamente se detiene?"* suele ser mucho más barata de
contestar que la pregunta que uno ya está discutiendo.

Lo segundo: **medí, no razones.** Descarté la hipótesis de los guards de C++ con
un wrapper y una corrida, después de leer un comentario que argumentaba muy bien
por qué era seguro. El argumento era correcto. Pero también lo era el de los otros
siete, y esos estaban mal.

Y lo tercero: **muestra chica miente.** Saqué el atraso de hilos con dos corridas
por valor que dieron idéntico. Con seis: sin atraso 5/6 en un sitio y 1/6 en otro.
Tuve que revertirlo.
