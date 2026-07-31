# Dead Space — traspaso técnico

**Proyecto, dirección del port, pruebas de dispositivo y eapx: EapRules.**
Las contribuciones posteriores de cada agente se separan abajo para conservar
la atribución técnica sin reemplazar la autoría principal del proyecto.

Estado actual (actualización ChatGPT/Codex, 2026-07-31): **M7 de 7 con el donor
completo**. El verificador inmutable completó 570 frames con consumo de audio
en tiempo real, cargó contenido, procesó 151 uploads de textura, hizo 35.511
draws y demostró cuatro cambios de escena después de 11 inputs JNI sintéticos.
En la R36S real EapRules confirmó imagen centrada, fallback PVRTC, escena 3D,
audio y controles principales. Después se corrigieron cursor recuperable,
cámara continua, acelerómetro en L2/R2 y portada normalizada. La SD contiene
ahora una instalación final deliberadamente limpia: sólo el ZIP libre del port
en `autoinstall/` y el donor público Vita RIP en `ports/deadspace/`. PortMaster
y el primer arranque todavía deben validar ese flujo exacto en el dispositivo;
los detalles y la diferencia respecto del donor completo están en 11.19.

Este documento es para que otro agente continúe sin repetir nada. Lo que está en
`HALLAZGOS.md` es el triage del juego; esto es el estado de la investigación.

Las secciones 1–10 preservan el traspaso histórico escrito durante la sesión de
Claude, cuando el estado todavía era M4/7. La sección 11 separa explícitamente
el trabajo posterior realizado por **ChatGPT/Codex (GPT-5)**, para que otro
agente pueda atribuir cada cambio y no vuelva a investigar el bloqueo viejo.

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

---

## 11. Actualización de ChatGPT/Codex (GPT-5) — M4 → M7

**Autor de esta sección y de los cambios enumerados abajo:** ChatGPT/Codex,
modelo GPT-5, sesión del 30 de julio de 2026. El trabajo de las secciones
anteriores y los commits hasta `4390fa3` fue heredado de la sesión de Claude.

### 11.1 Resultado verificable

Corrida final del árbitro de solo lectura `port/harness/verify.sh`:

```text
[verify] M1 ok
[verify] M2 ok (0 unresolved symbols)
[verify] M3 ok
[verify] M4 ok
[verify] M5 ok (600 frames)
[verify] M6 counters: assets=84 textures=11 draws=36348 nonblack=1
[verify] M6 ok (assets=84 textures=11 draws=36348, survived)
[verify] M7 autopilot: injected 12 keys over 600 frames, scene changed 2 time(s)
[verify] M7 ok
[verify] === milestone reached: 7 / 7 ===
```

La candidata armhf empaquetada después de esa corrida:

```text
commits:
  9045dc0  audio/VFP (commit mixto; ver autoría arriba)
  ffd9348  self-test/cobertura (principalmente sesión concurrente)
  e55e188  atribución, prueba final y passthrough del self-test
  5d2aac8  hash y contenido de la candidata empaquetada
  73fd650  copia verificada, sync y expulsión de la SD

build/deadspace
  7212196 bytes
  SHA256 92b55dfca095dffcb26fa4074547dfb9bec4b7110853fd84ccb337dae5a2078c

build/deadspace-portmaster.zip
  5195229 bytes
  SHA256 39751166aad2b140d49d97297f470bfdfe4a2ee86adc23ab13e93dea035fab7c
```

`unzip -t` validó todos los miembros. El listado contiene únicamente launcher,
loader, metadatos y diez librerías redistribuibles; no contiene
`libEAMGameDeadSpace.so` ni `assets/published`.

El primer intento de despliegue no encontró ningún dispositivo externo. Tras
reinsertar la tarjeta, macOS montó `/dev/disk4s1` (exFAT) en `/Volumes/ROMS`.
Se preservaron `assets/`, `lib/`, `var/`, launcher y GPTK; sólo se actualizaron:

```text
/Volumes/ROMS/ports/deadspace/deadspace
/Volumes/ROMS/ports/deadspace/README.md
/Volumes/ROMS/backup/deadspace-portmaster-M7.zip
```

Los tres destinos coincidieron byte por byte (`cmp=0`) y por SHA-256 con sus
fuentes. Se eliminaron únicamente los cuatro metadatos AppleDouble creados por
macOS:

```text
/Volumes/ROMS/backup/._deadspace-portmaster-M7.zip
/Volumes/ROMS/ports/._deadspace
/Volumes/ROMS/ports/deadspace/._README.md
/Volumes/ROMS/ports/deadspace/._deadspace
```

Después se ejecutó `sync` y el volumen se expulsó con `diskutil eject`.

El harness no fue modificado. También se ejecutó un dry-run del launcher en un
contenedor PortMaster descartable, quitando del sistema las bibliotecas que el
paquete afirma transportar; pasó carga del módulo, resumen final, limpieza de
gptokeyb y código de salida cero.

### 11.2 Cómo se destrabó el frame 2

El vector vacío de `+0x2dd454` no era un allocator roto. Era el resultado de
pedir un provider de VFS para `/published/...` y recibir `NULL`.

La evidencia dinámica mostró estos montajes:

```text
/game/published             -> /var
/game/var                   -> /var
/game/ea/deadspace/         -> /
```

El parser de rutas de VFS (`+0x302920`) salta el slash inicial. El lookup
(`+0x30ba38`) empieza por el primer componente con nombre y sólo selecciona un
provider al encontrar ese nodo. Por eso un provider montado en el nodo raíz
`/` nunca atendía `/published/...`.

Un experimento controlado montó el mismo backend una segunda vez:

```text
source: /game/ea/deadspace/published
mount:  /published
```

Con ese único cambio, los providers dejaron de ser nulos y el run pasó de un
frame a 120/120. El arreglo permanente está en `port/src/patch.cpp`:

- valida que el call site `+0x1c3d7c` contenga `0xeb05265c`;
- reemplaza sólo ese `BL` de startup por un trampoline ARM de dos words;
- el wrapper llama primero al montaje original;
- clona la metadata de cinco words de los strings UTF-16 EASTL;
- llama al provider original `+0x30d6f4` una segunda vez para `/published`.

