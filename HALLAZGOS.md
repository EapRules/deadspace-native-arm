# Dead Space — triage previo al port (2026-07-30)

Todo verificado contra el binario y el firmware, nada supuesto.

## El juego

| | |
|---|---|
| Librería | `libEAMGameDeadSpace.so`, 4.499.468 bytes |
| sha1 | `0ed42b611415015807f759ec9b5457857143ce39` |
| Arquitectura | **ARMv5TE + VFPv2** (los anteriores eran v7) |
| GL | **GLES 1.1 puro** — 190 imports de función fija, 0 de GLES2 |
| Imports | 393 en total: 190 `gl*`, el resto `libc`/`libm`/`liblog`. **Nada más.** |
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

## Lo que ese chequeo NO vio: short vectors de VFP

El párrafo de arriba busca **instrucciones** deprecadas. Los short vectors no son una
instrucción, son un **modo**, y por ahí se colaron. Hay 40 escrituras a `FPSCR` con
este patrón:

```
vmrs r0, fpscr
bic  r0, r0, #0x370000    ; limpia LEN (18:16) y STRIDE (21:20)
orr  r0, r0, #0x30000     ; LEN=3 -> vector de 4   (o #0x70000 -> vector de 8)
vmsr fpscr, r0
vpush {d8-d15}
```

Desde VFPv3 esos campos son RAZ/WI: la escritura se ignora y las instrucciones corren
**escalares**. Sin trap y sin crash — código que espera 4 u 8 resultados obtiene 1.

**Y el harness no lo ve, porque qemu sí los emula.** Medido:

```
FPSCR: 0x00000000 -> 0x00030000 | LEN leido de vuelta = 3
```

O sea el emulador guarda LEN y el hardware no. Es la peor forma de divergencia para un
loop que se autovalida contra qemu: optimista y silenciosa.

**Lo que acota el daño**: las 40 están **todas en el bloque de audio**
(`Java_com_ea_EAAudioCore_AndroidEAAudioCore_Release` y
`Java_com_ea_EAMAudio_EAMAudioCoreWrapper_NativePause`), ninguna en geometría, física
ni transforms. Así que:

- no puede falsear M5–M7, que miden frames, assets, texturas y draws;
- el síntoma esperado en la consola es audio mal, no geometría deformada;
- y como el backend de audio lo proveemos nosotros (`EAAudioCore` / AudioTrack falso),
  puede que ese DSP ni se ejecute en el camino que terminemos usando.

Verificar en la consola antes de dar el audio por bueno. No asumir que porque sonó bien
bajo el harness va a sonar bien en el aparato.

## Lo que SÍ cambia respecto de Minigore 2 e Ice Rage

### 1. No es NativeActivity. Nosotros somos el Java.

Los dos ports anteriores importaban `libandroid.so` y el juego se manejaba solo:
nosotros le dábamos el ciclo de vida y él corría su propio bucle.

Dead Space **no importa `libandroid.so`**. Tiene `JNI_OnLoad` y exporta **68
símbolos `Java_*`**. Es el modelo GLSurfaceView + JNI, donde una Activity de Java
crea el contexto GL y llama al nativo en cada frame.

**La secuencia exacta de arranque** — no deducida, copiada del `main.c` del port de Vita, que
funciona. Todo esto corre en un hilo propio con stack de 1 MB:

```
JNI_OnLoad(&jvm)
EAAudioCore__Startup()                                     ← nuestro, no del juego
Java_com_ea_blast_MainActivity_NativeOnCreate()
Java_com_ea_blast_AndroidRenderer_NativeOnSurfaceCreated()
Java_com_ea_blast_KeyboardAndroid_NativeOnVisibilityChanged(&jni, 0x42424242, 600, 1)
loop:  Java_com_ea_blast_AndroidRenderer_NativeOnDrawFrame();  swap()
```

Dos detalles que sólo se ven leyendo ese archivo:

- **El punto de entrada es `NativeOnCreate`, no `runEntryPoint`.** `runEntryPoint` existe y es
  tentador por el nombre, pero el port que anda no lo llama nunca.
- **`gl_init()` va recién en el frame 2**, no antes del bucle. En Vita es para no comerse una
  pantalla negra larga; acá el orden importa igual, porque el primer `NativeOnDrawFrame` es donde
  el motor toca GL por primera vez.

`NativeOnSurfaceChanged` existe pero el port de Vita nunca lo llama: la resolución es fija.

