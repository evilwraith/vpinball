#!/bin/bash

set -e

# VPINBALL/4kp: build SDL3 from evilwraith/SDL (branch 4kp1081), not libsdl-org. That fork forces
# the KMSDRM backend down the LEGACY modesetting path; stock SDL3 enables DRM_CLIENT_CAP_ATOMIC,
# and the SDL2 backend this fork shipped for years has no atomic support at all. See
# backport/1081-phase1-log.md F17/F18. Upstream SDL_SHA bumps name libsdl-org and do not apply.
SDL_REPO=evilwraith/SDL
SDL_SHA=474330f01b9ee58aa25b9498d5beff3b9180f669
SDL_PATCHSET=kmsdrm-force-legacy-consistency-surface-size-002
SDL_IMAGE_SHA=f661fa1ad24ab1b81e43662532f9a6a9fcf67ea6
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
BGFX_PATCHSET=kmsdrm-gbm-egl-gles-only-profiler-skippresent-primaryonly-012
BGFX_CMAKE_VERSION=1.157.9472-571
BGFX_PATCH_SHA=0233cd89d403553bc05b0f965a013dcc48b2d4d1
PINMAME_SHA=01a86af49af8fdb8436f920ed5fc9e126844b972
OPENXR_SHA=2b99fec95e9cdf352c1a98e9cb23bf4def1cf8e6
LIBDMDUTIL_SHA=8f9c0b44a492af3c7dd58b90a5a25d9b6de9d06a
LIBALTSOUND_SHA=f4b790a19ae45a9f93ae0051df6933800c7a6446
LIBDOF_SHA=afc2be6e79644a78670be2f4de3e83daa0baaa5f
# VPINBALL/4kp: FFmpeg comes from nyanmisaka/ffmpeg-rockchip, not FFmpeg/FFmpeg, and is built
# against rkmpp + rkrga so the RK3588 hardware video path exists at all (h264_rkmpp / hevc_rkmpp
# decoders and encoders, and RGA/im2d colour conversion). Stock FFmpeg has none of it. The cabinets
# keep hardware decode OFF by default -- librockchip_mpp has crashed -- but RGA colour convert and
# the encoders are wanted, and neither is reachable without this build. Upstream FFMPEG_SHA bumps
# name the stock repo and do not apply here.
FFMPEG_REPO=nyanmisaka/ffmpeg-rockchip
FFMPEG_SHA=388741a3544b92cf525f1cb3746ba9fb8f301d9a
FFMPEG_PATCHSET=rkmpp-rkrga-001
RKMPP_REPO=https://gitee.com/nyanmisaka/mpp.git
RKMPP_BRANCH=develop
RKRGA_REPO=https://gitee.com/nyanmisaka/rga.git
RKRGA_BRANCH=jellyfin-rga
LIBWINEVBS_SHA=048864920803a6d60534a8e4802fe1d03444e282
LIBZIP_SHA=6f8a0cdd24a0dc6cce9dac4a7679da784ab124ea
LIBBACKTRACE_SHA=0b9b49cf4a2c9229fc052d6716e1528b2f23e91a

if [ -z "${BUILD_TYPE}" ]; then
   BUILD_TYPE="Release"
fi

echo "Build type: ${BUILD_TYPE}"
echo ""