No se reengancha una rutina global y no se alterna código en runtime.

### 11.3 Bugs JNI/AssetManager encontrados durante la prueba

`port/jni/classes/android_assets.cpp` tenía dos desacuerdos con Android:

1. `AssetManager.list()` devolvía rutas recursivas. Android devuelve sólo los
   nombres del nivel pedido, incluyendo subdirectorios sin slash final. El
   motor concatena el slash él mismo y buscaba literalmente `published`.
2. `String(names[i])` elegía el constructor que toma propiedad de un `char *`.
   Después el caller liberaba ese mismo buffer: ownership doble. Se fuerza el
   constructor copiador con `String((const char *)names[i])`.

`port/jni/jni.cpp` trataba bytes UTF-8 como unidades UTF-16 en `NewString`,
`GetStringLength` y `GetStringChars`. Se agregaron conversiones UTF-8 ↔ UTF-16
reales, incluyendo pares sustitutos. Esto importa en rutas/locales aunque los
primeros nombres ASCII parecieran funcionar.

### 11.4 El crash GL que apareció después de cargar contenido

Una vez arreglado VFS, el crash cambió a:

```text
pc = 0
lr = +0x3244e0
call = glDrawElements@plt
```

El import estaba interceptado por `probe_glDrawElements`, pero el wrapper
reenviaba a la global GLES2 `glad_glDrawElements`, que nunca se inicializa en
este juego fixed-function. La tabla viva era GLES1 (190/190). El tail-call
conservaba el LR del motor, de ahí la forma del crash.

`port/src/symtab_glprobe.cpp` ahora busca `glDrawArrays`,
`glDrawElements` y `glTexImage2D` en `symtable_gles1`, cuenta las llamadas y
reenvía a esos punteros. No se tocó la zona congelada
`thunks/khronos/gles1.cpp`/`gles1_funcs.hpp`.

### 11.5 Métricas de M6

- `port/src/symtab_io.cpp` cuenta únicamente opens exitosos dentro de
  `/assets/published/`; no cuenta probes fallidos, directorios, config ni saves.
- `port/src/symtab_glprobe.cpp` cuenta uploads reales de `glTexImage2D` y draw
  calls, reenviando todos los argumentos sin modificarlos.
- `port/android/fb_probe.cpp` resolvía globals de GLES2 que están a cero. Ahora
  toma las cinco funciones integer-only de la tabla GLES1 viva, incluido
  `glBindFramebufferOES`.
- Este build no llama el `eglSwapBuffers` del shim; `src/main.cpp` es quien
  presenta. La sonda se movió al punto real, antes de `SDL_GL_SwapWindow`.
- Al terminar se imprime:

```text
TRACE: summary assets=N textures=N draws=N
```

### 11.6 Input real y M7

El código heredado de `android/platform.cpp` alimenta `AInputQueue`, pero este
binario no importa `AInputQueue`, `ALooper`, `AKeyEvent` ni `AMotionEvent`.
Enviar eventos ahí nunca podía controlar este juego.

El binario sí exporta:

```text
Java_com_ea_blast_KeyboardAndroid_NativeOnKeyDown
Java_com_ea_blast_KeyboardAndroid_NativeOnKeyUp
Java_com_ea_blast_TouchSurfaceAndroid_NativeOnPointerEvent
```

`port/android/input_bridge.cpp` llama esos exports directamente, con firmas,
keycodes, module IDs y geometría tomados del port de Vita:

- botones/d-pad → `KeyboardAndroid` con module ID 600;
- stick izquierdo → touchscreen virtual, module ID 1000;
- stick derecho → touchpad virtual, module ID 1100.

La función de pointer lleva dos `float`. El juego es softfp y el loader hardfp,
por lo que el typedef usa `pcs("aapcs")`; sin eso el caller pondría las
coordenadas en registros VFP y el juego leería basura desde registros core.

Con `DEADSPACE_AUTOPILOT=1`, el bridge envía una secuencia variada después del
warmup. Antes del swap muestrea una fila central y arma una firma perceptual de
32 buckets RGB. Sólo cuenta una escena cuando hay un cambio visual grande
dentro de la ventana posterior a una tecla; animación fuera de esa ventana no
cuenta. El resumen es:

```text
TRACE: autopilot keys=N scenes=N
```

### 11.7 Empaquetado y dispositivo

El scaffold PortMaster heredado pertenecía por error a otro juego: pedía el APK
OUYA `net.mountainsheep.deadspace`, buscaba `libdeadspace.so` y describía hockey
de Mountain Sheep. No podía iniciar este binario.

ChatGPT/Codex reemplazó los artefactos bajo `port/ports/` para este juego:

- el launcher recibe como argv el directorio extraído;
- exige `assets/EAMCore.ini`, `assets/published/` y
  `lib/armeabi/libEAMGameDeadSpace.so`;
- valida SHA1 antes de aplicar parches por offset;
- crea los alias EGL/GLES1/GLES2/libmali en `/tmp` para el blob 32-bit;
- inicia gptokeyb sólo para el combo de salida;
- documentación y metadata ahora dicen EA/IronMonkey, Xperia Play v1.1.33.

La tarjeta del usuario es `/Volumes/ROMS`, corresponde a SD2 y se monta como
`/roms2`; el launcher usa `/$directory/ports/deadspace`, por lo que sigue el
valor que PortMaster entregue al cambiar entre SD1/SD2.

### 11.8 Archivos modificados por ChatGPT/Codex

```text
port/android/fb_probe.cpp
port/android/input_bridge.cpp
port/android/input_bridge.h
port/jni/classes/android_assets.cpp
port/jni/jni.cpp
port/ports/Dead Space.sh
port/ports/deadspace/README.md
port/ports/deadspace/deadspace.gptk
port/ports/deadspace/gameinfo.xml
port/ports/deadspace/port.json
port/src/main.cpp
port/src/patch.cpp
port/src/symtab.cpp
port/src/symtab_glprobe.cpp
port/src/symtab_io.cpp
```

La zona congelada y `harness/verify.sh` permanecen intactos.

