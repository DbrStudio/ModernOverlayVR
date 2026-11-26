# ModernOverlayVR

Display EDMCModernOverlay broadcasts inside Elite Dangerous in VR on Linux. The EDMC plugin runs the renderer and the native OpenXR layer composites its output. Elite must already run through a **native Linux OpenXR loader** (for example through a working OpenComposite setup).

## Install

Install CMake, a C++17 compiler, OpenXR and Vulkan development files, and nlohmann-json. The host renderer uses /usr/bin/python3 with PySide6. Install and enable EDMCModernOverlay in the same EDMC plugins directory. Flatpak EDMC must be able to run the host renderer via flatpak-spawn --host.

From the checkout, run one of the following. Close EDMC and Elite before updating.

```sh
./install.sh --edmc native
./install.sh --edmc flatpak
./install.sh --prefix "/absolute/install directory" --plugins "/absolute/EDMC/plugins"
```

The default prefix is $XDG_DATA_HOME/ModernOverlayVR (normally ~/.local/share/ModernOverlayVR). With no arguments the installer selects the existing Flatpak plugins directory if present, otherwise the native XDG data directory. If both exist, choose explicitly. For a custom EDMC setup, use the plugins directory shown in EDMC settings. Re-running the installer updates both components. Display settings and frame files live in edmo-vr alongside the plugins directory.

## Launch

Start EDMC and enable **Start VR overlay with EDMC** in **ModernOverlay VR** preferences. Choose **Room-locked (follows Elite recenter)** to use Elite’s HMD recenter binding. Click **Apply** to change the display while Elite is running.

**Steam:** Keep the environment assignments from your working VR launch configuration. Insert the installed wrapper immediately before %command%. For example, replacing the path with your actual absolute path:

```sh
PROTON_USE_EXTERNAL_VR=1 PRESSURE_VESSEL_IMPORT_OPENXR_1_RUNTIMES=1 "/absolute/install directory/elite-layer.sh" live %command%
```

**Heroic:** In the game's advanced settings, set the wrapper executable to the installed elite-layer.sh using its absolute path. Leave wrapper arguments empty for live mode. Keep the Wine/Proton and external VR settings that already launch Elite in VR. For sandboxed launchers, grant read access to the install directory and EDMC's edmo-vr directory at their original host paths.

The wrapper accepts an optional live, panel (static test panel), or trace mode followed by the original command and arguments. It activates the layer for that invocation without inspecting any app ID or Wine prefix. Layer diagnostics go to stderr by default. Set EDMO_XR_LOG to an absolute writable file to capture them.

## Build and test

```sh
cmake -S . -B build
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Vulkan-dependent loader tests skip when no compatible device is available. To exercise the optional PlanetPOI broadcast replay with both plugins installed, set EDMO_PLUGINS to the absolute EDMC plugins directory and run python3 tests/modern_replay.py.
