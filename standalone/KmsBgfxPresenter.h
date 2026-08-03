#pragma once

// VPX-owned DRM presentation for BGFX on KMSDRM.
//
// Upstream 10.8.1 has no DRM presentation code at all: SDL creates the gbm_surface, BGFX renders
// into it and calls eglSwapBuffers, and nothing ever scans the resulting front buffer out. On a
// desktop that is fine because SDL_GL_SwapWindow does the flip, but we deliberately take SDL out of
// the present path (SDL_KMSDRM_SKIP_EGL_SURFACE) so that SDL and BGFX do not both own presentation
// state for the same surface. Something therefore has to do the flip, and that something is us --
// see backport/VPXSummary.md section 5 ("BGFX render, VPX page flip") and section 10 item 4.
//
// This is the atomic + IN_FENCE_FD path, ported from the fork's proven playfield implementation in
// current-gl (RenderDevice.cpp PresentOwnedPlayfieldGBMScanout / discover_playfield_atomic_props).
// The legacy drmModePageFlip path was measured worse on this hardware: see VPXSummary.md section 20,
// where atomic + an explicit fence removed the flipper-input lag that the legacy ASYNC path showed.
//
// HARD CONTRACTS -- violating these hangs or reboots the device (see CLAUDE.md, "Display / present
// pipeline"). They are not style preferences:
//
//   1. IN_FENCE_FD is NOT consumed by the kernel. The atomic ioctl only refcounts the underlying
//      struct file; the userspace fd stays open. It MUST be close()d after every commit, success or
//      failure. Leaking it burns one fd per frame, exhausts the process fd table in ~30 s at 60 fps,
//      and cascades into a VOP2 hard hang that needs a power cycle.
//   2. Every commit carries FULL plane geometry (CRTC_X/Y/W/H + SRC_X/Y/W/H, SRC in 16.16 fixed
//      point). An FB_ID-only commit is treated by VOP2 as a full plane reconfigure and serialises,
//      which measured 1.8ms -> 7.6ms of DRM time.
//   3. At most ONE in-flight atomic commit per CRTC. A second before the first's PAGE_FLIP_EVENT
//      arrives returns EBUSY, so the previous flip is drained before the next commit is built.
//   4. The previously scanned-out BO is released only AFTER the new one has been latched, otherwise
//      the display reads freed memory.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <unistd.h>
#include <poll.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include "KmsScanoutSlots.h"

namespace VPX::Kms
{

struct AtomicProps
{
   uint32_t planeId = 0;
   uint32_t rotation = 0;        // "rotation" prop id, 0 when the plane has none
   uint64_t rotationMask = 0;    // supported bits (rotate-0=0x1, rotate-90=0x2, rotate-270=0x8, ...)
   uint32_t fbId = 0, crtcId = 0, inFenceFd = 0;
   uint32_t crtcX = 0, crtcY = 0, crtcW = 0, crtcH = 0;
   uint32_t srcX = 0, srcY = 0, srcW = 0, srcH = 0;