| Para qué | Símbolo |
|---|---|
| **arranque** | `Java_com_ea_blast_MainActivity_NativeOnCreate` |
| superficie GL | `Java_com_ea_blast_AndroidRenderer_NativeOnSurfaceCreated` |
| **cada frame** | `Java_com_ea_blast_AndroidRenderer_NativeOnDrawFrame` |
| teclado visible | `Java_com_ea_blast_KeyboardAndroid_NativeOnVisibilityChanged` |
| teclas | `Java_com_ea_blast_KeyboardAndroid_NativeOnKeyDown` / `NativeOnKeyUp` |
| audio | `Java_com_ea_EAAudioCore_AndroidEAAudioCore_Init` / `_Release` |
| I/O | `Java_com_ea_EAIO_EAIO_Startup` |

**Consecuencia sobre el scaffold**: se reusa el loader, los thunks de libc y la
maquinaria del JNI falso. **No** se reusa `main.cpp` ni `android/platform.cpp`,
que están escritos para conducir un NativeActivity. Hay que escribir un driver
nuevo: crear la ventana y el contexto, llamar `JNI_OnLoad`, `runEntryPoint`,
`NativeOnSurfaceCreated`, y después `NativeOnDrawFrame` en bucle.

En cierto sentido es **más simple**: el bucle es nuestro, no hay que emular un
`ALooper` ni una cola de eventos. Pero es código nuevo, no un rename.

### 1.b Las 14 clases Java que hay que falsificar — la lista completa