### 11.9 Primer ensayo en R36S y build de diagnóstico

El primer ensayo en hardware real confirmó que el paquete y el loader arrancan
el juego. El usuario vio imagen y pudo cerrarlo normalmente, pero informó dos
problemas:

- la imagen aparece desplazada hacia la izquierda y algunos frames o zonas no
  se ven;
- los botones no permiten avanzar desde la primera pantalla.

El `log.txt` de ese ensayo termina en la salida normal de PortMaster y no
contiene `SIGSEGV` ni otro crash. Ese launcher todavía no exportaba
`LOADER_TRACE=1`, por lo que el log no mostraba la geometría GL ni los eventos
de entrada.

La inspección de ports funcionales en la misma tarjeta aportó dos causas
concretas para el mando:

1. `src/main.cpp` sólo llamaba `SDL_Init(SDL_INIT_VIDEO)`, por lo que el
   subsistema `SDL_INIT_GAMECONTROLLER` no tenía por qué enumerar el gamepad.
2. En esta R36S, rotulada con layout Nintendo, el botón físico A (derecha)
   llega como `SDL_CONTROLLER_BUTTON_B` y el físico B (abajo) como
   `SDL_CONTROLLER_BUTTON_A`. El bridge anterior interpretaba esas letras como
   layout Xbox y por lo tanto A enviaba Back en vez de Accept.

La build instalada después de ese ensayo:

- inicializa `SDL_INIT_GAMECONTROLLER`;
- usa layout Nintendo por defecto, seleccionable mediante
  `DEADSPACE_FACE_LAYOUT`;
- registra cantidad de joysticks/controladores, botones, ejes y el keycode
  Android enviado;
- exporta `LOADER_TRACE=1` desde el launcher;
- registra tamaño lógico/drawable de la ventana y los primeros cambios de
  `glViewport` y `glScissor`.

Las sondas de geometría sólo observan y reenvían a la tabla GLES1 viva; no
cambian viewport, scissor ni escala. Por lo tanto, el problema de pantalla
queda **diagnosticado pero aún no corregido** hasta obtener el próximo
`log.txt` del dispositivo.

Esta build volvió a pasar el harness inmutable en M7/7:

```text
[verify] M6 ok: assets=84 textures=11 draws=35670 nonblack=1
[verify] M7 autopilot: injected 12 keys over 600 frames, scene changed 2 time(s)
[verify] M7 ok
```

El binario de diagnóstico instalado directamente en la SD tiene SHA256:

```text
9b8d8ad942c7027014d31cf881388c1fe9ab771ccc3c4ee3292c94afa53d83db
```

### 11.10 Segundo ensayo: causa de geometría y menús táctiles

El log completo del segundo ensayo en R36S dio la evidencia que faltaba:

```text
TRACE: window geometry: logical=640x480 drawable=640x480
TRACE: DisplayAndroidDelegate constructed (640x480 @ 229 dpi)
TRACE: GL viewport: x=0 y=0 width=480 height=640
TRACE: controller: GO-Super Gamepad
TRACE: input bridge: JNI keys=yes pointer=yes joysticks=1 controllers=1
```

No hubo crash y el motor llegó al menos a 4900 frames. El mando abrió
correctamente y todos los botones ensayados produjeron eventos SDL. El botón
físico A llegó como SDL button 1 y fue traducido a Android key 23, tal como se
esperaba para el layout Nintendo.

#### Geometría

La pantalla desplazada no era un problema del driver Mali ni frames ausentes.
La ventana física era correcta, pero el motor intercambiaba ancho y alto al
crear su viewport.

`DisplayAndroidDelegate.GetDefaultWidth/Height` representa las dimensiones
naturales de un dispositivo Android antes de aplicar orientación. El port de
Vita lo demuestra: para su framebuffer 960x544 informa 544x960. Nuestro
delegado informaba directamente 640x480, por lo que el motor lo rotaba a
480x640.

La corrección está en
`port/jni/classes/ea_DisplayAndroidDelegate.cpp`: se informa 480x640 para que
el motor produzca el viewport físico 640x480. No se fuerza ni intercepta el
viewport; se corrige el dato que lo originaba.

#### Menús

El README y el código del port de Vita confirman que los menús originales no
aceptan navegación de gamepad; en Vita se operan con la pantalla táctil. Por
eso recibir correctamente Android key 23 no bastaba para avanzar.

`port/android/input_bridge.cpp` y `port/android/cursor_draw.cpp` implementan el
equivalente PortMaster para una consola sin táctil:

- el cursor empieza visible en el centro;
- la cruceta lo mueve;
- A llama `TouchSurfaceAndroid.NativeOnPointerEvent` con down/up;
- L3 muestra u oculta el cursor;
- mover cualquiera de los sticks lo oculta y entrega los sticks al control
  táctil/touchpad del gameplay.

El cursor se dibuja sobre el framebuffer con GLES 1.1 puro mediante
`glScissor` + `glClear`. Guarda y restaura framebuffer, scissor, clear color y
color mask; no usa shaders GLES2. La sonda M6 se ejecuta antes del overlay para
que el cursor no pueda convertir una pantalla negra en un falso positivo.

Con el cursor visible durante los 600 frames, el harness inmutable volvió a
pasar M7/7:

```text
[verify] M6 ok (assets=84 textures=11 draws=35721, survived)
[verify] M7 autopilot: injected 12 keys over 600 frames, scene changed 3 time(s)
[verify] M7 ok
[verify] === milestone reached: 7 / 7 ===
```

### 11.11 Tercer ensayo: pantalla/input resueltos, render 3D roto

Resultado informado por el usuario con la build `d4ca229` en la R36S:

- la imagen ahora ocupa correctamente la pantalla y está centrada;
- el cursor software aparece y la cruceta lo mueve;
- A/Start permiten avanzar y los pads responden;
- los menús y sus botones se ven;
- el cursor actual es una cruz funcional pero visualmente provisoria, y puede
  desaparecer al pasar entre cursor y controles de gameplay;
- el contenido 3D está roto: fondos y objetos/personajes aparecen casi
  completamente blancos, a veces sólo se distingue una línea, borde, sombra o
  silueta;
