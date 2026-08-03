#pragma once

// VPX-owned scanout buffers for the playfield (frame pacing phase 2).
//
// WHY THIS EXISTS
//
// The playfield's eglSwapBuffers blocks the render thread until the GPU has finished the frame:
// measured on TAF, fencing the GPU before the swap leaves the swap itself costing 0.04 ms, so the
// ~15 ms it was charging is pure GPU wait. Nothing else can run on that thread meanwhile, so the
// frame becomes issuing + GPU + present laid end to end -- 24.7 ms, ~40 fps -- while 10.8.0 fits the
// same 959k primitives and the same ~14 ms of GPU work into a vsync-locked 16.7 ms by overlapping
// the two. See backport/1081-frame-pacing-plan.md.
//
// The fix is to stop waiting on the CPU at all: render into a buffer we own, mint a native fence,
// hand the fence to KMS via IN_FENCE_FD (KmsBgfxPresenter already does this) and return immediately.
// The display hardware waits for the fence; the CPU goes straight on to the next frame. That is what
// current-gl does with PlayfieldGbmRenderSlot[3], and the comment there explains the third slot:
// render(N+1) needs somewhere to draw while scanout(N) is still on screen.
//
// WHY IMPORT AND NOT EXPORT
//
// The obvious route -- render to a normal GL texture and export it as a dmabuf -- is not available:
// this driver reports mesa_dma_buf_export=0. It does report dma_buf_import=1, so the buffer has to
// originate from GBM and be imported into GL as an EGLImage. That is the same direction current-gl
// took, for the same reason.
//
// This header is the buffer pool only: allocate, import, wrap as a GL texture and as a DRM
// framebuffer, and hand out slots. Wiring BGFX to render into a slot (bgfx::overrideInternal) and
// committing the slot's fb id belong to the caller.

#include <cstdint>
#include <cstring>
#include <dlfcn.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

namespace VPX::Kms
{

// Three, for the reason current-gl documents: two means the producer must wait for the previous
// flip before it has anywhere to draw, which caps throughput as soon as frame work exceeds the
// vblank budget. A fourth would only add frame age.
inline constexpr int kScanoutSlotCount = 3;

class ScanoutSlots
{
public:
   struct Slot
   {
      struct gbm_bo* bo = nullptr;
      EGLImageKHR image = EGL_NO_IMAGE_KHR;
      GLuint texture = 0;
      GLuint fbo = 0;
      uint32_t fbId = 0;
      bool inFlight = false; // committed, not yet confirmed latched
   };

   ~ScanoutSlots() { Destroy(); }

   ScanoutSlots() = default;
   ScanoutSlots(const ScanoutSlots&) = delete;
   ScanoutSlots& operator=(const ScanoutSlots&) = delete;

   bool IsReady() const { return m_ready; }
   int Count() const { return kScanoutSlotCount; }
   const Slot& GetSlot(const int i) const { return m_slots[i]; }