Esto debió estar en el triage del primer día y no estuvo. El skill lo pide
explícitamente ("cada clase que aparezca es una clase Java que vas a tener que
falsificar"), yo conté los 68 símbolos `Java_*` exportados y no hice **la otra**
lista, que es la que dice cuánto trabajo hay. Resultado: cada clase se descubrió
de a una, por crash, en vez de tenerlas todas desde el arranque.

```bash
strings libEAMGameDeadSpace.so | grep -E '^(com/|java/|android/)' | sort -u
```

Son **14**, y ésa es toda la superficie de JNI del port:

| Clase | Para qué |
|---|---|
| `com/ea/EAIO/EAIO` | I/O — `Startup` es nativa, se reenvía al propio juego |
| `com/ea/blast/MainActivity` | `GetInstance`, `getAssets` |
| `com/ea/blast/SystemAndroidDelegate` | qué dispositivo dice ser |
| `com/ea/blast/DisplayAndroidDelegate` | tamaño de pantalla, dpi, orientación |
| `com/ea/blast/PowerManagerAndroid` | keep-awake |
| `com/ea/blast/AccelerometerAndroidDelegate` | acelerómetro (no hay) |
| `com/ea/blast/DeviceOrientationHandlerAndroidDelegate` | rotación (no hay) |
| `com/ea/blast/TouchSurfaceAndroid` | multi-touch (no hay) |
| `com/ea/blast/GetAppDataDirectoryDelegate` | dónde escribir |
| `com/ea/blast/TouchPadAndroidXperiaPlay` | **los touchpads del Xperia Play** |
| `com/eamobile/Query` | compuerta de "contenido listo" |
| `java/io/InputStream` | la ruta de assets por JNI |
| `android/content/res/AssetFileDescriptor` | idem |
| `android/view/ViewRoot` | superficie de la vista |

`TouchPadAndroidXperiaPlay` merece atención cuando lleguemos a controles: es por
donde el juego lee los touchpads de la consola para la que fue compilado, y
probablemente sea el camino natural para mapear los sticks.

**La regla para el próximo port**: enumerar las clases *antes* de escribir código.
Cuesta un comando y convierte "descubrir por crash" en una lista con tildes.

### 2. Los assets viven en el filesystem, no en el APK

`data/deadspace/assets/published/`. El juego los abre por ruta, no por
`AAssetManager` — de hecho **no importa `libandroid.so` en absoluto**, así que todo
el `asset_manager.cpp` que heredamos no se usa.

La traducción de rutas está resuelta en el port de Vita (`reimpl/io.c`, `fix_path`) y
son tres reglas:

| El motor pide | Se traduce a |
|---|---|
| `appbundle:/X` | `<gamedir>/assets/X` |
| cualquier cosa con `Android/data/com.ea.deadspace/files/` | se borra ese prefijo |
| `deadspace/published` | `deadspace/assets/published` |

Se implementa envolviendo `fopen`/`open`/`stat`/`opendir` en la tabla de thunks, no
tocando el motor.

### 3. Es GLES 1.1, y son 190 símbolos

El triage inicial dijo "13 imports de función fija". **Estaba mal** — salían de contar
entradas de PLT. El número real, de `readelf -Ws | awk '$7=="UND"'`, es **190 `gl*`**:
GLES 1.1 completo más las extensiones OES (framebuffer objects, `glDrawTexfOES`,
`glMapBufferOES`, matrix palette, `glTexGen*OES`).

Nuestros thunks de GL están escritos para GLES2 (`thunks/khronos/gles2.cpp`). Hay que
armar la tabla de función fija contra `libGLESv1_CM`. Es mecánica, pero es larga, y
**algunas OES pueden no existir en el driver** — ésas hay que detectarlas resolviendo a
null y ver si el motor las llama de verdad o sólo las declara.

### 4. El audio no es OpenSL ES

Cero imports de OpenSL. El motor saca el audio por JNI, contra un `AudioTrack` de Java
que tenemos que falsificar (`Java_com_ea_EAAudioCore_AndroidEAAudioCore_Init`). El port
de Vita lo tiene resuelto en `android/EAAudioCore.c`, ~8 KB.

O sea: `android/opensles.cpp`, que son 45 KB heredados de Ice Rage, tampoco se usa.

### 5. Hay tres parches al binario y un hilo que hay que atrasar

Del `patch.c` de Vita, contra esta misma `.so` (mismo sha1, así que los offsets valen):

| Offset (desde `text_base`) | Qué |
|---|---|
| `0x0022bf6c`, `0x0022bbe8` | forzar a que falle el chequeo de `"appbundle:"` → el motor usa IO normal en vez de las funciones de IO por JNI |
| `0x0022b214` | nop |
| `0x00320624` | hook a un hilo que, sin atrasarlo, corrompe cosas y revienta en lugares distintos cada vez |

Ese último es la clase de bug que sin la referencia se persigue durante días.

## Referencia externa

`github.com/v-atamanenko/deadspace-vita` (MIT, 96★): 21.635 líneas, **839
entradas `gl*` ya mapeadas** y el JNI falso resuelto. Del mismo autor están
`FalsoJNI` y `soloader-boilerplate`.

Su capa GL no se copia tal cual (usa vitaGL), pero **la tabla de símbolos y el
orden de llamadas al JNI sí**.

## El harness (reescrito, no heredado)

`port/harness/verify.sh` es el árbitro del loop de agentes y **el loop no puede tocarlo**. Por eso
tuvo que reescribirse antes de largar nada: la copia de Ice Rage pedía tres cosas que en este juego
no pueden ser ciertas nunca —`ANativeActivity_onCreate`, un APK, y programas GLSL— y un harness así
no falla ruidosamente, deja el port clavado en M2 mientras los agentes queman iteraciones contra un
fantasma.

| Hito | Qué prueba acá |
|---|---|
| M1 | compila a armhf |
| M2 | la `.so` mapea con 0 símbolos sin resolver (de 393) |
| M3 | `JNI_OnLoad` **y** `NativeOnCreate` retornan |
| M4 | `NativeOnSurfaceCreated` retorna sobre un contexto GLES **1.1** |
| M5 | ≥60 `NativeOnDrawFrame` presentados |
| M6 | assets abiertos + **texturas subidas** + draws > 0 + framebuffer no negro |
| M7 | con autopiloto, la escena **cambia** — o sea el juego avanza, no sólo dibuja |

El cambio de fondo respecto de Ice Rage: ahí M6 contaba programas GLSL linkeados como prueba de que
los materiales eran reales. Acá no hay shaders, así que el equivalente de función fija es
`glTexImage2D` — donde una textura deja de ser un nombre de archivo y pasa a ser algo que la GPU
tiene.

Verificado que la imagen de build trae `libGLESv1_CM.so.1` (glvnd + Mesa llvmpipe), así que M4 en
adelante es alcanzable bajo `qemu-arm`. Sin eso el harness no podría juzgar nada más allá de M3.

## Estimación

**3-5 días.** Las dos incógnitas que la inflaban a 1-2 semanas —GLES 1.1 y
ARMv5TE— resultaron ser ninguna. Lo que queda de trabajo real es el driver
JNI/GLSurfaceView, que es código nuevo pero acotado.
