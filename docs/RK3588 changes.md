
# RK3588 Changes

This document tracks RK3588/ALP4K-specific changes. All RK-specific code paths are guarded with `__RK3588__`.

## Rendering / KMSDRM (BGFX)
- Enable BGFX GBM interop on KMSDRM, pass `SDL.window.kmsdrm.gbm_surface` to BGFX, and set `BGFX_USE_GBM`.
- Add KMSDRM flip/present handling (GBM buffer lock, DRM page flip, starvation recovery).
- Force ancillary windows to fullscreen on KMSDRM to avoid unreliable non-fullscreen behavior.
- Allow playfield scaling by using windowed logical size for BGFX init/backbuffer on RK3588.

## UI / ScoreView
- LiveUI: rotation, scaling, and clip-rect correction for portrait/rotated displays on RK3588.
- PerfUI: scale HUD text for 4K readability.
- ScoreView: rotate 90° CW and correct output centering when on KMSDRM.

## Input
- Force playfield focus handling and ImGui event rotation for RK3588 input routing.

## Stability / Startup
- Use preference path if `SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS)` returns null on RK3588.
- Mark prerender passes to reduce KMSDRM buffer starvation during static prepass.

## Non‑RK3588 portability changes (not platform‑specific)
- Replace `std::format`/`std::ranges` usage in plugins and `ViewSetup.cpp` for toolchain compatibility.
- Add audio device logging (SDL/Pulse) for diagnostics.

