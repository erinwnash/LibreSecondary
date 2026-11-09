# LibreSecondary

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Standard: C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://en.wikipedia.org/wiki/C%2B%2B17)
[![OpenFX](https://img.shields.io/badge/OpenFX-1.5-green.svg)](https://github.com/AcademySoftwareFoundation/openfx)

**LibreSecondary** is a high-performance OpenFX (OFX) image effect plugin implementing a DaVinci Resolve-style Secondary Keyer (Qualifier) targeting **Foundry Nuke** as the primary host.

---

## Key Features

- **Multi-Color Space Qualification:**
  - **HSV (Hue, Saturation, Value):** Classic perceptual color grading model with periodic hue distance wrap $[0.0, 1.0)$.
  - **HSL (Hue, Saturation, Lightness):** Symmetrical lightness qualification with perceptual color saturation.
  - **YCbCr (ITU-R BT.709):** Digital broadcast broadcast-standard luma ($Y$) and blue/red-difference chrominance ($Cb, Cr$).

- **Continuous Add & Sub Multi-Color Lists (like Keylight):**
  - **Continuous Selection Mode:** Sample footage continuously with the eye-dropper to dynamically add or subtract color swatches into the active qualification lists.
  - **Collapsible Disclosure Groups:** Add Colors and Sub Colors lists are nested inside dedicated groups that spin up / collapse under an arrow.
  - **Individual Delete Buttons:** Each added or subtracted color swatch in the list has an individual `[X]` delete button for instant removal.
  - **Clear All:** Single-click clearing of additive or subtractive qualification sets.
  - **Project State Persistence:** Active color lists automatically persist across Nuke project save and reload.

- **4 Best Falloff Curves:**
  - **Smoothstep (Hermite Cubic):** $1 - (3t^2 - 2t^3)$ — Industry standard gradual roll-off with flat boundary tangents.
  - **Smootherstep (Perlin Quintic):** $1 - (6t^5 - 15t^4 + 10t^3)$ — Continuous 1st and 2nd derivatives for ultra-smooth edge transitions without perceptual knees.
  - **Cosine (Filmic S-Curve):** $0.5 \cdot (1 + \cos(\pi t))$ — Photographic, organic S-curve matching human visual roll-off.
  - **Linear (Crisp Slope):** $1 - t$ — Direct linear slope for sharp, mathematically uniform transitions.

- **4 Best Spatial Blur Filter Types:**
  - **Gaussian (Separable Optical):** True 2-pass separable Gaussian convolution ($e^{-k^2/(2\sigma^2)}$) for natural photographic edge softening.
  - **Box Blur (Uniform Window):** Fast moving average box filter for flat, even edge feathering.
  - **Triangle (Tent Linear):** Linearly-weighted 2-pass filter with sharper feature peak retention than Gaussian.
  - **Exponential (Decay Tail):** Sharp central peak with long optical decay tail ($e^{-|k|/\sigma}$) mimicking optical flare and glow roll-off.

- **Matte Finesse & Morphology:**
  - **Matte Shrink:** Circular erosion (minimum filter).
  - **Matte Grow:** Circular dilation (maximum filter).
  - **Matte Blur:** 2-pass separable convolution using the selected filter type.

- **Tile-Based Rendering Architecture:**
  - Overrides `getRegionsOfInterest` and `getRegionOfDefinition` to request necessary spatial padding from the host, eliminating tile boundary and seam artifacts during tiled or multi-threaded rendering in Nuke.

- **Multi-Threaded Processing:**
  - Native SMP multi-threading via `OFX::MultiThread::Processor::multiThread()`.
  - Zero-allocation fast path when spatial finesse is set to zero.

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

2. Clone repository and configure with CMake:
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

## Parameters Reference

| Parameter | Type | Default | Options / Range | Description |
|:---|:---|:---:|:---:|:---|
| **`color_space`** | Choice | `HSV` | `HSV`, `HSL`, `YCbCr (Rec. 709)` | Operating qualification color space |
| **`falloff_curve`** | Choice | `Smoothstep` | `Smoothstep`, `Smootherstep`, `Cosine`, `Linear` | Mathematical transition roll-off profile |
| **`hsv_pick`** | RGB | (0, 0, 0) | [0, 1] | Target primary base color to isolate |
| **`grp_add_colors`** | Group | *Collapsed* | — | Group for expanding qualification (spins up under arrow) |
| **`pick_add_color`** | RGB | (0, 0, 0) | [0, 1] | Eye-dropper for sampling additional colors |
| **`continuous_add`** | Bool | `true` | `true`/`false` | Automatically add sampled colors on eye-dropper click |
| **`btn_add_color`** | Button | — | — | Explicit button to append sampled color to list |
| **`btn_clear_add`** | Button | — | — | Clear all added colors from the list |
| **`add_color_0..15`**| RGB | (0, 0, 0) | [0, 1] | Individual color swatches in the Add list |
| **`del_add_0..15`** | Button | `[X]` | — | Individual delete button for each added color |
| **`grp_sub_colors`** | Group | *Collapsed* | — | Group for carving out colors (spins up under arrow) |
| **`pick_sub_color`** | RGB | (0, 0, 0) | [0, 1] | Eye-dropper for sampling subtracted colors |
| **`continuous_sub`** | Bool | `true` | `true`/`false` | Automatically subtract sampled colors on eye-dropper click |
| **`btn_sub_color`** | Button | — | — | Explicit button to append sampled sub color to list |
| **`btn_clear_sub`** | Button | — | — | Clear all subtracted colors from the list |
| **`sub_color_0..15`**| RGB | (0, 0, 0) | [0, 1] | Individual color swatches in the Sub list |
| **`del_sub_0..15`** | Button | `[X]` | — | Individual delete button for each subtracted color |
| **`hue_spread`** | Double | 0.1 | [0.0, 0.5] | Channel 1 (Hue / Y) tolerance plateau half-width |
| **`hue_falloff`** | Double | 0.05 | [0.001, 0.5] | Channel 1 (Hue / Y) softness transition width |
| **`sat_spread`** | Double | 0.2 | [0.0, 1.0] | Channel 2 (Sat / Cb) tolerance plateau half-width |
| **`sat_falloff`** | Double | 0.1 | [0.001, 1.0] | Channel 2 (Sat / Cb) softness transition width |
| **`val_spread`** | Double | 0.2 | [0.0, 1.0] | Channel 3 (Val / Cr) tolerance plateau half-width |
| **`val_falloff`** | Double | 0.1 | [0.001, 1.0] | Channel 3 (Val / Cr) softness transition width |
| **`matte_shrink`**| Double | 0.0 | [0.0, 50.0] | Morphological circular erosion radius in pixels |
| **`matte_grow`**  | Double | 0.0 | [0.0, 50.0] | Morphological circular dilation radius in pixels |
| **`matte_blur`**  | Double | 0.0 | [0.0, 100.0] | Spatial blur radius ($\sigma$) in pixels |
| **`filter_type`** | Choice | `Gaussian` | `Gaussian`, `Box`, `Triangle`, `Exponential` | Spatial convolution blur kernel type |
| **`output_mode`** | Choice | `Premultiplied` | `Premultiplied RGBA`, `Alpha Matte Only` | Render output mode |

---

## License

This project is licensed under the [MIT License](LICENSE).
The OpenFX standard and Support Library are copyrighted by The Open Effects Association Ltd under the BSD-3-Clause license.
