#!/bin/bash

set -e

# VPINBALL/4kp: build SDL3 from evilwraith/SDL (branch 4kp1081), not libsdl-org. That fork forces
# the KMSDRM backend down the LEGACY modesetting path; stock SDL3 enables DRM_CLIENT_CAP_ATOMIC,
# and the SDL2 backend this fork shipped for years has no atomic support at all. See
# backport/1081-phase1-log.md F17/F18.
SDL_REPO=evilwraith/SDL
SDL_SHA=474330f01b9ee58aa25b9498d5beff3b9180f669
SDL_PATCHSET=kmsdrm-force-legacy-consistency-surface-size-002
SDL_IMAGE_SHA=bec9134a26c7d0f31b36d6083c25296e04cabff5
SDL_TTF_SHA=a1ce3670aec736ecbf0936c43f2f0cc53aa61e5b
FREEIMAGE_SHA=b1613452a0c3849d43ac877b154cf51ff9e078d3
# 4kp: scaled decode for PNG and EXR, matching the load-size flag PluginJPEG already honours
FREEIMAGE_PATCHSET=scaled-decode-png-exr-001
# 4kp: header-only fmtlib, used ONLY as the <format> implementation behind
# third-party/include/compat/cxx-format/ when the toolchain's libstdc++ has none (GCC < 13).
# Fetched unconditionally because external.sh cannot know the compiler CMake will pick; on GCC 13+
# the headers are simply never included.
FMT_SHA=11.1.4
# 4kp: KMSDRM/GBM EGL hunks ported from evilwraith/bgfx@03f25e9 (branch 4kp_fixes)
BGFX_PATCHSET=kmsdrm-gbm-egl-gles-only-profiler-skippresent-surfaceless-009
BGFX_CMAKE_VERSION=1.157.9447-569
BGFX_PATCH_SHA=8cd1aa31dff55f1855c05d6fee0b82e3c780c468
PINMAME_SHA=d32f0ad7275b733e25d4644193d93c578707fdce
OPENXR_SHA=b15ef6ce120dad1c7d3ff57039e73ba1a9f17102
LIBDMDUTIL_SHA=3485e2e0e1e9252148914cd613510ccad7bb56b5
LIBALTSOUND_SHA=f4b790a19ae45a9f93ae0051df6933800c7a6446
LIBDOF_SHA=eef645d9f5df618290962946c9e3e8ed30886639
# VPINBALL/4kp: FFmpeg comes from nyanmisaka/ffmpeg-rockchip, not FFmpeg/FFmpeg, and is built
# against rkmpp + rkrga so the RK3588 hardware video path exists at all (h264_rkmpp / hevc_rkmpp
# decoders and encoders, and RGA/im2d colour conversion). Stock FFmpeg has none of it. The cabinets
# keep hardware decode OFF by default -- librockchip_mpp has crashed -- but RGA colour convert and
# the encoders are wanted, and neither is reachable without this build.
FFMPEG_REPO=nyanmisaka/ffmpeg-rockchip
FFMPEG_SHA=388741a3544b92cf525f1cb3746ba9fb8f301d9a
FFMPEG_PATCHSET=rkmpp-rkrga-001
RKMPP_REPO=https://gitee.com/nyanmisaka/mpp.git
RKMPP_BRANCH=develop
RKRGA_REPO=https://gitee.com/nyanmisaka/rga.git
RKRGA_BRANCH=jellyfin-rga
LIBWINEVBS_SHA=bcc790e58d394b282c327feca2a7c921ca022e8d
LIBZIP_SHA=6f8a0cdd24a0dc6cce9dac4a7679da784ab124ea

if [ -z "${BUILD_TYPE}" ]; then
   BUILD_TYPE="Release"
fi

echo "Build type: ${BUILD_TYPE}"
echo ""