   bool complete() const
   {
      return planeId && fbId && crtcId && crtcX && crtcY && crtcW && crtcH && srcX && srcY && srcW && srcH;
   }
};

// Discover the primary plane driving `crtcId` and cache every property ID we set per frame. Looking
// these up per commit would be a per-frame ioctl storm, and a missing ID must fail here rather than
// silently produce a partial commit.
inline bool DiscoverAtomicProps(int drmFd, uint32_t crtcId, AtomicProps& out)
{
   out = AtomicProps {};
   if (drmFd < 0 || crtcId == 0)
      return false;

   // Sticky on the fd, and required before planes report their type at all.
   if (drmSetClientCap(drmFd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) != 0)
      return false;
   if (drmSetClientCap(drmFd, DRM_CLIENT_CAP_ATOMIC, 1) != 0)
      return false;

   int crtcIndex = -1;
   if (drmModeResPtr res = drmModeGetResources(drmFd))
   {
      for (int i = 0; i < res->count_crtcs; ++i)
         if (res->crtcs[i] == crtcId)
         {
            crtcIndex = i;
            break;
         }
      drmModeFreeResources(res);
   }
   if (crtcIndex < 0)
      return false;
   const uint32_t crtcMask = 1u << crtcIndex;

   drmModePlaneResPtr planeRes = drmModeGetPlaneResources(drmFd);
   if (!planeRes)
      return false;

   for (uint32_t i = 0; i < planeRes->count_planes && out.planeId == 0; ++i)
   {
      const uint32_t planeId = planeRes->planes[i];
      drmModePlanePtr plane = drmModeGetPlane(drmFd, planeId);
      if (!plane)
         continue;
      const uint32_t possible = plane->possible_crtcs;
      const uint32_t bound = plane->crtc_id;
      drmModeFreePlane(plane);
      if ((possible & crtcMask) == 0)
         continue;
      // RK3588 VOP2 advertises planes on several CRTCs and reports crtc_id 0 until first bind, so a
      // plane already bound elsewhere belongs to another window's presenter -- skip it.
      if (bound != crtcId && bound != 0)
         continue;

      drmModeObjectPropertiesPtr props = drmModeObjectGetProperties(drmFd, planeId, DRM_MODE_OBJECT_PLANE);
      if (!props)
         continue;

      uint32_t type = UINT32_MAX;
      AtomicProps cand;
      for (uint32_t p = 0; p < props->count_props; ++p)
      {
         drmModePropertyPtr prop = drmModeGetProperty(drmFd, props->props[p]);
         if (!prop)
            continue;
         const char* n = prop->name;
         if (!strcmp(n, "type"))            type = (uint32_t)props->prop_values[p];
         else if (!strcmp(n, "FB_ID"))      cand.fbId = prop->prop_id;
         else if (!strcmp(n, "CRTC_ID"))    cand.crtcId = prop->prop_id;
         else if (!strcmp(n, "IN_FENCE_FD"))cand.inFenceFd = prop->prop_id;
         else if (!strcmp(n, "CRTC_X"))     cand.crtcX = prop->prop_id;
         else if (!strcmp(n, "CRTC_Y"))     cand.crtcY = prop->prop_id;
         else if (!strcmp(n, "CRTC_W"))     cand.crtcW = prop->prop_id;
         else if (!strcmp(n, "CRTC_H"))     cand.crtcH = prop->prop_id;
         else if (!strcmp(n, "SRC_X"))      cand.srcX = prop->prop_id;
         else if (!strcmp(n, "SRC_Y"))      cand.srcY = prop->prop_id;
         else if (!strcmp(n, "SRC_W"))      cand.srcW = prop->prop_id;
         else if (!strcmp(n, "SRC_H"))      cand.srcH = prop->prop_id;
         else if (!strcmp(n, "rotation"))
         {
            cand.rotation = prop->prop_id;
            // Bitmask enum: collect every supported bit so callers can ask "can this plane do 90?"
            for (int e = 0; e < prop->count_enums; ++e)
               cand.rotationMask |= (1ull << prop->enums[e].value);
         }
         drmModeFreeProperty(prop);
      }
      drmModeFreeObjectProperties(props);

      if (type == DRM_PLANE_TYPE_PRIMARY)
      {
         cand.planeId = planeId;
         if (cand.complete())
            out = cand;
      }
   }
   drmModeFreePlaneResources(planeRes);
   return out.complete();
}

// Probe whether this hardware can rotate at scanout, using DRM_MODE_ATOMIC_TEST_ONLY so nothing is
// ever actually committed. On RK3588 VOP2 the PRIMARY planes are Esmart windows (no rotation) while
// the idle OVERLAY planes are Cluster windows that advertise rotate-90/270 -- but Cluster rotation
// commonly requires AFBC-compressed buffers, and ours are plain XR24. TEST_ONLY is the only way to
// find out without risking a commit.
//
// Deliberately read-only: attaching a plane to a VOP2 Video Port is a port re-mux == a MODESET, and
// doing that inside a per-frame NONBLOCK|PAGE_FLIP_EVENT commit returns EBUSY and cascades into
// every other CRTC's presenter. So this probe only ever TESTs.
inline void ProbeScanoutRotation(int drmFd, uint32_t crtcId, uint32_t fbId, uint32_t modeW, uint32_t modeH)
{
   if (drmFd < 0 || crtcId == 0 || fbId == 0)
      return;

   int crtcIndex = -1;
   if (drmModeResPtr res = drmModeGetResources(drmFd))
   {
      for (int i = 0; i < res->count_crtcs; ++i)
         if (res->crtcs[i] == crtcId) { crtcIndex = i; break; }
      drmModeFreeResources(res);
   }
   if (crtcIndex < 0)
      return;
   const uint32_t crtcMask = 1u << crtcIndex;

   drmModePlaneResPtr planeRes = drmModeGetPlaneResources(drmFd);
   if (!planeRes)
      return;

   for (uint32_t i = 0; i < planeRes->count_planes; ++i)
   {
      const uint32_t planeId = planeRes->planes[i];
      drmModePlanePtr plane = drmModeGetPlane(drmFd, planeId);
      if (!plane)
         continue;
      const bool usable = (plane->possible_crtcs & crtcMask) != 0 && (plane->crtc_id == 0 || plane->crtc_id == crtcId);
      drmModeFreePlane(plane);
      if (!usable)
         continue;

      AtomicProps p;
      drmModeObjectPropertiesPtr props = drmModeObjectGetProperties(drmFd, planeId, DRM_MODE_OBJECT_PLANE);
      if (!props)
         continue;
      uint32_t type = UINT32_MAX;
      for (uint32_t j = 0; j < props->count_props; ++j)
      {
         drmModePropertyPtr prop = drmModeGetProperty(drmFd, props->props[j]);
         if (!prop) continue;
         const char* n = prop->name;
         if      (!strcmp(n, "type"))     type = (uint32_t)props->prop_values[j];
         else if (!strcmp(n, "FB_ID"))    p.fbId = prop->prop_id;
         else if (!strcmp(n, "CRTC_ID"))  p.crtcId = prop->prop_id;
         else if (!strcmp(n, "CRTC_X"))   p.crtcX = prop->prop_id;
         else if (!strcmp(n, "CRTC_Y"))   p.crtcY = prop->prop_id;
         else if (!strcmp(n, "CRTC_W"))   p.crtcW = prop->prop_id;
         else if (!strcmp(n, "CRTC_H"))   p.crtcH = prop->prop_id;
         else if (!strcmp(n, "SRC_X"))    p.srcX = prop->prop_id;
         else if (!strcmp(n, "SRC_Y"))    p.srcY = prop->prop_id;
         else if (!strcmp(n, "SRC_W"))    p.srcW = prop->prop_id;
         else if (!strcmp(n, "SRC_H"))    p.srcH = prop->prop_id;
         else if (!strcmp(n, "rotation"))
         {
            p.rotation = prop->prop_id;
            for (int e = 0; e < prop->count_enums; ++e)
               p.rotationMask |= (1ull << prop->enums[e].value);
         }
         drmModeFreeProperty(prop);
      }
      drmModeFreeObjectProperties(props);

      constexpr uint64_t kRotate90 = 0x2;
      const bool claims90 = (p.rotationMask & kRotate90) != 0;
      if (p.rotation == 0 || !claims90 || p.fbId == 0)
         continue;

      // Rotated: CRTC extents are post-rotation, so a landscape source lands on a portrait panel.
      drmModeAtomicReqPtr req = drmModeAtomicAlloc();
      if (!req)
         continue;
      drmModeAtomicAddProperty(req, planeId, p.fbId, fbId);
      drmModeAtomicAddProperty(req, planeId, p.crtcId, crtcId);
      drmModeAtomicAddProperty(req, planeId, p.crtcX, 0);
      drmModeAtomicAddProperty(req, planeId, p.crtcY, 0);
      drmModeAtomicAddProperty(req, planeId, p.crtcW, modeW);
      drmModeAtomicAddProperty(req, planeId, p.crtcH, modeH);
      drmModeAtomicAddProperty(req, planeId, p.srcX, 0);
      drmModeAtomicAddProperty(req, planeId, p.srcY, 0);
      drmModeAtomicAddProperty(req, planeId, p.srcW, (uint64_t)modeH << 16); // swapped for 90 deg
      drmModeAtomicAddProperty(req, planeId, p.srcH, (uint64_t)modeW << 16);
      drmModeAtomicAddProperty(req, planeId, p.rotation, kRotate90);

      const int rc90 = drmModeAtomicCommit(drmFd, req, DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET, nullptr);
      drmModeAtomicFree(req);

      // CONTROL: the same commit with rotate-0 and unswapped geometry. Without this the result is
      // ambiguous -- a rejection could mean "this plane cannot rotate" OR "this plane cannot be
      // attached to this CRTC at all". On VOP2 the latter is likely, because possible_crtcs is
      // over-permissive and Cluster windows are wired to particular Video Ports; the driver then
      // reports ENOSPC (no free window on the port) rather than EINVAL (bad rotation).
      drmModeAtomicReqPtr ctl = drmModeAtomicAlloc();
      int rc0 = -1;
      if (ctl)
      {
         drmModeAtomicAddProperty(ctl, planeId, p.fbId, fbId);
         drmModeAtomicAddProperty(ctl, planeId, p.crtcId, crtcId);
         drmModeAtomicAddProperty(ctl, planeId, p.crtcX, 0);
         drmModeAtomicAddProperty(ctl, planeId, p.crtcY, 0);
         drmModeAtomicAddProperty(ctl, planeId, p.crtcW, modeW);
         drmModeAtomicAddProperty(ctl, planeId, p.crtcH, modeH);
         drmModeAtomicAddProperty(ctl, planeId, p.srcX, 0);
         drmModeAtomicAddProperty(ctl, planeId, p.srcY, 0);
         drmModeAtomicAddProperty(ctl, planeId, p.srcW, (uint64_t)modeW << 16);
         drmModeAtomicAddProperty(ctl, planeId, p.srcH, (uint64_t)modeH << 16);
         drmModeAtomicAddProperty(ctl, planeId, p.rotation, 0x1 /* rotate-0 */);
         rc0 = drmModeAtomicCommit(drmFd, ctl, DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET, nullptr);
         drmModeAtomicFree(ctl);
      }

      const char* verdict =
           (rc90 == 0)              ? "ROTATION VIABLE"
         : (rc0 == 0 && rc90 != 0)  ? "plane attaches, but rotate-90 refused -> rotation unsupported"
         : (rc0 != 0)               ? "plane cannot attach to this CRTC at all -> not a rotation limit"
                                    : "unknown";

      PLOGD.printf("[kms_rot_probe] crtc=%u plane=%u type=%s mask=0x%llx  rotate-90 -> %s (%s)   control rotate-0 -> %s (%s)   VERDICT: %s",
         crtcId, planeId, type == DRM_PLANE_TYPE_PRIMARY ? "primary" : (type == DRM_PLANE_TYPE_OVERLAY ? "overlay" : "cursor"),
         (unsigned long long)p.rotationMask,
         rc90 == 0 ? "ACCEPTED" : "rejected", rc90 == 0 ? "-" : strerror(-rc90),
         rc0 == 0 ? "ACCEPTED" : "rejected", rc0 == 0 ? "-" : strerror(-rc0),
         verdict);
   }
   drmModeFreePlaneResources(planeRes);
}

// One presenter per KMSDRM output window.
class WindowPresenter
{
public:
   bool IsReady() const { return m_ready; }
   uint32_t GetCrtcId() const { return m_crtcId; }