   // Allocates the pool and imports each buffer into GL. Must run on the thread that owns the EGL
   // context, since it creates GL objects. Returns false with everything cleaned up on any failure:
   // a partially built pool is worse than none, because the caller would render into a slot that
   // cannot be scanned out.
   bool Init(const int drmFd, struct gbm_device* gbmDevice, EGLDisplay display,
      const uint32_t width, const uint32_t height, const uint32_t format, std::string& error)
   {
      Destroy();

      m_drmFd = drmFd;
      m_display = display;
      m_width = width;
      m_height = height;

      // VPX links BGFX, not libGLESv2 -- BGFX resolves GL itself and exports none of it, so the few
      // GL entry points needed here are resolved the same way the presenter resolves its EGL
      // extensions rather than by linking a second copy of the GL library into the process.
      if (!ResolveGl(error))
         return false;
      PLOGI << "[4kpDebug][owned_scanout] step: GL entry points resolved";

      auto createImage = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
      auto destroyImage = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
      auto imageTargetTexture = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
      if (createImage == nullptr || destroyImage == nullptr || imageTargetTexture == nullptr)
      {
         error = "EGL image entry points unavailable (need EGL_KHR_image_base + GL_OES_EGL_image)";
         return false;
      }
      m_destroyImage = destroyImage;
      PLOGI << "[4kpDebug][owned_scanout] step: EGL image entry points resolved";

      for (int i = 0; i < kScanoutSlotCount; ++i)
      {
         Slot& slot = m_slots[i];

         // SCANOUT so VOP2 can read it, RENDERING so GL can draw into it.
         PLOGI.printf("[4kpDebug][owned_scanout] step: slot %d gbm_bo_create", i);
         slot.bo = gbm_bo_create(gbmDevice, width, height, format, GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
         if (slot.bo == nullptr)
         {
            error = "gbm_bo_create failed for slot " + std::to_string(i);
            Destroy();
            return false;
         }

         const uint32_t stride = gbm_bo_get_stride(slot.bo);

         // EGL_NATIVE_PIXMAP_KHR with the gbm_bo itself, not EGL_LINUX_DMA_BUF_EXT with an fd and
         // modifier attributes. Both create an image on this driver, but the dma_buf one comes back
         // usable only as an external texture: binding it to GL_TEXTURE_2D fails with
         // GL_INVALID_OPERATION, and an external texture cannot be a render target. current-gl
         // reached the same conclusion; this is its path.
         PLOGI.printf("[4kpDebug][owned_scanout] step: slot %d eglCreateImageKHR (native pixmap, bo=%p stride=%u)", i, (void*)slot.bo, stride);
         slot.image = createImage(display, EGL_NO_CONTEXT, EGL_NATIVE_PIXMAP_KHR,
            reinterpret_cast<EGLClientBuffer>(slot.bo), nullptr);

         if (slot.image == EGL_NO_IMAGE_KHR)
         {
            error = "eglCreateImageKHR failed for slot " + std::to_string(i) + " (egl error 0x"
               + std::to_string(eglGetError()) + ')';
            Destroy();
            return false;
         }

         PLOGI.printf("[4kpDebug][owned_scanout] step: slot %d import to GL texture", i);
         s_glGenTextures(1, &slot.texture);
         s_glBindTexture(GL_TEXTURE_2D, slot.texture);
         // Set before the import: the sampler state has to be complete for the imported image, and
         // an imported EGLImage has no mip chain, so anything mip-filtered would be incomplete.
         s_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
         s_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
         s_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
         s_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
         imageTargetTexture(GL_TEXTURE_2D, (GLeglImageOES)slot.image);
         const GLenum glErr = s_glGetError();
         s_glBindTexture(GL_TEXTURE_2D, 0);
         if (glErr != GL_NO_ERROR)
         {
            error = "glEGLImageTargetTexture2DOES failed for slot " + std::to_string(i) + " (gl error 0x"
               + std::to_string(glErr) + ')';
            Destroy();
            return false;
         }

         // Importing is not enough -- the buffer has to work as a render target. A texture that
         // imports cleanly and then yields an incomplete framebuffer would fail later, at the point
         // where it is much harder to attribute.
         PLOGI.printf("[4kpDebug][owned_scanout] step: slot %d framebuffer", i);
         s_glGenFramebuffers(1, &slot.fbo);
         s_glBindFramebuffer(GL_FRAMEBUFFER, slot.fbo);
         s_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, slot.texture, 0);
         const GLenum fboStatus = s_glCheckFramebufferStatus(GL_FRAMEBUFFER);
         s_glBindFramebuffer(GL_FRAMEBUFFER, 0);
         if (fboStatus != GL_FRAMEBUFFER_COMPLETE)
         {
            error = "framebuffer incomplete for slot " + std::to_string(i) + " (status 0x"
               + std::to_string(fboStatus) + ')';
            Destroy();
            return false;
         }

         // The scanout side. Same call the presenter makes for gbm_surface buffers.
         PLOGI.printf("[4kpDebug][owned_scanout] step: slot %d drmModeAddFB2", i);
         const uint32_t handles[4] = { gbm_bo_get_handle(slot.bo).u32, 0, 0, 0 };
         const uint32_t strides[4] = { stride, 0, 0, 0 };
         const uint32_t offsets[4] = { 0, 0, 0, 0 };
         if (drmModeAddFB2(drmFd, width, height, format, handles, strides, offsets, &slot.fbId, 0) != 0)
         {
            error = "drmModeAddFB2 failed for slot " + std::to_string(i);
            Destroy();
            return false;
         }
      }

      m_ready = true;
      return true;
   }

   // Next slot not currently being scanned out or awaiting latch. Returns -1 when every slot is
   // busy, which the caller should treat as "skip this frame" rather than stalling -- stalling is
   // the behaviour this whole change exists to remove.
   int AcquireFree()
   {
      for (int n = 0; n < kScanoutSlotCount; ++n)
      {
         const int i = (m_cursor + n) % kScanoutSlotCount;
         if (!m_slots[i].inFlight)
         {
            m_cursor = (i + 1) % kScanoutSlotCount;
            return i;
         }
      }
      return -1;
   }

   void MarkInFlight(const int i) { m_slots[i].inFlight = true; }
   void MarkFree(const int i) { m_slots[i].inFlight = false; }

