# RDR2-SSR-Fix
A customizable ASI mod for RDR2 that effectively crops the viewport, allowing screen-space to see 'off-screen', mitigating edge screen artifacting under SSR/SSS.

**_This is Vulkan only, it will not be coming to directx, see `Sources` down below for a DX12 version._**
# AI Disclosure
I'm no programmer, so I vibecoded this project :(

Beyond this readme and the 12 messages I sent to claude, this project is entirely AI generated.
# Usage
Download RDR2.Vulkan.Overscan.zip from the `Releases` tab

Put RDR2OverscanVK.asi inside the RDR2 root, requires an ASI loader

On first run it creates the config file (RDR2Overscan.ini) next to the ASI, you can edit it while the game runs (checks for change once a second, reloads when changed)

Zoom is responsible for the amount of, well, zoom, higher number = less artifacts but narrower FOV.

HoldZoomOnPause keeps the zoom while paused so the background no longer unzooms and rezooms. 

Sharpness enables AMDs CAS sharpening on the zoomed, 3D scene only, avoiding UI, it's configurable 0.0-1.0, start at 0.5. Report bugs!

PauseGapFix finally kills the annoying ass flicker on unpause!

Logging is better specified in the configurable INI, used for debugging/analysis :p

If you experience any glitches, check the `Debug` section in the ini!
# Build Me :3 
**Download RDR2.Vulkan.Overscan_source.zip for the version you're building from the `Releases` tab**

This is above my larp level paygrade, here's what claude says:

**What you need:**

**1. A MinGW-w64 x86_64 compiler.** I built with llvm-mingw (x86_64-w64-mingw32-clang++). On Windows, the easiest route is MSYS2 with pacman -S mingw-w64-clang-x86_64-toolchain or mingw-w64-ucrt-x86_64-gcc, or the prebuilt llvm-mingw release. The build is 64-bit only, because RDR2 is 64-bit.

**2. Vulkan headers.** Clone or download KhronosGroup/Vulkan-Headers and point the build at its include folder. You don’t need the Vulkan SDK or any libs, because the build uses -DVK_NO_PROTOTYPES and doesn’t link vulkan-1.

**3. Microsoft Detours source.** Clone microsoft/Detours. build.sh compiles detours.cpp, disasm.cpp, modules.cpp, image.cpp and creatwth.cpp from src/ directly, with -DDETOURS_X64. You don’t need to build the library with nmake.

**4. A bash shell to run build.sh: MSYS2, Git Bash or WSL.** Without one, the script is only a few compiler commands that you can copy into a VS Code task.

**Setup:**

Edit the three path variables at the top of build.sh: TC (toolchain), VKH (Vulkan headers include) and DET (Detours src). They default to my session’s paths.

In VS Code, install the C/C++ extension and add a task that runs bash build.sh. In the C/C++ settings, set the compiler path to the MinGW clang or g++ and add the Vulkan headers to the include path.

The output is build/RDR2OverscanVK.asi.
# Sources
**Shoutout [linehand](https://www.nexusmods.com/profile/linehand/mods?gameId=3024), they're my direct inspiration for this project, having made a DX12 version, this acts like a Vulkan alternative**

ASI loader: https://github.com/ThirteenAG/Ultimate-ASI-Loader

Mod on Nexus: https://www.nexusmods.com/reddeadredemption2/mods/11546

<img width="1917" height="1078" alt="Screenshot 2026-11-09 023551" src="https://github.com/user-attachments/assets/44c5fdcb-9ebe-4a72-8c58-e5954c9e6df5" />
<img width="1917" height="1076" alt="Screenshot 2026-11-09 171541" src="https://github.com/user-attachments/assets/5b26a51d-2c7b-42c1-8c58-c211d4e5d2e3" />