   // Live output adjustment, ported verbatim in behaviour from the 10.8.0 fork's
   // apply_dmd_window_adjust_rect(): scale the panel-filling rect about its CENTRE, then shift by a
   // pixel offset. Deliberately not a width/height -- SDL cannot resize a window on KMSDRM, and a
   // relative scale is cabinet- and resolution-independent. scale 1 + offsets 0 is the identity, so
   // the default is exactly "fill the panel" and this cannot regress the untouched case.
   void SetAdjust(float scale, int offsetX, int offsetY)
   {
      // A non-positive scale means "unset", never "invisible". The playfield has no entry in the
      // Scale property array, so reading it yields 0 -- and clamping that to a minimum produced a
      // 1x1 destination rect, i.e. a black panel. Identity is the only safe interpretation.
      m_scale = (scale > 0.0f) ? std::max(0.05f, scale) : 1.0f;
      m_offsetX = offsetX;
      m_offsetY = offsetY;
   }

   bool Init(int drmFd, uint32_t crtcId, struct gbm_surface* surface, uint32_t modeW, uint32_t modeH)
   {
      m_drmFd = drmFd;
      m_crtcId = crtcId;
      m_surface = surface;
      m_modeW = modeW;
      m_modeH = modeH;
      m_ready = (drmFd >= 0) && crtcId && surface && DiscoverAtomicProps(drmFd, crtcId, m_props);
      return m_ready;
   }

