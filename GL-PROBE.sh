#!/bin/bash
#
# Contesta una sola pregunta: ¿este aparato sabe OpenGL ES 1.1?
#
# De eso depende cuánto cuesta portar Dead Space, que es GLES 1.1 puro (13
# imports de función fija, cero de GLES2). Si el driver ya expone esos símbolos
# no hay que escribir ni un thunk; si no, hay que meter GL4ES en el medio.
#
# Va en ports/ como cualquier entrada, se ejecuta desde el menú, no dibuja nada
# y escribe el informe al lado del script. Tarda unos segundos.

OUT="/$directory/ports/gl-probe.txt"
[ -n "$directory" ] || OUT="$(dirname "$0")/gl-probe.txt"

{
  echo "=== GL probe — $(date) ==="
  echo
  echo "--- qué librerías de GL hay ---"
  for d in /usr/lib/arm-linux-gnueabihf /usr/lib /usr/lib32 /lib/arm-linux-gnueabihf; do
    [ -d "$d" ] && ls -la "$d" 2>/dev/null | grep -iE "gles|mali|egl|gl4es" | sed "s|^|  [$d] |"
  done

  echo
  echo "--- ¿existe libGLESv1_CM? (la pregunta principal) ---"
  find /usr/lib /lib -name "libGLESv1*" 2>/dev/null || echo "  (ninguno)"

  echo
  echo "--- ¿el blob de Mali exporta los símbolos de función fija? ---"
  for blob in /usr/lib/arm-linux-gnueabihf/libmali*.so* /usr/lib/libmali*.so*; do
    [ -e "$blob" ] || continue
    echo "  $blob"
    if command -v readelf >/dev/null 2>&1; then
      n=$(readelf -Ws "$blob" 2>/dev/null | grep -cE "glMatrixMode|glVertexPointer|glTexEnvf")
      echo "    símbolos GLES1 exportados: $n   (>0 = no hay que escribir thunks)"
    else
      # readelf puede no estar; grep sobre el binario alcanza para saber si el
      # nombre está adentro.
      n=$(grep -ac "glMatrixMode" "$blob" 2>/dev/null)
      echo "    'glMatrixMode' aparece en el binario: $n  (readelf no está, esto es aproximado)"
    fi
  done

  echo
  echo "--- ¿hay GL4ES en el sistema? ---"
  find / -name "libGL.so*" -o -name "*gl4es*" 2>/dev/null | head -10

  echo
  echo "--- qué reporta el driver ---"
  command -v glxinfo   >/dev/null 2>&1 && glxinfo -B 2>/dev/null | head -8
  command -v es2_info  >/dev/null 2>&1 && es2_info 2>/dev/null | head -6
  echo
  echo "=== fin ==="
} > "$OUT" 2>&1

sync
