# ROLE & GOAL
You are an expert C++ VFX software engineer. Your task is to write a complete, compilable OpenFX (OFX) plugin for a DaVinci Resolve-style Secondary Keyer (Qualifier) targeting Foundry Nuke as the primary host.

# TECH STACK & CONSTRAINTS
- Language: C++17
- Framework: OpenFX API using the `openfx-cplusplus` support library (do NOT use the raw C API).
- Build System: CMake (generate a complete CMakeLists.txt).
- License constraint: The code must be compatible with a permissive MIT/BSD-3 release. Do not include any GPL dependencies.

# ARCHITECTURE REQUIREMENTS
1. Plugin Class: Inherit from `OFX::ImageEffect` and `OFX::ImageEffectFactory`.
2. Contexts: Support `OFX::eContextFilter` and `OFX::eContextGeneral`.
3. Inputs/Outputs: One input clip ("Source"), one output clip ("Output"). 
4. Multi-threading: Implement the image processing loop inside the `render()` override using `OFX::MultiThread::Processor`.
5. Pixel Formats: Support 32-bit floating-point RGBA (`OFX::ePixelComponentRGBA`, `OFX::eBitDepthFloat`).
6. Tile-Based Rendering: Override `getRegionsOfInterest` and `getRegionOfDefinition` to request necessary padding for the spatial finesse filtering (blur, grow, shrink) to prevent tile boundary artifacts.

# PARAMETERS (KNOBS) TO IMPLEMENT
Implement the following parameters using `OFX::DoubleParam`, `OFX::RGBParam`, `OFX::BooleanParam`, or `OFX::IntParam`:
- Base Color: `hsv_pick` (RGB color picker)
- Add/Sub Colors: 
  - `enable_add` (Bool), `color_add` (RGB color picker)
  - `enable_sub` (Bool), `color_sub` (RGB color picker)
- Hue: `hue_spread` (0.0 to 0.5), `hue_falloff` (0.001 to 0.5)
- Sat: `sat_spread` (0.0 to 1.0), `sat_falloff` (0.001 to 1.0)
- Val: `val_spread` (0.0 to 1.0), `val_falloff` (0.001 to 1.0)
- Matte Finesse: `matte_shrink` (0 to 50), `matte_grow` (0 to 50), `matte_blur` (0 to 100)
- Output Mode: `output_mode` (Choice: "Premultiplied RGBA", "Alpha Matte Only")

# MATH & ALGORITHM
Inside the rendering pipeline, process in these discrete passes:

1. Raw Matte Generation (Per-Pixel Multi-Matte):
   - Convert the input RGB pixel to HSV.
   - Create a lambda/helper function that takes a target RGB color (converted to HSV), and the spread/falloff params, and returns a float matte (0.0 to 1.0).
   - In the helper: Calculate distance between pixel HSV and target HSV (accounting for Hue 0.0-1.0 wrap). Use Hermite interpolation: `matte = 1.0 - smoothstep(spread, spread + falloff, distance)`. Combine H, S, V via multiplication or minimum.
   - Calculate `base_matte` using `hsv_pick`.
   - If `enable_add` is true, calculate `add_matte` using `color_add`. Combine: `combined_matte = max(base_matte, add_matte)`.
   - If `enable_sub` is true, calculate `sub_matte` using `color_sub`. Combine: `combined_matte = max(0.0f, combined_matte - sub_matte)`.

2. Matte Finesse (Spatial Morphological Ops):
   - Apply intermediate buffers as needed.
   - If `matte_shrink` > 0: Minimum filter (erosion).
   - If `matte_grow` > 0: Maximum filter (dilation).
   - If `matte_blur` > 0: Box or gaussian blur convolution.

3. Final Output:
   - Apply the finessed matte to the Output clip's Alpha channel.
   - If `output_mode` is "Premultiplied", multiply the RGB channels by the new Alpha.

# DELIVERABLES
1. `CMakeLists.txt` (structured to link against openfx-cplusplus).
2. `SecondaryKeyer.h` (Class definitions, RoI overrides).
3. `SecondaryKeyer.cpp` (Implementation, Describe action, instance creation, and the multi-threaded spatial render loop).
4. Brief instructions on how to compile this on Linux using GCC.