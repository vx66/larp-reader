<div align="center">

# 📖 Larp Reader

### A lightweight native EPUB reader for the Sony PSP

**Created by Xergno · DEAD SMILE**

![Version](https://img.shields.io/badge/version-0.0.22-111111?style=flat-square)
![Platform](https://img.shields.io/badge/platform-Sony%20PSP-2f3437?style=flat-square)
![Language](https://img.shields.io/badge/C%2B%2B-17-4d6d8a?style=flat-square)
![SDL2](https://img.shields.io/badge/SDL-2-596b58?style=flat-square)
![Format](https://img.shields.io/badge/format-EPUB-8b6f47?style=flat-square)

**Read EPUB books directly on a PSP with a fast, simple interface designed for a 480×272 screen.**

</div>

---

## About Larp Reader

**Larp Reader** is a native EPUB reader written in **C++17 + SDL2** for the Sony PSP.

The project started from a simple idea: copy a regular EPUB file to a PSP Memory Stick and read it directly on the console without converting it to images, PDF, or plain text first.

Larp Reader parses the internal structure of an EPUB, builds its own reading layout, renders text and illustrations, and keeps a semantic reading position so changing orientation or font size does not make you lose your place.

> **Current version:** `v0.0.22`

The reader has been tested on real PSP hardware as well as PPSSPP.

---

## Features

- Native **EPUB** reading from the PSP Memory Stick.
- Automatic library scanning from `ms0:/BOOKS/`.
- EPUB metadata parsing.
- EPUB manifest and spine support.
- Table of contents support.
- Internal anchor navigation.
- UTF-8 text rendering with **Noto Sans**.
- JPEG and PNG image rendering.
- Mixed text and images inside the same book.
- Basic typography support:
  - regular text;
  - italic text;
  - headings;
  - centered text;
  - image captions.
- Landscape and portrait reading modes.
- Real text reflow when changing orientation.
- Configurable font sizes:
  - `12`
  - `14`
  - `16`
  - `18`
- Four reading themes:
  - **Sepia**
  - **Light**
  - **Dark**
  - **Black**
- Persistent reading progress.
- Semantic reading position based on document block + text offset.
- Persistent orientation, theme, and font size.
- Support for EPUB filenames containing accented / Unicode characters on real PSP hardware.
- PSP-safe scaling for large EPUB illustrations.
- Custom PSP XMB icon and background.
- Integrated debug logging.

---

## Controls

### Library

| Button | Action |
|---|---|
| `↑ / ↓` | Select book |
| `✕` Cross | Open selected book |
| `START` | Exit Larp Reader |

### Reader

| Button | Action |
|---|---|
| `L / R` | Previous / next page |
| `↑ / ↓` | Previous / next page |
| `✕` Cross | Change reading theme |
| `△` Triangle | Open table of contents |
| `□` Square | Switch landscape / portrait |
| `○` Circle | Return to library |
| `SELECT` | Change font size |
| `START` | Save progress and exit |

### Table of Contents

| Button | Action |
|---|---|
| `↑ / ↓` | Move selection |
| `✕` Cross | Jump to selected chapter |
| `○` Circle | Close table of contents |
| `△` Triangle | Close table of contents |
| `START` | Save progress and exit |

---

## Reading Themes

Press `✕` while reading to cycle through:

```text
Sepia → Light → Dark → Black → Sepia
```

The selected theme is stored in `settings.cfg`.

---

## Font Size

Press `SELECT` while reading:

```text
12 → 14 → 16 → 18 → 12
```

The current chapter is reflowed while keeping the same semantic reading position.

---

## Portrait Mode

Press `□` to switch between:

```text
Landscape ↔ Portrait
```

Portrait mode uses direct rendering to the PSP's physical `480×272` framebuffer.

Logical portrait coordinates are transformed to the physical screen, while text and image surfaces are rotated before texture creation.

This avoids relying on a large off-screen portrait render target and works correctly on real PSP hardware.

The semantic reading position is preserved even though the total page count changes between orientations.

---

## Real PSP Hardware Compatibility

Larp Reader was developed with PPSSPP for fast iteration, but the final reader has also been tested on a real **PSP-3000**.

Testing on real hardware exposed several issues that were not visible in the emulator.

### 512×512 texture limit

The PSP cannot create arbitrarily large textures.

Large EPUB illustrations are decoded, scaled to a PSP-safe size, and only then uploaded as textures.

This prevents errors such as:

```text
Texture dimensions are limited to 512x512
```

and also avoids portrait rotation failures caused by oversized image surfaces.

### Accented and Unicode EPUB filenames

Real PSP Memory Stick filesystem behavior differs from a regular desktop filesystem.

Larp Reader uses native PSP directory APIs and handles FAT filename aliases so files such as:

```text
Nagaru Tanigawa - Un día lluvioso.epub
Nagaru Tanigawa - Volumen 1 La Melancolía de Suzumiya Haruhi.epub
```

can be detected and opened correctly on real hardware.

---

# Installation

The easiest way to install Larp Reader is to download the latest release ZIP.

## Memory Stick layout

After extracting the release, copy the `PSP` and `BOOKS` folders to the root of your Memory Stick.

The final layout should look like this:

```text
ms0:/
├── BOOKS/
│   ├── book1.epub
│   ├── book2.epub
│   └── ...
│
└── PSP/
    └── GAME/
        └── LarpReader/
            ├── EBOOT.PBP
            └── assets/
                └── fonts/
                    ├── NotoSans-Regular.ttf
                    └── OFL.txt
```

Put your EPUB files inside:

```text
ms0:/BOOKS/
```

Then launch Larp Reader from:

```text
Game → Memory Stick → Larp Reader
```

---

# Building from Source

## Requirements

You need a working **PSPDEV / PSPSDK** installation.

The project uses:

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

All libraries must be installed for the PSP target inside your PSPDEV environment.

Check your toolchain first:

```bash
echo "$PSPDEV"
psp-g++ --version
cmake --version
ninja --version
```

A common PSPDEV setup is:

```bash
export PSPDEV=/opt/pspdev
export PATH="$PSPDEV/bin:$PATH"
```

---

## Build

From the repository root:

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

If the build succeeds, the PSP executable will be generated at:

```text
build/EBOOT.PBP
```

---

## XMB Artwork

The PSP XMB artwork is stored in:

```text
assets/xmb/
├── ICON0.PNG
└── PIC1.PNG
```

Expected dimensions:

```text
ICON0.PNG → 144×82
PIC1.PNG  → 480×272
```

These files are embedded directly inside `EBOOT.PBP` during the build.

They do not need to be copied separately to the Memory Stick.

---

# Running in PPSSPP

On Linux, a common PPSSPP directory is:

```text
~/.config/ppsspp/
```

Prepare Larp Reader with:

```bash
mkdir -p ~/.config/ppsspp/PSP/GAME/LarpReader/assets/fonts
mkdir -p ~/.config/ppsspp/BOOKS

cp build/EBOOT.PBP \
  ~/.config/ppsspp/PSP/GAME/LarpReader/EBOOT.PBP

cp assets/fonts/NotoSans-Regular.ttf \
  ~/.config/ppsspp/PSP/GAME/LarpReader/assets/fonts/

cp assets/fonts/OFL.txt \
  ~/.config/ppsspp/PSP/GAME/LarpReader/assets/fonts/
```

Copy EPUB files:

```bash
cp ~/Books/*.epub ~/.config/ppsspp/BOOKS/
```

Then launch:

```bash
PPSSPPSDL \
  ~/.config/ppsspp/PSP/GAME/LarpReader/EBOOT.PBP
```

You can also launch the generated build directly:

```bash
PPSSPPSDL "$PWD/build/EBOOT.PBP"
```

> PPSSPP paths may vary depending on the operating system and installation method.

---

## Files Generated by Larp Reader

Runtime files are stored in:

```text
ms0:/PSP/GAME/LarpReader/
```

### `progress.tsv`

Stores reading progress for each book.

The current progress format uses a semantic document position so changing pagination does not lose the reading location.

### `settings.cfg`

Stores reader preferences such as:

```text
orientation=landscape
theme=sepia
font_size=14
```

### `debug.log`

Contains useful information for debugging EPUB parsing, layout, images, filesystem behavior, and rendering issues.

In a typical Linux PPSSPP setup:

```text
~/.config/ppsspp/PSP/GAME/LarpReader/debug.log
```

---

## EPUB Compatibility

Larp Reader uses its own lightweight renderer.

It is **not a full web browser** and does not attempt to implement the complete HTML/CSS platform.

It is primarily designed for traditional EPUB books, novels, and illustrated light novels.

Currently supported areas include:

- basic metadata;
- manifest;
- spine;
- table of contents;
- internal anchors;
- XHTML text;
- UTF-8 text;
- JPEG images;
- PNG images;
- basic typography;
- captions;
- mixed text and images.

EPUBs that depend heavily on complex CSS, JavaScript, DRM, advanced fixed layouts, or browser-specific behavior may not render correctly.

---

## Supported Formats

Larp Reader currently supports:

```text
EPUB
```

It does **not** currently support:

```text
PDF
CBZ
TXT
```

No other ebook format should be assumed to work unless it is explicitly documented here.

---

## Project Structure

```text
LarpReader/
├── assets/
│   ├── fonts/
│   │   ├── NotoSans-Regular.ttf
│   │   └── OFL.txt
│   └── xmb/
│       ├── ICON0.PNG
│       └── PIC1.PNG
├── src/
│   └── main.cpp
├── CMakeLists.txt
└── README.md
```

Build and release directories are generated locally and are not required as part of the source tree.

---

## Hardware Tested

The current version has been tested successfully on:

```text
Sony PSP-3000
System Software 6.61
ARK-5 cIPL
```

PPSSPP is still extremely useful during development, but real PSP testing is important because some filesystem, memory, texture, and renderer behavior is hardware-specific.

---

## Releases

Download packaged builds from:

https://github.com/vx66/larp-reader/releases

The release ZIP contains the correct PSP folder structure and a multilingual installation guide.

---

## Books and Copyright

This repository does **not** include commercial or copyrighted EPUB books.

Use your own books, public-domain books, or files that you have permission to use.

---

## Credits

**Larp Reader** was created by **Xergno** under **DEAD SMILE**.

Noto Sans is distributed under the **SIL Open Font License**. The corresponding `OFL.txt` file is included with the font.

---

<div align="center">

### Larp Reader

**EPUB reading on hardware that refuses to die.**

`C++17` · `SDL2` · `PSPDEV` · `EPUB`

</div>
