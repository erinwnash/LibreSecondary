# LibreSecondary

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Standard: C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://en.wikipedia.org/wiki/C%2B%2B17)
[![OpenFX](https://img.shields.io/badge/OpenFX-1.5-green.svg)](https://github.com/AcademySoftwareFoundation/openfx)

**LibreSecondary** is a high-performance, open-source OpenFX (OFX) image effect plugin implementing a DaVinci Resolve-style Secondary Keyer (HSV Qualifier) targeting **Foundry Nuke** as the primary host.

---

## Features

- **HSV Secondary Qualification:**
  - **Base Pick Color (`hsv_pick`):** Target base color qualification.
  - **Additional Qualified Color (`enable_add`, `color_add`):** Add a secondary color range (`max(base, add)`).
  - **Subtracted Qualified Color (`enable_sub`, `color_sub`):** Exclude an unwanted color range (`max(0, combined - sub)`).
- **Hermite Smoothstep Isolation Curves:**
  - Independent **Spread** (flat pass region) and **Falloff** (Hermite $3t^2 - 2t^3$ transition) parameters for **Hue**, **Saturation**, and **Value**.
  - Circular periodic hue distance calculation accounting for $[0.0, 1.0)$ wrap-around.
- **Matte Finesse (Spatial Morphological & Convolution Ops):**
  - **Matte Shrink (`matte_shrink`):** Circular erosion (minimum filter).
  - **Matte Grow (`matte_grow`):** Circular dilation (maximum filter).
  - **Matte Blur (`matte_blur`):** 2-pass separable Gaussian blur convolution.
- **Tile-Based Rendering Architecture:**
  - Overrides `getRegionsOfInterest` and `getRegionOfDefinition` to request necessary spatial padding from the host, eliminating tile boundary and seam artifacts during tiled or multi-threaded rendering in Nuke.
- **Multi-Threaded Image Processing:**
  - Multi-pass processing orchestrated using OpenFX's native `OFX::MultiThread::Processor` to leverage all CPU cores.
  - Fast-path execution: Zero buffer allocation and single-pass composite when spatial finesse is set to zero.
- **Output Modes:**
  - **Premultiplied RGBA:** R, G, B multiplied by qualified alpha matte, with matte in Alpha.
  - **Alpha Matte Only:** Qualified matte written across all RGBA channels for inspection or direct masking.
- **Pixel Depth Support:**
  - 32-bit floating-point RGBA (`OFX::eBitDepthFloat`, `OFX::ePixelComponentRGBA`).
- **Permissive MIT License:**
  - Free from GPL dependencies; suitable for commercial visual effects production pipelines.

---

## Directory Structure

```text
LibreSecondary/
├── CMakeLists.txt        # Cross-platform CMake build configuration
├── SecondaryKeyer.h       # Plugin definitions, parameter definitions, and class declarations
├── SecondaryKeyer.cpp     # Multi-threaded image processing loop and OFX entrypoints
├── secondaryKeyer.md      # Specification and requirements document
├── LICENSE                # MIT License
├── README.md              # Documentation and build instructions
└── .gitignore             # Git ignore patterns
```

---

## Building

### Requirements
- **CMake** 3.16 or later
- **C++17** compatible compiler:
  - Linux: GCC 9+ or Clang 10+
  - Windows: Visual Studio 2019, 2022, or 2026 (MSVC)
  - macOS: Xcode 12+ / Apple Clang
- **Git** (for automatically fetching OpenFX headers/library via `FetchContent`)

---

### Compiling on Linux using GCC

1. Install build essentials and CMake:
   ```bash
   # Debian / Ubuntu
   sudo apt-get update
   sudo apt-get install -y build-essential cmake git

   # Rocky Linux / CentOS / RHEL / Fedora
   sudo dnf groupinstall -y "Development Tools"
   sudo dnf install -y cmake git
   ```

2. Clone the repository and configure with CMake:
   ```bash
   git clone https://github.com/erinwnash/LibreSecondary.git
   cd LibreSecondary
   cmake -B build -DCMAKE_BUILD_TYPE=Release
   ```
   *(CMake will automatically fetch the OpenFX C++ support library from GitHub).*

3. Build the plugin bundle:
   ```bash
   cmake --build build -j$(nproc)
   ```

   The compiled plugin bundle will be generated at:
   ```text
   build/LibreSecondary.ofx.bundle/Contents/Linux-x86-64/LibreSecondary.ofx
   ```

---

### Compiling on Windows using MSVC

1. Open PowerShell or Developer Command Prompt:
   ```powershell
   git clone https://github.com/erinwnash/LibreSecondary.git
   cd LibreSecondary
   cmake -B build
   cmake --build build --config Release
   ```

   The compiled bundle will be at:
   ```text
   build/LibreSecondary.ofx.bundle/Contents/Win64/LibreSecondary.ofx
   ```

---

### Compiling on macOS

```bash
git clone https://github.com/erinwnash/LibreSecondary.git
cd LibreSecondary
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(sysctl -n hw.ncpu)
```

The compiled bundle will be at:
```text
build/LibreSecondary.ofx.bundle/Contents/MacOS/LibreSecondary.ofx
```

---

## Installation in Foundry Nuke

Standard OpenFX plugin search paths:

- **Linux:** `/usr/OFX/Plugins/`
- **Windows:** `C:\Program Files\Common Files\OFX\Plugins\`
- **macOS:** `/Library/OFX/Plugins/`

### Manual Installation
Copy the directory `LibreSecondary.ofx.bundle` into one of the directories above.

Alternatively, set the `OFX_PLUGIN_PATH` environment variable in your terminal, Nuke startup script, or `.bashrc`:

```bash
export OFX_PLUGIN_PATH=/path/to/LibreSecondary/build:$OFX_PLUGIN_PATH
```

Inside Nuke:
1. Hit `Tab` or `X` in the Node Graph.
2. Type `LibreSecondary`.
3. Connect your image stream to **Source** and view the qualified output on **Output**.

---

## Parameters

| Parameter | Type | Default | Range | Description |
|:---|:---|:---:|:---:|:---|
| **`hsv_pick`** | RGB | (0, 0, 0) | [0, 1] | Target base color to isolate using HSV qualification |
| **`enable_add`** | Bool | `false` | `true`/`false` | Enable additional color range in matte |
| **`color_add`** | RGB | (0, 0, 0) | [0, 1] | Additional color to union with base matte |
| **`enable_sub`** | Bool | `false` | `true`/`false` | Enable subtraction of color range from matte |
| **`color_sub`** | RGB | (0, 0, 0) | [0, 1] | Color range to subtract from qualified matte |
| **`hue_spread`** | Double | 0.1 | [0.0, 0.5] | Half-width of flat tolerance plateau in Hue |
| **`hue_falloff`** | Double | 0.05 | [0.001, 0.5] | Softness transition width beyond hue spread |
| **`sat_spread`** | Double | 0.2 | [0.0, 1.0] | Half-width of flat tolerance plateau in Saturation |
| **`sat_falloff`** | Double | 0.1 | [0.001, 1.0] | Softness transition width beyond sat spread |
| **`val_spread`** | Double | 0.2 | [0.0, 1.0] | Half-width of flat tolerance plateau in Value/Luma |
| **`val_falloff`** | Double | 0.1 | [0.001, 1.0] | Softness transition width beyond val spread |
| **`matte_shrink`**| Double | 0.0 | [0.0, 50.0] | Morphological circular erosion radius in pixels |
| **`matte_grow`**  | Double | 0.0 | [0.0, 50.0] | Morphological circular dilation radius in pixels |
| **`matte_blur`**  | Double | 0.0 | [0.0, 100.0] | Separable Gaussian blur standard deviation ($\sigma$) |
| **`output_mode`** | Choice | `Premultiplied RGBA` | 0 or 1 | Switch between `Premultiplied RGBA` and `Alpha Matte Only` |

---

## License

This project is licensed under the [MIT License](LICENSE).
The OpenFX standard and Support Library are copyrighted by The Open Effects Association Ltd under the BSD-3-Clause license.