   // Frame pacing phase 2, step 1: build the pool the render path will use. Runs once, allocates
   // the three slots, imports each into GL and registers a DRM framebuffer, and keeps them. Nothing
   // renders into them yet.
   //
   // The pool is held for the process lifetime rather than probed and released: releasing it
   // segfaulted inside libmali (pthread_mutex_lock on the render thread, from a driver object still
   // referencing what we had freed). current-gl keeps its slots for the same reason, and the real
   // implementation needs them to persist anyway.
   //
   // Uses a live surface buffer as the template so the slots match what the display is already
   // accepting -- guessing the format or the modifier is the easiest way to get a pool that builds
   // and then fails at commit time.
   const ScanoutSlots& GetOwnedSlots() const { return m_ownedSlots; }

   void ProbeOwnedScanout()
   {
      if (m_ownedScanoutProbed || !m_ready || m_prevBo == nullptr)
         return;
      m_ownedScanoutProbed = true;

      const uint32_t w = gbm_bo_get_width(m_prevBo);
      const uint32_t h = gbm_bo_get_height(m_prevBo);
      const uint32_t fmt = gbm_bo_get_format(m_prevBo);

      // The first attempt segfaulted with no output at all, so the probe could not say which step
      // it died on. Announce each step: the last line in the log then names the culprit.
      PLOGI.printf("[4kpDebug][owned_scanout] probe start: %ux%u fourcc 0x%08x", w, h, fmt);

      struct gbm_device* dev = gbm_bo_get_device(m_prevBo);
      EGLDisplay dpy = eglGetCurrentDisplay();
      PLOGI.printf("[4kpDebug][owned_scanout] gbm_device=%p egl_display=%p drm_fd=%d", (void*)dev, (void*)dpy, m_drmFd);

      if (dev == nullptr || dpy == EGL_NO_DISPLAY)
      {
         PLOGE << "[4kpDebug][owned_scanout] probe aborted: no gbm device or no current EGL display on this thread";
         return;
      }

      std::string err;
      const bool ok = m_ownedSlots.Init(m_drmFd, dev, dpy, w, h, fmt, err);

      if (ok)
         PLOGI.printf("[4kpDebug][owned_scanout] pool ready: %d slots %ux%u fourcc 0x%08x -- imported to GL, framebuffer complete, registered with DRM",
            m_ownedSlots.Count(), w, h, fmt);
      else
         PLOGE.printf("[4kpDebug][owned_scanout] pool FAILED: %s (%ux%u fourcc 0x%08x)", err.c_str(), w, h, fmt);
   }