- el audio no funciona.

Esto confirma en hardware real las dos correcciones del ensayo anterior:
orientación natural 480x640 → viewport físico 640x480, y menús táctiles
operables mediante cursor. También separa el próximo problema: ya no es
geometría de ventana ni falta de input; es el camino de render de materiales,
texturas, iluminación o estado GLES1 sobre Mali.

La UI visible con el mundo 3D blanco acota la investigación. El proceso no
está simplemente presentando un framebuffer vacío, y el motor sí carga assets,
sube texturas y emite draws. Hay que comparar estado y errores GL entre qemu y
el driver Mali real, especialmente formatos de textura/extensiones y las
llamadas de fixed-function que afectan textura, color, iluminación, blending y
matrices.

El audio se deja deliberadamente para el final. Ya estaba documentada una
divergencia ARM importante en ese bloque: el binario usa short vectors VFP que
qemu emula y el Cortex-A35 de la R36S trata como RAZ/WI. No mezclar esa deuda
con el fallo visual evita abrir dos frentes de bajo nivel a la vez.

### 11.12 Emulador interactivo y MCP local

Se agregó un camino de desarrollo separado del árbitro:
`port/harness/verify.sh` permanece inmutable y sigue calificando M1-M7; el
nuevo `port/emulator/` mantiene qemu-arm + Mesa vivo para inspección y control.

Componentes:

- `android/emulator_control.cpp/.h`: canal opcional activado únicamente por
  `DEADSPACE_CONTROL_DIR`;
- `emulator/run.sh`: construye/inicia qemu con un directorio compartido;
- `emulator/send.sh`: cliente manual validado;
- `emulator/mcp_server.py`: servidor MCP stdio sin dependencias externas;
- `emulator/smoke_test_mcp.py`: prueba integral real;
- `emulator/README.md`: protocolo y uso.

El canal es append-only. El host escribe una línea en `commands` y el loader
consume como máximo una por frame. Soporta posición absoluta de cursor,
down/up táctil, botones, captura y salida. Consumir una sola línea por frame
impide que un click down/up se colapse en un evento de duración cero.

Las capturas no son screenshots sintéticos del host: el loader lee el default
framebuffer actual mediante `glReadPixels`, lo invierte verticalmente y escribe
un PNG RGBA válido con zlib. Estado, frame y ruta de la captura vuelven por
`status.json`.

El MCP publica ocho herramientas:

```text
start_emulator
stop_emulator
emulator_status
move_cursor
click
press_control
capture_screen
read_emulator_log
```

La prueba integral ejecutó `initialize` → start/rebuild → captura PNG 640x480
→ stop y terminó con:

```text
MCP smoke test PASS: initialize, start, PNG capture, stop
```

Lo más importante es que la captura local tardía reprodujo exactamente el
reporte de la R36S: título y botones de menú correctos, fondo/modelo 3D blanco
con apenas bordes y sombras. Por lo tanto el defecto visual ya se puede
investigar sin copiar ni expulsar la SD en cada iteración.

La misma prueba encontró un bug aislado del cursor. `symtable_gles1` contiene
thunks softfp para llamadas que vienen del juego. El overlay host hardfp estaba
llamando `glClearColor(float...)` a través de uno de esos thunks, perdiendo tres
argumentos y produciendo la cruz roja observada en la primera captura. El
cursor y el capturador ahora resuelven punteros crudos con
`SDL_GL_GetProcAddress`; una captura posterior muestra la cruz blanca.

### 11.13 Causa y corrección del mundo 3D blanco

El emulador local permitió reproducir exactamente el defecto visto en la
R36S. La primera versión del probe sólo podía decir que al llegar a
`glReadPixels` ya había un error GL pendiente. Para atribuirlo se agregó un
modo opt-in, `DEADSPACE_GL_DIAG=1`, que hace pasar todas las entradas GLES por
su thunk tipado y drena `glGetError` inmediatamente antes y después de cada
llamada. No se activa durante una ejecución normal.

El resultado fue inequívoco:

```text
GLDIAG: produced by glCompressedTexImage2D: 0x0500
```

`0x0500` es `GL_INVALID_ENUM`. Los argumentos mostraron cadenas completas de
mipmaps con estos dos formatos:

```text
0x8c00 = GL_COMPRESSED_RGB_PVRTC_4BPPV1_IMG
0x8c02 = GL_COMPRESSED_RGBA_PVRTC_4BPPV1_IMG
```

Por ejemplo, el primer material intenta subir una textura RGBA PVRTC1 de
1024x1024 y 524288 bytes, seguida por sus niveles hasta 1x1. PVRTC es el
formato propietario de PowerVR para el que fueron preparados los assets de
este port del juego. llvmpipe/Mesa no lo acepta y la Mali-G31 tampoco; por eso
la UI podía dibujarse mientras las texturas del mundo 3D eran rechazadas y los
modelos quedaban blancos con sólo geometría, bordes y sombras.

La corrección intercepta `glCompressedTexImage2D`:

- si el driver anuncia `GL_IMG_texture_compression_pvrtc`, conserva el upload
  comprimido nativo;
- si no lo anuncia, decodifica PVRTC1 RGB/RGBA de 2 o 4 bpp a RGBA8888 y lo
  sube mediante `glTexImage2D`;
- para el formato RGB fuerza alpha 255;
- valida el tamaño comprimido mínimo y los desbordes antes de reservar memoria.

El decoder se vendorizó sin modificaciones desde el SDK oficial PowerVR de
Imagination Technologies, commit
`2b1bf2f14d3365d0bb801e2a6a131a319d3a2e48`, con su licencia MIT en
`port/third_party/powervr/`.

La captura local posterior muestra el menú completo: pasillo, tuberías,
pantallas, luces, transparencias y materiales correctos. Ya no aparece el
fondo blanco. El framebuffer probe tampoco reporta el sufijo de error GL
pendiente.

El harness inmutable volvió a pasar:

