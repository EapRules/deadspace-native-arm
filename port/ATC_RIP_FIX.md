# Dead Space — diagnóstico y corrección del RIP ATC

Registro de la incidencia de texturas 3D blancas observada al instalar el
port con el ZIP RIP de Dead Space Mobile.

## Síntomas

- El port se instalaba correctamente.
- La primera ejecución mostraba la pantalla de extracción de datos de `eapx`.
- El juego arrancaba, pero personajes, objetos y fondos 3D aparecían blancos.
- El mismo loader funcionaba con el paquete completo de datos.

La pantalla de extracción pertenece al importador de primer arranque `eapx.py`,
no a PortMaster. El importador descubre el donor por su contenido, lo extrae
en una etapa temporal, valida el árbol y luego publica `assets/` y `lib/`.

## Causa raíz

El paquete completo y el RIP no usan la misma compresión de texturas:

| Datos | Formatos observados | Resultado original |
|---|---|---|
| ZIP completo | PVRTC `0x8c00` / `0x8c02` | Ya cubierto por el fallback PVRTC |
| ZIP RIP | ATC `0x8c92` / `0x8c93` | El driver rechazaba las cargas |

En la prueba del RIP, el diagnóstico GL reportaba `GL_INVALID_ENUM (0x0500)`
después de cada `glCompressedTexImage2D`. Esos formatos son:

- `0x8c92`: `GL_ATC_RGB_AMD`
- `0x8c93`: `GL_ATC_RGBA_EXPLICIT_ALPHA_AMD`

El Mali/llvmpipe utilizado en la ruta del port no anuncia
`GL_AMD_compressed_ATC_texture`, por lo que las texturas quedaban sin cargar y
se veían blancas. La especificación de Khronos define ATC como bloques 4×4;
el RGB ocupa 8 bytes por bloque y el RGBA explícito 16 bytes por bloque:
<https://registry.khronos.org/OpenGL/extensions/AMD/AMD_compressed_ATC_texture.txt>.

## Corrección implementada

Se añadió un decodificador ATC software a RGBA8888:

- `src/atc_decompress.h`
- `src/atc_decompress.cpp`
- Integración en `src/symtab_glprobe.cpp`

El loader ahora:

1. Detecta `GL_AMD_compressed_ATC_texture` en el driver.
2. Si no está disponible, decodifica `ATC_RGB_AMD` y
   `ATC_RGBA_EXPLICIT_ALPHA_AMD` a RGBA8888.
3. Envía la textura resultante mediante `glTexImage2D`.
4. Mantiene el fallback PVRTC existente sin cambios.

## Validación en el emulador

Se levantó Colima/Docker y pasó el smoke test del MCP:

```text
MCP smoke test PASS: initialize, start, stick, PNG capture, stop
```

### ZIP completo

```text
PVRTC: decoded ...
framebuffer non-black at frame 10
summary assets=83 textures=149 draws=1102
```

### Instalación con RIP de la SD

```text
ATC: native driver support absent; software fallback enabled
ATC: decoded level=0 format=0x8c93 ...
ATC: decoded level=0 format=0x8c92 ...
framebuffer non-black at frame 10
summary assets=97 textures=233 draws=40
```

Después del fallback ATC no aparecieron errores `0x0500` y el framebuffer dejó
de ser blanco/vacío en la prueba de 20 frames.

## PortMaster y SD

El ZIP actualizado se reconstruyó y validó con `package_portmaster.sh`, luego se
copió a:

```text
/Volumes/ROMS/tools/PortMaster/autoinstall/deadspace-portmaster.zip
```

El hash SHA-256 de origen y destino fue idéntico:

```text
dc4e11888669c1e84b61539fb29ed73f4bf785805c970b7ed2cd052871ca0229
```

El ZIP RIP y los datos ya instalados no se modificaron ni borraron.

La SD contiene además un sidecar de macOS llamado
`._deadspace-portmaster.zip`. PortMaster puede mostrar un fallo adicional para
ese archivo oculto; el archivo válido es el que comienza directamente con
`deadspace-portmaster.zip`.

## Incidencia anterior de PortMaster

En una etapa anterior PortMaster dejó de arrancar porque
`tools/PortMaster/config/config.json` estaba truncado a cero bytes. El log
terminaba en:

```text
JSONDecodeError: Expecting value: line 1 column 1
```

La reinstalación de PortMaster restauró su configuración y permitió volver a
instalar el port. Esa incidencia era independiente del problema ATC del RIP.

## Próximo paso de hardware

Arrancar la R36S con el ZIP actualizado en `autoinstall/`, esperar a que termine
la instalación de PortMaster y ejecutar Dead Space usando el RIP existente. El
log del port se escribe en `ports/deadspace/log.txt`; durante el primer arranque
también se puede revisar `ports/deadspace/eapx.log`.