   // The atomic commit itself, shared by the gbm_surface path and the owned-slot path so the four
   // hard contracts at the top of this file live in exactly one place. srcW/srcH describe the
   // BUFFER, which is not the mode whenever the buffer is deliberately smaller (BackBufferScale).
   bool CommitFb(const uint32_t fbId, const uint32_t srcW, const uint32_t srcH)
   {
      // Contract 1: mint the fence, and close it unconditionally below.
      const int fenceFd = CreateNativeFenceFd();

      drmModeAtomicReqPtr req = drmModeAtomicAlloc();

      int addFailed = 0;
      if (req)
      {
         const uint32_t p = m_props.planeId;
         addFailed |= drmModeAtomicAddProperty(req, p, m_props.fbId, fbId) < 0;
         addFailed |= drmModeAtomicAddProperty(req, p, m_props.crtcId, m_crtcId) < 0;
         if (fenceFd >= 0)
            addFailed |= drmModeAtomicAddProperty(req, p, m_props.inFenceFd, (uint64_t)fenceFd) < 0;
         // Contract 2: full geometry every commit. SRC_* are 16.16 fixed point.
         // Base rect fills the panel; the adjustment scales it about the centre and shifts it. Clamped
         // into the CRTC because a rect hanging off the edge is rejected outright, which would drop
         // the frame rather than merely look wrong.
         uint32_t dstW = static_cast<uint32_t>(static_cast<float>(m_modeW) * m_scale);
         uint32_t dstH = static_cast<uint32_t>(static_cast<float>(m_modeH) * m_scale);
         dstW = std::clamp(dstW, 1u, m_modeW);
         dstH = std::clamp(dstH, 1u, m_modeH);
         int dstX = static_cast<int>((m_modeW - dstW) / 2) + m_offsetX;
         int dstY = static_cast<int>((m_modeH - dstH) / 2) + m_offsetY;
         dstX = std::clamp(dstX, 0, static_cast<int>(m_modeW - dstW));
         dstY = std::clamp(dstY, 0, static_cast<int>(m_modeH - dstH));

         addFailed |= drmModeAtomicAddProperty(req, p, m_props.crtcX, static_cast<uint64_t>(dstX)) < 0;
         addFailed |= drmModeAtomicAddProperty(req, p, m_props.crtcY, static_cast<uint64_t>(dstY)) < 0;
         addFailed |= drmModeAtomicAddProperty(req, p, m_props.crtcW, dstW) < 0;
         addFailed |= drmModeAtomicAddProperty(req, p, m_props.crtcH, dstH) < 0;
         // SRC is the BUFFER, not the mode. They are equal only when rendering at native resolution;
         // under BackBufferScale < 1 the buffer is deliberately smaller and VOP2 scales SRC->CRTC at
         // scanout, which is the whole point (it deletes the GPU upscale pass and stops the GPU
         // writing a 4K surface). Hardcoding the mode here would tell the plane to read past the end
         // of a smaller buffer.
         addFailed |= drmModeAtomicAddProperty(req, p, m_props.srcX, 0) < 0;
         addFailed |= drmModeAtomicAddProperty(req, p, m_props.srcY, 0) < 0;
         addFailed |= drmModeAtomicAddProperty(req, p, m_props.srcW, (uint64_t)srcW << 16) < 0;
         addFailed |= drmModeAtomicAddProperty(req, p, m_props.srcH, (uint64_t)srcH << 16) < 0;
      }

      int ret = -1;
      if (req && !addFailed)
      {
         m_flipPending = true;
         ret = drmModeAtomicCommit(m_drmFd, req, DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT, this);
         if (ret != 0)
            m_flipPending = false;
      }
      if (req)
         drmModeAtomicFree(req);

      // Contract 1, unconditionally, before any branch on ret.
      if (fenceFd >= 0)
         close(fenceFd);

      if (ret != 0)
      {
         ++m_commitErrors;
         return false;
      }

      return true;
   }