   void Destroy()
   {
      for (Slot& slot : m_slots)
      {
         if (slot.fbId != 0 && m_drmFd >= 0)
            drmModeRmFB(m_drmFd, slot.fbId);
         if (slot.fbo != 0 && s_glDeleteFramebuffers != nullptr)
            s_glDeleteFramebuffers(1, &slot.fbo);
         if (slot.texture != 0 && s_glDeleteTextures != nullptr)
            s_glDeleteTextures(1, &slot.texture);
         if (slot.image != EGL_NO_IMAGE_KHR && m_destroyImage != nullptr)
            m_destroyImage(m_display, slot.image);
         if (slot.bo != nullptr)
            gbm_bo_destroy(slot.bo);
         slot = Slot {};
      }
      m_ready = false;
   }

private:
   typedef void   (GL_APIENTRYP GenTexturesFn)(GLsizei, GLuint*);
   typedef void   (GL_APIENTRYP BindTextureFn)(GLenum, GLuint);
   typedef void   (GL_APIENTRYP DeleteTexturesFn)(GLsizei, const GLuint*);
   typedef GLenum (GL_APIENTRYP GetErrorFn)(void);
   typedef void   (GL_APIENTRYP TexParameteriFn)(GLenum, GLenum, GLint);
   typedef void   (GL_APIENTRYP GenFramebuffersFn)(GLsizei, GLuint*);
   typedef void   (GL_APIENTRYP BindFramebufferFn)(GLenum, GLuint);
   typedef void   (GL_APIENTRYP FramebufferTexture2DFn)(GLenum, GLenum, GLenum, GLuint, GLint);
   typedef GLenum (GL_APIENTRYP CheckFramebufferStatusFn)(GLenum);
   typedef void   (GL_APIENTRYP DeleteFramebuffersFn)(GLsizei, const GLuint*);

   static inline GenTexturesFn s_glGenTextures = nullptr;
   static inline BindTextureFn s_glBindTexture = nullptr;
   static inline DeleteTexturesFn s_glDeleteTextures = nullptr;
   static inline GetErrorFn s_glGetError = nullptr;
   static inline TexParameteriFn s_glTexParameteri = nullptr;
   static inline GenFramebuffersFn s_glGenFramebuffers = nullptr;
   static inline BindFramebufferFn s_glBindFramebuffer = nullptr;
   static inline FramebufferTexture2DFn s_glFramebufferTexture2D = nullptr;
   static inline CheckFramebufferStatusFn s_glCheckFramebufferStatus = nullptr;
   static inline DeleteFramebuffersFn s_glDeleteFramebuffers = nullptr;

   // eglGetProcAddress, not dlsym. libGLESv2.so.2 on this device is a ~5 KB stub that only pulls in
   // libmali.so.1 and exports none of GL itself; dlsym on its handle still resolves through the
   // dependency and returns a non-null pointer that is not safely callable here -- which is how the
   // first attempt passed its null checks and then segfaulted on the first call.
   // eglGetProcAddress returns the entry point belonging to the current context's implementation,
   // which is the one BGFX is already driving.
   static bool ResolveGl(std::string& error)
   {
      if (s_glGenTextures != nullptr)
         return true;

      s_glGenTextures = (GenTexturesFn)eglGetProcAddress("glGenTextures");
      s_glBindTexture = (BindTextureFn)eglGetProcAddress("glBindTexture");
      s_glDeleteTextures = (DeleteTexturesFn)eglGetProcAddress("glDeleteTextures");
      s_glGetError = (GetErrorFn)eglGetProcAddress("glGetError");
      s_glTexParameteri = (TexParameteriFn)eglGetProcAddress("glTexParameteri");
      s_glGenFramebuffers = (GenFramebuffersFn)eglGetProcAddress("glGenFramebuffers");
      s_glBindFramebuffer = (BindFramebufferFn)eglGetProcAddress("glBindFramebuffer");
      s_glFramebufferTexture2D = (FramebufferTexture2DFn)eglGetProcAddress("glFramebufferTexture2D");
      s_glCheckFramebufferStatus = (CheckFramebufferStatusFn)eglGetProcAddress("glCheckFramebufferStatus");
      s_glDeleteFramebuffers = (DeleteFramebuffersFn)eglGetProcAddress("glDeleteFramebuffers");

      if (s_glGenTextures == nullptr || s_glBindTexture == nullptr
       || s_glDeleteTextures == nullptr || s_glGetError == nullptr
       || s_glTexParameteri == nullptr || s_glGenFramebuffers == nullptr
       || s_glBindFramebuffer == nullptr || s_glFramebufferTexture2D == nullptr
       || s_glCheckFramebufferStatus == nullptr || s_glDeleteFramebuffers == nullptr)
      {
         error = "eglGetProcAddress could not resolve the core GL entry points (needs "
                 "EGL_KHR_get_all_proc_addresses for non-extension functions)";
         s_glGenTextures = nullptr;
         return false;
      }

      // Cheap liveness check before anything relies on these. glGetError on a current context is
      // harmless and touches the same dispatch path the rest will, so a bad resolve shows up here
      // rather than as a fault three calls later.
      s_glGetError();
      return true;
   }

   Slot m_slots[kScanoutSlotCount];
   int m_drmFd = -1;
   EGLDisplay m_display = EGL_NO_DISPLAY;
   PFNEGLDESTROYIMAGEKHRPROC m_destroyImage = nullptr;
   uint32_t m_width = 0, m_height = 0;
   int m_cursor = 0;
   bool m_ready = false;
};

} // namespace VPX::Kms