```text
[verify] M5 ok (570 frames)
[verify] M6 counters: assets=84 textures=151 draws=35049 nonblack=1
[verify] M6 ok (assets=84 textures=151 draws=35049, survived)
[verify] M7 autopilot: injected 11 keys over 570 frames, scene changed 3 time(s)
[verify] M7 ok
[verify] === milestone reached: 7 / 7 ===
```

El aumento de 11 a 151 uploads no es una regresión: antes el contador sólo
veía `glTexImage2D`; ahora también cuenta los niveles PVRTC que atraviesan el
fallback.

La build candidata fue copiada y sincronizada en:

```text
/Volumes/ROMS/ports/deadspace/deadspace
SHA256 9199544a9db9113e20facac61fb518dfc892beff35f17156ff3e313924a015da
7160896 bytes
```

También se actualizó el README instalado, se eliminó únicamente el archivo de
metadatos AppleDouble `._deadspace` creado por macOS y el volumen
`/Volumes/ROMS` fue expulsado correctamente con `diskutil eject`.

Estado al cerrar esa etapa: render correcto en la reproducción local y M7/7;
la confirmación posterior sobre la Mali-G31 real queda registrada abajo. Audio
continuaba deliberadamente postergado.

### 11.14 Confirmación PVRTC en hardware y corrección de los controles

El usuario probó en la R36S real la build cuyo binario tenía SHA-256
`9199544a9db9113e20facac61fb518dfc892beff35f17156ff3e313924a015da`.
Confirmó que el mundo 3D ahora se ve correctamente: personajes, objetos,
fondos, luces y texturas dejaron de aparecer blancos. Esto cierra la hipótesis
PVRTC también sobre la Mali-G31 real; no era una particularidad de llvmpipe.

La misma prueba dejó tres observaciones más acotadas:

1. el cursor funcional seguía siendo la cruz provisoria;
2. mover un stick ocultaba el cursor y en ese dispositivo no había una forma
   evidente de recuperarlo;
3. el stick izquierdo producía movimiento continuo, pero el derecho sólo
   giraba una etapa por cada nueva deflexión: mantenerlo a un lado no sostenía
   el giro;
4. todavía no había audio.

La causa del punto 3 estaba en `android/input_bridge.cpp`. `update_sticks()` se
ejecutaba únicamente al recibir `SDL_CONTROLLERAXISMOTION`. SDL emite ese
evento cuando cambia el valor del eje, no durante cada frame que permanece
mantenido. Además, el código trataba el touchpad derecho como un único arrastre
largo. El port de Vita de referencia hace otra cosa: en cada poll termina el
gesto derecho anterior y emite un nuevo `DOWN` en el origen más un `MOVE` a la
posición del stick. El juego consume cada uno como un desplazamiento de cámara
finito.

La corrección de ChatGPT/Codex:

- llama `update_sticks()` una vez por frame desde `android_input_tick()`;
- conserva el stick izquierdo como un toque sostenido con `MOVE` por frame;
- traduce el derecho a `UP → DOWN → MOVE` por frame, igual que Vita;
- registra al terminar cuántos pulsos de frame produjo el gesto para que una
  prueba pueda distinguir continuidad real de un único evento;
- reemplaza la cruz por una flecha pixel-art negra/celeste dibujada con el
  mismo backend GLES1/scissor y restauración completa de estado;
- permite mostrar/ocultar la flecha con L3 o R3;
- si el cursor estaba oculto, Start lo restaura al centro y a la vez conserva
  el key event Start que abre el menú;
- agrega `stick left|right X Y` al canal del emulador y `set_stick` al MCP para
  poder mantener un eje estable sin joystick físico.

La validación local capturó el framebuffer tanto con el cursor oculto por un
stick como restaurado por Start. La captura restaurada muestra la flecha sobre
el menú 3D ya texturado. En una segunda ejecución, mantener el stick derecho
durante dos segundos produjo **77 pulsos consecutivos de frame** antes del
release; ya no depende de nuevos eventos de eje. Esta candidata todavía
requiere la prueba de controles en la R36S.

### 11.15 Audio: SDL sin inicializar y short vectors VFP en ARMv8

**Autoría:** ChatGPT/Codex implementó la inicialización/salida SDL y el
expansor VFP. Una sesión concurrente agregó el digest PCM, el escape hatch
`DEADSPACE_NO_VFP_PATCH`, el self-test de registros y la auditoría independiente
de cobertura; se conservaron porque hacen falsable el parche. La atribución a
VFPVector corresponde a su autor original, Bythos14, y su licencia MIT se
incluye en `port/third_party/vfpvector/`.

Los hashes no separan esa autoría de forma perfecta porque ambas sesiones
compartían el mismo working tree:

- `9045dc0` es **mixto**. ChatGPT/Codex hizo la inicialización SDL, selección de
  device, período/back-pressure, validaciones y el expansor VFP base. La otra
  sesión ya había dejado dentro de los mismos archivos el self-test VFP y el
  cambio que mide PCM antes de abandonar por `deviceId == 0`; al commitear se
  incluyeron juntos.
- `ffd9348` es principalmente de la sesión concurrente: agrega
  `analysis/vfp_coverage.py` y la explicación en `HALLAZGOS.md`. La enmienda de
  ChatGPT/Codex sólo inicializa defensivamente la variable local
  `instruction` del encoder.

No se reescribió el historial para separarlos: estos puntos son la fuente de
verdad de atribución y permiten conservar exactamente el estado que pasó las
pruebas.

El primer defecto era independiente de la aritmética del mixer y explica por
sí solo que no hubiera ningún sonido. `src/main.cpp` hacía:

```text
SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER)
```

pero el constructor falso de `android/media/AudioTrack` llamaba directamente
`SDL_OpenAudioDevice`. Sin `SDL_INIT_AUDIO`, SDL devuelve device ID 0. El shim
no comprobaba ni registraba ese resultado: `EAAudioCore init done` sólo
demostraba que el mixer había arrancado, no que existiera una salida.

La corrección en `jni/classes/media_AudioTrack.cpp`:

- inicializa explícitamente `SDL_INIT_AUDIO`;
- registra driver, endpoints y formato solicitado/obtenido;
- prueba el endpoint por defecto y, si falla, endpoints enumerados, priorizando
  los que no sean HDMI y reintentando temporalmente cuando ALSA informa busy;
