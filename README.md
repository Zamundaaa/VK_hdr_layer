## Vulkan Wayland HDR WSI Layer

Vulkan layer utilizing a small color management / HDR protocol for experimentation. Upstream protocol proposal for color management is here: [wp_color_management](https://gitlab.freedesktop.org/wayland/wayland-protocols/-/merge_requests/14).

Implements the following vulkan extensions, if the protocol is supported by the compositor.
- [VK_EXT_swapchain_colorspace](https://registry.khronos.org/vulkan/specs/1.3-extensions/man/html/VK_EXT_swapchain_colorspace.html)
- [VK_EXT_hdr_metadata](https://registry.khronos.org/vulkan/specs/1.3-extensions/man/html/VK_EXT_hdr_metadata.html)

KWin git master supports the protocol, and so will the first Plasma 6 beta and 6.0 release.

### Testing with gamescope

There aren't many vulkan clients to choose from right now, that run on wayland and can make use of the previously mentioned extensions. One of these clients is [`gamescope`](https://github.com/ValveSoftware/gamescope), which can run nested as a wayland client. As such it can forward HDR metadata of HDR windows games running inside of it via DXVK.

Given gamescope utilizes it's own vulkan layer and creative.. hacks to support this, setting up can be a bit convoluted.
You want to enable this layer for gamescope, but not for it's clients.

Here is an example command line (assuming this layer has been installed to the system as an implicit layer), which enables this layer for gamescope and enables the gamescope layer and HDR for games in the session:
`ENABLE_HDR_WSI=1 gamescope --hdr-enabled --hdr-debug-force-output --steam -- env ENABLE_GAMESCOPE_WSI=1 DXVK_HDR=1 DISABLE_HDR_WSI=1 steam -bigpicture`

Another client that works is Quake II RTX, when run in Wayland native mode. To do that, put `SDL_VIDEODRIVER=wayland ENABLE_HDR_WSI=1 %command%` into its launch arguments.

Debugging what layers are being loaded can be done by setting `VK_LOADER_DEBUG=error,warn,info`.

### Testing with Wine Wayland

As of [Wine 9.0](https://gitlab.winehq.org/wine/wine/-/releases/wine-9.0), Wine's native wayland mode supports HDR through DXVK.    
To enable it, run `wine reg.exe add HKCU\\Software\\Wine\\Drivers /v Graphics /d x11,wayland` inside your Wine Prefix, Then unset the `DISPLAY` variable.  
Make sure you set `ENABLE_HDR_WSI=1` when running an application or game.

### Testing with Video Players

[mpv](https://mpv.io/) Supports HDR Video playback through Vulkan's HDR Surface.    
To play an HDR video, set `ENABLE_HDR_WSI=1` and run mpv with `--vo=gpu-next --target-colorspace-hint --gpu-api=vulkan --gpu-context=waylandvk`