   // Present a buffer we own rather than one the EGL surface handed us. No gbm_surface locking and
   // no buffer release: the slots are ours for the process lifetime, so the only contract that still
   // applies is one commit in flight per CRTC.
   bool PresentOwnedFb(const uint32_t fbId, const uint32_t srcW, const uint32_t srcH)
   {
      if (!m_ready || fbId == 0)
         return false;
      DrainPendingFlip(); // Contract 3
      return CommitFb(fbId, srcW, srcH);
   }

   // Call on the thread that owns the GL/EGL context, immediately after BGFX has swapped, so the
   // front buffer exists and eglGetCurrentDisplay() is valid for minting the fence.
   bool Present()
   {
      if (!m_ready)
         return false;

      // Contract 3: never have two commits in flight on one CRTC.
      DrainPendingFlip();

      // Contract 4: only NOW is the previously displayed buffer safe to recycle -- DrainPendingFlip
      // has confirmed the commit that replaced it has latched. Releasing it right after committing
      // (as this used to) hands back a buffer that is still being scanned out, and also means the
      // pipeline never needs more than two buffers at once. GBM allocates lazily, so that suppressed
      // allocation of a third: with only two, the producer must wait for a flip before it has
      // anywhere to draw, which caps throughput at exactly the point frame work exceeds the vblank
      // budget. Holding one frame longer creates the demand for a third.
      if (m_retiredBo)
      {
         gbm_surface_release_buffer(m_surface, m_retiredBo);
         m_retiredBo = nullptr;
      }

      struct gbm_bo* bo = gbm_surface_lock_front_buffer(m_surface);
      if (!bo)
      {
         // Producer starvation: nothing new has been rendered. Releasing the buffer we are holding
         // lets the surface recycle it rather than deadlocking, and the next frame retries.
         // Only ever give back the retired buffer -- m_prevBo is the one currently on screen.
         if (++m_starveCount > 3 && m_retiredBo)
         {
            gbm_surface_release_buffer(m_surface, m_retiredBo);
            m_retiredBo = nullptr;
            m_starveCount = 0;
         }
         return false;
      }
      m_starveCount = 0;

      // How deep is this gbm_surface actually? The producer can only run ahead of scanout if the
      // driver hands out more buffers than we hold: we keep the committed one plus the previous one,
      // so a 2-buffer surface forces eglSwapBuffers to block until a flip retires one -- which caps
      // throughput exactly when frame work exceeds the vblank budget. bgfx's numBackBuffers does not
      // apply here (only the D3D11 and Vulkan backends read it), so this is driver-decided and the
      // only way to know is to count distinct buffers. Bounded: at most kMaxTrackedBos lines per run.
      if (m_distinctBoCount < kMaxTrackedBos)
      {
         bool known = false;
         for (int i = 0; i < m_distinctBoCount; ++i)
            if (m_seenBos[i] == bo)
            {
               known = true;
               break;
            }
         if (!known)
         {
            m_seenBos[m_distinctBoCount++] = bo;
            PLOGI.printf("[4kpDebug][gbm_slots] crtc=%u distinct scanout buffers seen: %d", m_crtcId, m_distinctBoCount);
         }
      }

      const uint32_t fbId = GetOrCreateFb(bo);
      if (fbId == 0)
      {
         gbm_surface_release_buffer(m_surface, bo);
         return false;
      }

      if (!CommitFb(fbId, gbm_bo_get_width(bo), gbm_bo_get_height(bo)))
      {
         // Never scanned out, so it is safe to recycle right away.
         gbm_surface_release_buffer(m_surface, bo);
         return false;
      }
            // Diagnostic only, and the answer has not changed on this hardware: VOP2 refuses rotate-90 on
      // every plane because it needs AFBC. It costs two TEST_ONLY atomic commits per plane per CRTC
      // at startup, so run it only when debug logging is actually enabled (Release builds cap the
      // logger at info, see Logger.cpp).
      if (!m_rotationProbed)
      {
         m_rotationProbed = true;
         if (plog::get() != nullptr && plog::get()->checkSeverity(plog::debug))
            ProbeScanoutRotation(m_drmFd, m_crtcId, fbId, m_modeW, m_modeH);
      }

      // The buffer this one replaces cannot be recycled yet: the commit above is NONBLOCK, so it has
      // not latched and m_prevBo is still on screen. Hand it to the retire slot, which the next
      // present frees once DrainPendingFlip confirms the latch.
      m_retiredBo = m_prevBo;
      m_prevBo = bo;
      return true;
   }