- permite forzar un nombre SDL con `DEADSPACE_AUDIODEV`;
- valida tipo y rango del `short[]` antes de leerlo;
- informa señal, pico, media cuadrática y digest FNV-1a en checkpoints
  acotados;
- devuelve back-pressure cuando hay más de dos períodos en cola, en lugar de
  esperar que la cola llegue a cero después de cada bloque.

También había un segundo desacuerdo: `bufferSizeInBytes=1048576` describe el
ring productor del engine, pero se usaba como período físico. Para PCM16
stereo eso solicitaba **262.144 frames**, casi seis segundos a 44.1 kHz. La
salida ahora pide un período convencional de 1.024 frames.

El tercer problema sólo se manifiesta correctamente en el hardware. El
binario armeabi usa el antiguo modo short-vector de VFP: escribe FPSCR
LEN=3/LEN=7 y ejecuta 40 operaciones F32 en 20 regiones del mixer. qemu-arm
todavía implementa esa semántica. El Cortex-A35 ARMv8 de la R36S trata
LEN/STRIDE como RAZ/WI y ejecutaría únicamente lane 0, dejando el resto del
vector sin calcular.

`src/vfp_vector_patch.cpp` contiene la lista exacta de 40 offsets y opcode
esperado. Antes de tocar el binario valida cada palabra y sólo acepta VMOV,
VADD, VSUB, VMUL y VMLA F32. Cada operación se reemplaza por un branch
condicional a un trampoline A32 cercano que:

1. preserva `r4`, `r5` y FPSCR;
2. limpia LEN/STRIDE;
3. emite cuatro u ocho operaciones escalares con wrap por banco VFP;
4. restaura FPSCR y registros;
5. vuelve a la instrucción siguiente.

El decoder/generador se adaptó de Bythos14/VFPVector, commit `d95ba13`
(`Fix mis-identification of VABS and VSQRT`), MIT. No se copió su manejador de
excepciones de Vita: Linux/ARMv8 acepta silenciosamente estas instrucciones,
por lo que este loader aplica una lista eager y específica para el SHA1 fijado.

Verificación local en dos direcciones:

- `DEADSPACE_VFP_SELFTEST=1` arma stubs ejecutables y corre cada opcode
  short-vector original y su expansión escalar con los mismos 32 registros;
  exige igualdad registro por registro y además verifica que qemu realmente
  vectorizó el lado de referencia: **40/40 exactas**;
- `python3 port/analysis/vfp_coverage.py` reconstruye las 20 regiones LEN desde
  `dis/all.dis`: encontró 40 operaciones aritméticas, 40 cubiertas, 0 faltantes,
  0 entradas fuera de región y todas las palabras exactas;
- el dummy SDL abrió exactamente a 44.100 Hz, 2 canales, S16 y período 1.024.

El menú local produjo silencio durante los primeros checkpoints; los `.sps` de
música no se abren hasta avanzar al juego. Los digests con/sin patch coinciden
durante esos ceros, pero luego dependen del tiempo de juego y no son un oráculo
aritmético reproducible. Para eso se usa el self-test de registros anterior.
Lo demostrado en el camino SDL es que el device abre y la cola se consume a
tiempo real. La salida por el parlante ALSA real queda como prueba de hardware.

Corrida final del árbitro inmutable con estos cambios:

```text
[verify] M2 ok (0 unresolved symbols)
[verify] M3 ok
[verify] M4 ok
[verify] M5 ok (375 frames)
[verify] M6 counters: assets=84 textures=168 draws=22009 nonblack=1
[verify] M6 ok (assets=84 textures=168 draws=22009, survived)
[verify] M7 autopilot: injected 7 keys over 375 frames, scene changed 2 time(s)
[verify] M7 ok
[verify] === milestone reached: 7 / 7 ===
```

### 11.16 Release independiente con autoinstall y donor automático

**Objetivo:** repetir el modelo ya publicado para Ice Rage y Minigore 2, no
presentar todavía un PR al catálogo oficial. El usuario deja el ZIP libre del
port en `PortMaster/autoinstall/`, deja su donor en `ports/deadspace/`, abre
PortMaster, reinicia y lanza el juego. El port no descarga ni distribuye datos
de EA.

**Autoría:** `eapx` fue diseñado y escrito por **EapRules** en
`Projects/Others/eapx`. ChatGPT/Codex auditó la herramienta, vendorizó una copia
sin cambios en `port/tools/eapx.py` e implementó para Dead Space la receta, la
integración del launcher, los tests de extremo a extremo, el empaquetado, las
licencias y la documentación pública. No atribuir el motor de eapx a Codex.

La herramienta encaja exactamente con este donor porque identifica APK, ZIP,
XAPK/splits o carpetas por contenido y no por nombre, extrae a un stage dentro
del mismo filesystem, valida y recién entonces publica por rename. Antes de
integrarla se ejecutó su suite completa: **46 tests, 46 OK**, incluyendo cortes
en cada paso del commit, rollback interrumpido, archivos ajenos, ZIP Slip,
APKs fat, XAPK/splits y carpetas ya extraídas.

La receta `ports/deadspace/deadspace.eapx.json` fija:

```text
lib/armeabi/libEAMGameDeadSpace.so
size   4499468
SHA1   0ed42b611415015807f759ec9b5457857143ce39
SHA256 1ff0a34a2ef35f50c0031b383d8fd7b14369dca0f41969c5d4da8fabe569ed38
```

Acepta cuatro raíces conocidas sin que el usuario reacomode el archivo:

```text
assets/...
deadspace/assets/...
data/deadspace/assets/...
NeededFiles/data/deadspace/assets/...
```

También valida el `EAMCore.ini` exacto y exige al menos 1.700 archivos y
300.000.000 bytes bajo `assets/published`. Un test con un ZIP real cuya raíz es
`deadspace/` produjo un plan de **1.771 items / 303,0 MiB**, lo extrajo,
confirmó el SHA1, verificó nuevamente los 1.771 items y dejó marker válido.

