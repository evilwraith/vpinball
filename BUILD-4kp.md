# Building for the AtGames cabinets (ALP4K / HDP)

This branch is upstream 10.8.1 (`2c6782d`) plus the RK3588 cabinet work. Everything below runs on
the aarch64 build host itself — there is no cross-compilation step.

## The build is not the 10.8.0 build

10.8.0 kept one `CMakeLists.txt` per target under `standalone/cmake/`, and you copied the one you
wanted over the root file. **10.8.1 has a single root `CMakeLists.txt`** driven by cache variables,
so there is no `cp make/CMakeLists_*.txt CMakeLists.txt` step and no such file to copy. Passing
`-DRENDERER=…` is what selects the backend now.

## Build

```
BUILD_TYPE=Release ./platforms/linux-aarch64/external.sh

cmake -DRENDERER=BGFX -DPLATFORM=linux -DARCH=aarch64 -DBUILD_RK3588=ON \
      -DCMAKE_BUILD_TYPE=Release -B build/Release
cmake --build build/Release -- -j9
```

A cold build of the whole tree at `-j9` exhausts memory on an 8 GB host and dies with
`cc1plus: Killed signal terminated program`, which reads like a compiler bug and is not one. Use
`-j4` for the first build; `-j9` is fine incrementally.

Incremental rebuilds are just the last line. `external.sh` is expensive (it builds SDL3, bgfx,
FFmpeg, PinMAME, libdmdutil and the rest from source) but it caches per-dependency in
`external/linux-aarch64/Release/*/cache.txt`, so re-running it after a `config.sh` edit only
rebuilds what actually changed.

The output binary is `build/Release/VPinballX_BGFX`, with the plugins beside it in
`build/Release/plugins/`.

### What `external.sh` pulls that upstream does not

- **SDL3 from `evilwraith/SDL`**, not `libsdl-org` — see `SDL_REPO`/`SDL_SHA` in
  `platforms/config.sh`. Stock SDL3 enables `DRM_CLIENT_CAP_ATOMIC` on KMSDRM, which this hardware's
  kernel does not survive; the fork forces the legacy modesetting path.
- **`vpx-patches/*.patch`**, applied to the extracted SDL and bgfx trees. `VPX_PATCH_DIR` overrides
  the location if you need to point somewhere else.
- **header-only fmtlib**, copied to `third-party/include/fmt/`. It is fetched unconditionally
  because `external.sh` cannot know which compiler CMake will pick; it is only ever *included* when
  the toolchain has no `<format>`, via the shim below.
- **bgfx built GLES-only** (`BGFX_CONFIG_RENDERER_OPENGL=0`). libmali on this device has no desktop
  GL, and bgfx's shared EGL context binds `EGL_OPENGL_API` at compile time when the GL renderer is
  enabled, which fails with `EGL_BAD_PARAMETER`.

Bumping any of these means bumping the matching `*_PATCHSET` in `platforms/config.sh` too, or the
cache check will not notice and you will keep linking the old library.

### Toolchain note

The cabinets' build host is Debian 12 / GCC 12, which has no `<format>`. The root `CMakeLists.txt`
probes for it and only falls back to the bundled fmtlib shim in
`third-party/include/compat/cxx-format/` when it is genuinely absent, so on GCC 13+ this is a no-op.

### System packages

Beyond upstream's Linux requirements, `BUILD_RK3588=ON` links libdrm, gbm and EGL directly (VPX owns
DRM presentation on KMSDRM — see `standalone/KmsBgfxPresenter.h`):

```
sudo apt install libdrm-dev libgbm-dev libegl1-mesa-dev
```

## Packaging a cart

Packaging lives **outside this tree**, at the workspace root, so that nothing cabinet-specific has to
be carried on an upstream-shaped branch:

```
cd /workspaces/vpinball
./build-bundle.sh
```

It packages only -- build here first, then package. The root holds the three things a cart needs that
this repo deliberately does not: `build-bundle.sh`, `pack_bundle.sh`, and `prebuilt/` -- the cabinets'
own Mali/EGL userspace (`libmali.so.1`, `libEGL.so.1`, `libGLESv2.so.2`, `libgbm.so.1`,
`libdrm.so.2`, `libstdc++.so.6`), which VPX will not run correctly without.

It reads this tree's `build/Release` and takes the renderer from whichever `VPinballX_*` binary is
actually there, so it cannot drift out of sync with your cmake invocation. Run
`./build-bundle.sh --help` at the root for the overrides.

The cart is written to the workspace root as `VPinballX_GL-<version>-<sha>-<timestamp>-alp4k`. Deploy
by streaming it -- never `scp`, which silently corrupts large writes to the cabinets' fuseblk USB:

```
cat <cart> | ssh root@<cabinet> 'cat > /media/usb0/<cart>'
ssh root@<cabinet> 'md5sum /media/usb0/<cart>'    # must match the md5 printed above
```
