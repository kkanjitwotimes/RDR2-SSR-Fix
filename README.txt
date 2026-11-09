RDR2 Vulkan Overscan*

A customizable mod that effectively crops the viewport, allowing screen-space to see 'off-screen', mitigating edge screen artifacting.

Put RDR2OverscanVK.asi inside the RDR2 root

On first run it creates the config file (RDR2Overscan.ini) next to the ASI, you can edit it while the game runs (checked about once per second, reloaded only when the file has been saved)

Zoom is responsible for the amount of, well, zoom, higher number = less artifacts but narrower FOV.

HoldZoomOnPause keeps the zoom while paused so the background no longer unzooms and rezooms. 

EngageFrames and RampFrames control a zoom function on menu enter and exit, previously exiting a menu/loading in caused the zoom and not-zoomed view to fight and flicker, this jank-i-ly fixes that by employing EngageFrames (how many frames after unpausing until it starts zooming in) and RampFrames (how many frames until fully zoomed) {may work differently with the introduction of zoom holding}

Sharpness enables AMDs CAS sharpening on the zoomed, 3D scene only, avoiding UI, it's configurable 0.0-1.0, start at 0.5. Report bugs!

Logging is better specified in the configurable INI, used for debugging/analysis :p