El launcher sólo invoca eapx cuando falta el árbol utilizable, de modo que las
instalaciones manuales existentes siguen funcionando. Si falta Python 3, la
receta o el runtime, muestra un error en la TTY; un fallo del donor deja el
detalle en `eapx.log`. Tras una importación exitosa conserva el chequeo SHA1
del launcher antes de aplicar cualquier parche por offset.

`package_portmaster.sh` ahora genera `build/deadspace.zip`, incluye eapx,
receta, placeholder, cover 4:3 aportada por el usuario, captura real 640x480 y
las licencias del port/gmloader/PowerVR/VFPVector. `collect_libs.sh` resuelve el
paquete Debian dueño de cada una de las diez `.so` transportadas y copia su
copyright exacto; el empaquetado falla si falta uno. Sigue abortando si el ZIP
contiene la biblioteca o los assets del juego.

La prueba integral ejecutó el launcher real dentro del contenedor, con el donor
copiado bajo un nombre sin significado y **176 bibliotecas armhf del sistema
apartadas** para impedir falsos positivos. Resultado: eapx escribió marker y
log, importó biblioteca/assets, el módulo cargó, dibujó hasta su resumen,
`pm_finish` corrió, gptokeyb no quedó vivo y el launcher salió 0: **PASS**.

El informe externo que señalaba `/game/ea/deadspace/` vacío y la deuda de VFP
era correcto para una etapa vieja, pero no describe el HEAD actual. La ruta se
resolvió en `32923c5` y el log demuestra
`/game/ea/deadspace/published -> /game/assets/published (ok)`; los 40
short-vectors se resolvieron en `9045dc0`/`ffd9348`. No se reabrieron ni se
modificaron esos arreglos durante el trabajo del release.

Artefacto independiente final, libre de contenido de EA:

```text
port/build/deadspace.zip
size   8016847 bytes
SHA256 784a9348b8a99638ab55323c01c8dae88de3309acd9699b9399a255c92f52eb5
```

La última corrida del verificador inmutable posterior al empaquetado alcanzó
M7/7. La corrida posterior del cambio de acelerómetro se registra en 11.17.

### 11.17 Sustitución del acelerómetro con L2/R2

La prueba de juego real de EapRules confirmó que render, audio y controles
principales funcionan, y encontró una interacción no bloqueante que luego se
vuelve obligatoria: el juego pide inclinar el teléfono para rotar/cambiar el
modo del arma. El mismo sensor se usa más adelante para saltar en Zero-G.

El camino activo `android/input_bridge.cpp` no procesaba
`SDL_CONTROLLER_AXIS_TRIGGERLEFT/RIGHT`; L2 y R2 estaban completamente libres.
El shim declaraba un acelerómetro, pero `AccelerometerAndroidDelegate` sólo
aceptaba `SetEnabled` y descartaba la operación. El binario sí exporta:

```text
Java_com_ea_blast_AccelerometerAndroidDelegate_NativeOnAcceleration
signature Java: (FFF)V
ABI del juego: armeabi softfp / pcs("aapcs")
```

El `classes.dex` decompilado confirma que Android le pasa directamente
`SensorEvent.values[0..2]`. Como referencia primaria se auditó
`v-atamanenko/deadspace-vita` (`loader/reimpl/controls.c`): su opción sin sensor
reproduce una secuencia medida y marca de forma explícita el comienzo de las
muestras para Zero-G. Se conservaron esos valores y su cadencia, separados en:

- **L2:** 33 muestras de inclinación para rotar/cambiar el modo del arma;
- **R2:** 27 muestras de movimiento brusco para saltar en Zero-G.

Cada gesto se dispara una vez en el flanco de presión del gatillo; mantenerlo
no repite acciones y hay que soltarlo antes de volver a usarlo. La llamada usa
un puntero `pcs("aapcs")`, necesario porque los tres floats del juego siguen
softfp mientras el loader se compila hardfp. El emulador/MCP y `send.sh` también
aceptan ahora `l2` y `r2`.

Prueba dinámica bajo qemu con el juego real:

```text
input bridge: JNI keys=yes pointer=yes acceleration=yes
input: weapon-tilt/L2 accelerometer gesture started (33 samples)
input: weapon-tilt/L2 accelerometer gesture completed
input: zero-g/R2 accelerometer gesture started (27 samples)
input: zero-g/R2 accelerometer gesture completed
run finished: 369 frames
```

El motor siguió dibujando sin crash. Después, el verificador inmutable completo
volvió a pasar:

```text
[verify] M5 ok (570 frames)
[verify] M6 counters: assets=84 textures=151 draws=35511 nonblack=1
[verify] M7 autopilot: injected 11 keys over 570 frames, scene changed 4 time(s)
[verify] === milestone reached: 7 / 7 ===
```

La secuencia y su licencia MIT se atribuyen a deadspace-vita en `NOTICE.md`,
`CREDITS.md` y `third_party/deadspace-vita/LICENSE`. Falta confirmar ambos
gatillos en la R36S: L2 durante el tutorial de modo de arma y R2 en una zona
Zero-G.

La build `fa8db99` se instaló después directamente sobre la SD de prueba en
`/Volumes/ROMS/ports/`, usando el ZIP completo para validar el mismo contenido
que recibirá el usuario final. La copia conservó `assets/`, `lib/` y `var/`; el
SHA1 del donor siguió siendo el esperado y el binario instalado se verificó
contra el build local:

```text
SHA256 d0ba9983a13a1cf7dbd7a7c5c26d57d544cd9e435ffe80308a03799ea20390de
size   7215740 bytes
```

Se eliminaron exclusivamente los AppleDouble `._*` creados por macOS durante
la copia antes de sincronizar y expulsar el volumen.

### 11.18 Portada normalizada de EmulationStation

La prueba posterior en consola mostró que el juego seguía usando un icono viejo
del APK en la lista de Ports aunque el release ya llevaba la composición 4:3
aportada por EapRules. La portada canónica estaba bien ubicada y el
`gameinfo.xml` ya seguía el formato oficial de PortMaster:

```xml
<image>./deadspace/cover.png</image>
```

