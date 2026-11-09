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

EngageFrames and RampFrames control a zoom function on menu enter and exit, previously exiting a menu/loading in caused the zoom and not-zoomed view to fight and flicker, this jank-i-ly fixes that by employing EngageFrames (how many frames after unpausing until it starts zooming in) and RampFrames (how many frames until fully zoomed)

The logging feature takes 'snapshots' of the graphics pipeline under the hood upon initiating a hot key (default is backslash) for debugging and analysis.
# Build Me :3 
**Download [RDR2.Vulkan.Overscan_source.zip](https://github.com/kkanjitwotimes/RDR2-SSR-Fix/releases/download/v0.0.1/RDR2.Vulkan.Overscan_source.zip)**

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
