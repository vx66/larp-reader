<div align="center">

# 📖 Larp Reader

### A lightweight native EPUB reader for the Sony PSP

![Version](https://img.shields.io/badge/version-0.0.19-6f5a87?style=flat-square)
![Platform](https://img.shields.io/badge/platform-Sony%20PSP-2f3437?style=flat-square)
![Language](https://img.shields.io/badge/C%2B%2B-17-4d6d8a?style=flat-square)
![SDL2](https://img.shields.io/badge/SDL-2-596b58?style=flat-square)
![Format](https://img.shields.io/badge/format-EPUB-8b6f47?style=flat-square)

**Lee EPUBs en una PSP con una interfaz simple, rápida y pensada para una pantalla de 480×272.**

</div>

---

## ✦ Acerca de Larp Reader

**Larp Reader** es un lector EPUB nativo escrito en **C++17 + SDL2** para Sony PSP.

El proyecto nació con una idea sencilla: poder leer libros EPUB directamente en una PSP sin convertir previamente el libro a imágenes, PDF o texto plano.

El lector procesa la estructura interna del EPUB, pagina el contenido para la pantalla de la consola y mantiene una posición de lectura **semántica**, de modo que cambiar la orientación o el tamaño de fuente no debería hacerte perder el punto donde estabas leyendo.

> **Versión actual:** `v0.0.19`

---

## ✨ Características

- Lectura de archivos **EPUB** directamente desde la Memory Stick.
- Biblioteca automática desde `ms0:/BOOKS/`.
- Lectura del **spine** del EPUB.
- Tabla de contenidos.
- Navegación mediante anchors internos.
- Texto UTF-8 mediante **Noto Sans**.
- Renderizado de imágenes **JPEG y PNG**.
- Texto e imágenes mezclados dentro de un mismo libro.
- Soporte básico de estilos:
  - texto normal;
  - cursiva;
  - encabezados;
  - texto centrado;
  - captions de imágenes.
- Modo **landscape** y **portrait**.
- Reflow real del texto al cambiar orientación.
- Tamaños de fuente configurables:
  - `12`
  - `14`
  - `16`
  - `18`
- Cuatro temas de lectura:
  - **Sepia**
  - **Claro**
  - **Oscuro**
  - **Negro**
- Progreso de lectura persistente.
- Posición semántica basada en bloque + offset de texto.
- Orientación, tema y tamaño de fuente persistentes.
- Interfaz y controles adaptados a PSP.
- Log de depuración integrado.

---

## 🎮 Controles

### Biblioteca

| Botón | Acción |
|---|---|
| `↑ / ↓` | Seleccionar libro |
| `✕` Cross | Abrir libro |
| `START` | Salir |

### Lector

| Botón | Acción |
|---|---|
| `L / R` | Página anterior / siguiente |
| `↑ / ↓` | Página anterior / siguiente |
| `✕` Cross | Cambiar tema |
| `△` Triangle | Abrir índice |
| `□` Square | Landscape / Portrait |
| `○` Circle | Volver a la biblioteca |
| `SELECT` | Cambiar tamaño de fuente |
| `START` | Guardar progreso y salir |

### Índice

| Botón | Acción |
|---|---|
| `↑ / ↓` | Mover selección |
| `✕` Cross | Ir al capítulo seleccionado |
| `○` Circle | Cerrar índice |
| `△` Triangle | Cerrar índice |
| `START` | Guardar progreso y salir |

---

## 🎨 Temas de lectura

Pulsa `✕` mientras estás leyendo para cambiar entre:

```text
Sepia → Claro → Oscuro → Negro → Sepia
```

La selección queda guardada en `settings.cfg`.

---

## 🔤 Tamaño de fuente

Pulsa `SELECT` mientras lees:

```text
12 → 14 → 16 → 18 → 12
```

El documento se vuelve a paginar manteniendo la posición semántica actual.

---

## ↕️ Modo Portrait

Pulsa `□` para alternar:

```text
Landscape ↔ Portrait
```

Portrait usa un canvas lógico de `272×480` y el contenido se adapta mediante reflow.

El lector conserva la misma posición semántica aunque el número total de páginas cambie entre orientaciones.

---

# 🛠️ Compilación

## Requisitos

Necesitas una instalación funcional de **PSPDEV**.

Además del toolchain de PSP, el proyecto utiliza:

- CMake `>= 3.20`
- Ninja
- pkg-config
- SDL2
- SDL2_image
- SDL2_ttf
- tinyxml2
- libzip
- mbedTLS
- bzip2
- liblzma
- zlib

Las bibliotecas deben estar instaladas para el target PSP dentro de tu entorno PSPDEV.

Comprueba primero:

```bash
echo "$PSPDEV"
psp-g++ --version
cmake --version
ninja --version
```

`$PSPDEV` debe apuntar a tu instalación de PSPDEV. Un ejemplo habitual es:

```bash
export PSPDEV=/opt/pspdev
export PATH="$PSPDEV/bin:$PATH"
```

---

## Compilar desde cero

Desde la raíz del repositorio:

```bash
rm -rf build

cmake \
    -S . \
    -B build \
    -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$PSPDEV/psp/share/pspdev.cmake" \
    -DCMAKE_BUILD_TYPE=Release

cmake --build build
```

Si todo sale bien se generará:

```text
build/EBOOT.PBP
```

---

## 📁 Estructura necesaria

Larp Reader espera encontrar la fuente en:

```text
PSP/GAME/LarpReader/
├── EBOOT.PBP
└── assets/
    └── fonts/
        └── NotoSans-Regular.ttf
```

Los libros van separados, en:

```text
BOOKS/
├── libro1.epub
├── libro2.epub
└── ...
```

Desde el código, esas rutas corresponden a:

```text
ms0:/PSP/GAME/LarpReader/
ms0:/BOOKS/
```

---

# 🧪 Ejecutar en PPSSPP

En Linux, una instalación típica de PPSSPP utiliza:

```text
~/.config/ppsspp/
```

Puedes preparar Larp Reader así:

```bash
mkdir -p ~/.config/ppsspp/PSP/GAME/LarpReader/assets/fonts
mkdir -p ~/.config/ppsspp/BOOKS

cp build/EBOOT.PBP \
    ~/.config/ppsspp/PSP/GAME/LarpReader/EBOOT.PBP

cp assets/fonts/NotoSans-Regular.ttf \
    ~/.config/ppsspp/PSP/GAME/LarpReader/assets/fonts/
```

Después copia tus EPUB:

```bash
cp ~/Books/*.epub ~/.config/ppsspp/BOOKS/
```

Y ejecuta:

```bash
PPSSPPSDL \
    ~/.config/ppsspp/PSP/GAME/LarpReader/EBOOT.PBP
```

También puedes lanzar directamente la build desde la raíz del proyecto:

```bash
PPSSPPSDL "$PWD/build/EBOOT.PBP"
```

> La ubicación del directorio de PPSSPP puede variar dependiendo del sistema operativo o del tipo de instalación.

---

# 🕹️ Instalar en una PSP

Copia esta estructura a la Memory Stick:

```text
ms0:/
├── BOOKS/
│   └── tus-libros.epub
│
└── PSP/
    └── GAME/
        └── LarpReader/
            ├── EBOOT.PBP
            └── assets/
                └── fonts/
                    └── NotoSans-Regular.ttf
```

Después inicia **Larp Reader** desde:

```text
Juego → Memory Stick
```

Los EPUB colocados dentro de `ms0:/BOOKS/` aparecerán automáticamente en la biblioteca.

---

## 💾 Archivos generados por Larp Reader

La aplicación guarda sus datos en:

```text
ms0:/PSP/GAME/LarpReader/
```

### `progress.tsv`

Guarda el progreso de lectura.

La versión actual utiliza una posición semántica para poder conservar el punto de lectura aunque cambie la paginación.

### `settings.cfg`

Guarda preferencias como:

```text
orientation=landscape
theme=sepia
font_size=14
```

### `debug.log`

Contiene información útil para depurar EPUBs y problemas del renderer.

En PPSSPP para Linux normalmente se encuentra en:

```text
~/.config/ppsspp/PSP/GAME/LarpReader/debug.log
```

---

## 📚 Compatibilidad EPUB

Larp Reader implementa un renderer propio y **no es un navegador web completo**.

Está pensado principalmente para EPUBs tradicionales de texto, novelas y libros con ilustraciones.

Actualmente soporta las partes del formato necesarias para:

- metadata básica;
- spine;
- tabla de contenidos;
- anchors;
- XHTML;
- texto UTF-8;
- JPEG / PNG;
- estilos tipográficos básicos.

EPUBs con CSS complejo, JavaScript, DRM, layouts fijos avanzados o contenido web muy elaborado pueden no renderizar correctamente.

---

## 🧱 Estructura del proyecto

```text
LarpReader/
├── assets/
│   └── fonts/
│       └── NotoSans-Regular.ttf
├── src/
│   └── main.cpp
├── CMakeLists.txt
└── README.md
```

---

## 🔬 Estado del proyecto

`v0.0.19` constituye la primera versión funcional completa del lector.

La versión actual ha sido desarrollada y probada principalmente con **PPSSPP**.  
Las pruebas exhaustivas en hardware PSP real todavía son una etapa importante del proyecto.

---

## ⚠️ Nota sobre los libros

Este repositorio **no incluye EPUBs comerciales o con copyright**.

Usa únicamente libros propios, de dominio público o archivos para los que tengas permiso de uso.

---

<div align="center">

### Larp Reader

**EPUB reading on hardware that refuses to die.**

`C++17` · `SDL2` · `PSPDEV` · `EPUB`

</div>