La causa era específica del estado existente de ArkOS/dArkOS: EmulationStation
tenía normalizada otra copia en `ports/images/Dead Space.png` y el update
directo por ZIP no volvió a ejecutar el importador de metadata de PortMaster.
Por eso reemplazar `deadspace/cover.png` no cambiaba lo que el frontend tenía
referenciado y cacheado.

El launcher ahora compara ambas copias en cada inicio y, sólo si difieren,
actualiza `ports/images/Dead Space.png` desde la portada canónica. No parsea ni
reescribe `gamelist.xml`, no toca imágenes de otros ports y por lo tanto no
arriesga favoritos, `playcount` ni `lastplayed`. EmulationStation puede mantener
la textura vieja en RAM hasta reiniciar el frontend o la consola.

El dry-run integral se amplió para instalar `cover.png` y exigir igualdad byte
por byte con la copia normalizada. Volvió a pasar con 176 bibliotecas armhf del
sistema apartadas, importación eapx real, arranque del juego y salida limpia:

```text
[dryrun] ok:   ArkOS artwork normalized from canonical cover
[dryrun] === PASS ===
```

Al cerrar 11.18 este arreglo estaba empaquetado pero todavía no se había copiado
a la SD porque el volumen había sido expulsado al cerrar 11.17. La instalación
limpia posterior de 11.19 ya incorpora ese launcher y su portada normalizada.

### 11.19 Instalación limpia y soporte dual Complete/Vita RIP

La prueba de publicación dejó claro que un APK no es un requisito técnico. El
port necesita un donor que contenga la biblioteca ARM exacta y un árbol de
assets admitido; puede llegar como APK, ZIP o carpeta extraída. Para reproducir
el recorrido de un usuario real se investigaron estos dos ítems públicos:

```text
dead space vita 1.1.33
https://archive.org/details/deadspace_202504

Dead Space Mobile Version 1.1.33
https://archive.org/details/dead-space-mobile-ps-vita-1.1.33
```

Los dos ofrecen exactamente el mismo `deadspace.zip`: 145.903.794 bytes, SHA1
`61d51d8ba5374f97b5a4971a2d9d7da31baf840c`. El segundo ítem agrega un VPK de
Vita que el port de Linux no usa. Por lo tanto no son dos donors distintos y
ninguno contiene la variante completa.

Desde ahora la documentación distingue tres artefactos para evitar que el
nombre repetido `deadspace.zip` induzca a sobrescribir uno con otro:

```text
Port ZIP       deadspace-portmaster.zip   libre, sin contenido de EA
Complete donor árbol Xperia Play completo, 303 MiB importados
Vita RIP donor deadspace.zip público, 247 MiB importados
```

La comparación por path encontró 25 archivos presentes sólo en Complete: los
nueve paquetes UI `~2x`, cuatro mapas Survival, diez archivos de modelo/textura
del Burst Rifle, `EurostileLTStd.ttf` y el mapa de prueba `00_test_respawn`.
Vita RIP conserva los 103 MiB de sonidos y los archivos de la campaña, aunque
algunos mapas, modelos, partículas, texturas y `level_index.sb` son variantes
distintas. No se supone que esa diferencia sea invisible: a 640x480 la ausencia
de UI `~2x` probablemente tenga poco impacto, pero Survival y Burst Rifle sí son
contenido ausente y la apariencia sólo puede cerrarse con la prueba real.

La receta eapx subió a v2. No se limitó a bajar el umbral: admite ambos tamaños
y agrega hashes aceptados explícitos para `level_index.sb`, layouts, primer mapa
de campaña, modelo del menú, frontend UI y base de audio, además de conservar el
SHA256 exacto de la biblioteca y de `EAMCore.ini`. Resultados de planificación:

```text
Complete donor  1.771 items  317.682.271 bytes
Vita RIP donor  1.746 items  259.247.380 bytes
```

El ZIP público se extrajo completo con eapx a un stage descartable, se publicó
transaccionalmente y sus 1.746 items se verificaron después del commit. Bajo el
harness llegó a M6/7: cargó el módulo, dibujó 600 frames, abrió 147 assets, hizo
515 uploads de textura y 2.444 draws con framebuffer no negro. M7 registró cero
cambios de escena porque el piloto automático no navegó el frontend variante;
eso no se convirtió en un PASS artificial y queda como diferencia pendiente de
la prueba de controles en hardware.

El dry-run del launcher PortMaster real sí pasó de extremo a extremo con Vita
RIP: importó biblioteca/assets, escribió marker y log, normalizó portada, cargó
el juego, dibujó hasta su resumen, ejecutó `pm_finish` y no dejó gptokeyb vivo.
El paquete libre resultante no contiene la biblioteca ni `assets/published`:

```text
build/deadspace.zip
SHA256 7be3419bb45d22bcdaa7eb8b5fe615092d6dbd3cf67976d25dae2cfce267b762
```

Para ensayar una instalación nueva se borró exclusivamente el estado instalado
de Dead Space de la SD: directorio completo (incluidos saves), launcher, portada
global vieja y las dos entradas activas de `gamelist.xml`. El script
`port/tools/clean_deadspace_gamelist.py` exige encontrar exactamente los paths
`./Dead Space.sh` y `./deadspace/grab_screen.sh`; la limpieza pasó de 21 a 19
entradas y no tocó ningún otro juego ni los backups históricos del gamelist.

El estado preinstalación que se entrega a la consola contiene únicamente:

```text
/roms/tools/PortMaster/autoinstall/deadspace-portmaster.zip  # nuestro port
/roms/ports/deadspace/deadspace.zip                          # Vita RIP donor
```

Ambos ZIP pasaron `unzip -t`; el donor conserva internamente la biblioteca SHA1
`0ed42b611415015807f759ec9b5457857143ce39`. No hay launcher, assets extraídos,
`var/`, portada global ni entrada de menú antes de abrir PortMaster. La prueba
pendiente es abrir PortMaster, dejar que consuma el ZIP de autoinstall, reiniciar
y lanzar el juego: el primer arranque debe importar Vita RIP y permitir navegar
manualmente el frontend que M7 automático no pudo atravesar.