   void Shutdown()
   {
      DrainPendingFlip();
      if (m_surface)
      {
         if (m_retiredBo)
            gbm_surface_release_buffer(m_surface, m_retiredBo);
         if (m_prevBo)
            gbm_surface_release_buffer(m_surface, m_prevBo);
      }
      m_retiredBo = nullptr;
      m_prevBo = nullptr;
      m_ready = false;
   }

   uint64_t GetCommitErrors() const { return m_commitErrors; }

private:
   // Per-BO framebuffer, cached on the BO itself so it is created once and destroyed with it.
   struct FbCache
   {
      int drmFd;
      uint32_t fbId;
   };

   static void DestroyFbCache(struct gbm_bo* bo, void* data)
   {
      (void)bo;
      FbCache* fb = static_cast<FbCache*>(data);
      if (fb)
      {
         if (fb->fbId)
            drmModeRmFB(fb->drmFd, fb->fbId);
         delete fb;
      }
   }

   uint32_t GetOrCreateFb(struct gbm_bo* bo)
   {
      if (FbCache* cached = static_cast<FbCache*>(gbm_bo_get_user_data(bo)))
         return cached->fbId;

      const uint32_t w = gbm_bo_get_width(bo);
      const uint32_t h = gbm_bo_get_height(bo);
      const uint32_t stride = gbm_bo_get_stride(bo);
      const uint32_t handle = gbm_bo_get_handle(bo).u32;
      uint32_t fbId = 0;
      if (drmModeAddFB(m_drmFd, w, h, 24, 32, stride, handle, &fbId) != 0)
         return 0;

      gbm_bo_set_user_data(bo, new FbCache { m_drmFd, fbId }, DestroyFbCache);
      return fbId;
   }

