RDR2 Vulkan Overscan*

A customizable mod that effectively crops the viewport, allowing screen-space to see 'off-screen', mitigating edge screen artifacting.

Put RDR2OverscanVK.asi inside the RDR2 root

On first run it creates the config file (RDR2Overscan.ini) next to the ASI, you can edit it while the game runs (checked about once per second, reloaded only when the file has been saved)

Zoom is responsible for the amount of, well, zoom, higher number = less artifacts but narrower FOV.

HoldZoomOnPause keeps the zoom while paused so the background no longer unzooms and rezooms. 

Sharpness enables AMDs CAS sharpening on the zoomed, 3D scene only, avoiding UI, it's configurable 0.0-1.0, start at 0.5. Report bugs!

PauseGapFix finally kills the annoying ass flicker on unpause!

Logging is better specified in the configurable INI, used for debugging/analysis :p

If you experience any glitches, check the `Debug` section in the ini!