   int CreateNativeFenceFd()
   {
      EGLDisplay dpy = eglGetCurrentDisplay();
      if (dpy == EGL_NO_DISPLAY)
         return -1;

      static auto createSync = (PFNEGLCREATESYNCKHRPROC)eglGetProcAddress("eglCreateSyncKHR");
      static auto destroySync = (PFNEGLDESTROYSYNCKHRPROC)eglGetProcAddress("eglDestroySyncKHR");
      static auto dupFence = (PFNEGLDUPNATIVEFENCEFDANDROIDPROC)eglGetProcAddress("eglDupNativeFenceFDANDROID");
      if (!createSync || !destroySync || !dupFence)
         return -1;

      EGLSyncKHR sync = createSync(dpy, EGL_SYNC_NATIVE_FENCE_ANDROID, nullptr);
      if (sync == EGL_NO_SYNC_KHR)
         return -1;
      // The fence must be flushed to the driver before it can be dup'd, or we hand the kernel a
      // fence that never signals and scanout stalls forever.
      glFlushIfAvailable();
      const int fd = dupFence(dpy, sync);
      destroySync(dpy, sync);
      return fd;
   }

   static void glFlushIfAvailable()
   {
      // Declared here rather than including a GL header: BGFX owns the GL loader, and we only need
      // the one entry point. eglGetProcAddress resolves it against the current GLES context.
      static auto flush = (void (*)(void))eglGetProcAddress("glFlush");
      if (flush)
         flush();
   }

   static void FlipHandler(int, unsigned int, unsigned int, unsigned int, unsigned int, void* data)
   {
      if (auto* self = static_cast<WindowPresenter*>(data))
         self->m_flipPending = false;
   }

   void DrainPendingFlip()
   {
      if (!m_flipPending || m_drmFd < 0)
         return;

      drmEventContext ev {};
      ev.version = 3;
      ev.page_flip_handler2 = &WindowPresenter::FlipHandler;

      // Bounded wait: a lost event must not wedge the render thread forever.
      for (int attempts = 0; m_flipPending && attempts < 3; ++attempts)
      {
         pollfd pfd { m_drmFd, POLLIN, 0 };
         if (poll(&pfd, 1, 35) <= 0)
            break;
         drmHandleEvent(m_drmFd, &ev);
      }
      m_flipPending = false;
   }

   int m_drmFd = -1;
   uint32_t m_crtcId = 0;
   struct gbm_surface* m_surface = nullptr;
   ScanoutSlots m_ownedSlots; // held for the process lifetime; see ProbeOwnedScanout
   bool m_ownedScanoutProbed = false;
   struct gbm_bo* m_prevBo = nullptr;    // currently on screen (or awaiting latch)
   struct gbm_bo* m_retiredBo = nullptr; // replaced on screen; freed after the next latch
   static constexpr int kMaxTrackedBos = 8;
   struct gbm_bo* m_seenBos[kMaxTrackedBos] {};
   int m_distinctBoCount = 0;
   AtomicProps m_props;
   uint32_t m_modeW = 0, m_modeH = 0;
   float m_scale = 1.0f;      // 1 == fill the panel
   int m_offsetX = 0, m_offsetY = 0;
   bool m_ready = false;
   bool m_flipPending = false;
   int m_starveCount = 0;
   bool m_rotationProbed = false;
   uint64_t m_commitErrors = 0;
};

} // namespace VPX::Kms
