// license:GPLv3+

#include "core/stdafx.h"
#include "renderer/Renderer.h"

#include "parts/Collection.h"

#ifdef _MSC_VER
#include "dwmapi.h"
#pragma comment(lib, "Dwmapi.lib")
#endif

#include <thread>

#ifdef __LIBVPINBALL__
#ifdef __APPLE__
#include <pthread.h>
#include <sys/qos.h>
#endif
#endif

#if !defined(DISABLE_FORCE_NVIDIA_OPTIMUS) && defined(ENABLE_DX9)
#include "nvapi/nvapi.h"
#endif

#include "RenderDevice.h"
#include "RenderCommand.h"
#include "Shader.h"
#include "VRDevice.h"
#include "standalone/vpx_ready_signal.h"
#ifdef __RK3588__
#include "standalone/KmsBgfxPresenter.h"
#include <unordered_map>
#endif
#include "renderer/AreaTex.h"
#include "renderer/SearchTex.h"

#if defined(ENABLE_BGFX)
#ifdef __STANDALONE__
#pragma push_macro("_WIN64")
#undef _WIN64
#endif
#include <bx/platform.h>
#include <bx/string.h>
#include <bgfx/bgfx.h>
#include <bimg/bimg.h>
#ifdef __STANDALONE__
#pragma pop_macro("_WIN64")
#endif

#elif defined(ENABLE_OPENGL)
#include "typedefs3D.h"
#include "TextureManager.h"

#elif defined(ENABLE_DX9)
#include "parts/Material.h"
#endif

#ifdef __LIBVPINBALL__
#include "lib/src/VPinballLib.h"
#endif

// MSVC Concurrency Viewer support (requires to add the MSVC Concurrency SDK to the project)
//#define MSVC_CONCURRENCY_VIEWER
#ifdef MSVC_CONCURRENCY_VIEWER
#include <cvmarkersobj.h>
using namespace Concurrency::diagnostic;
marker_series series;
#define BEGIN_SPAN(name, label) span* name = new span(series, 1, _T(label));
#define END_SPAN(name) delete name;
#else
#define BEGIN_SPAN(name, label)
#define END_SPAN(name)
#endif

#if BX_PLATFORM_WINDOWS
#include "PresentMon/PresentMonProvider.h"
#include "PresentMon/PresentMonProvider.cpp"
#endif

// Define to 1 to get full BGFX log in debug build
#define LOG_BGFX 0



////////////////////////////////////////////////////////////////////

#if defined(ENABLE_BGFX)
void RenderDevice::tBGFXCallback::fatal(const char* _filePath, uint16_t _line, bgfx::Fatal::Enum _code, const char* _str)
{
   PLOGE << _filePath << ':' << _line << "BGFX FATAL " << _code << ": " << _str;
   if (bgfx::Fatal::DebugCheck == _code)
      bx::debugBreak();
   else
      abort();
}

void RenderDevice::tBGFXCallback::traceVargs(const char* _filePath, uint16_t _line, const char* _format, va_list _argList)
{
#if LOG_BGFX
   char temp[2048];
   char* out = temp;
   va_list argListCopy;
   va_copy(argListCopy, _argList);
   int32_t len = bx::snprintf(out, std::size(temp), "%s (%d): ", _filePath, _line);
   int32_t total = len + bx::vsnprintf(out + len, std::size(temp) - len, _format, argListCopy);
   va_end(argListCopy);
   if ((int32_t)std::size(temp) < total)
   {
      out = (char*)alloca(total + 1);
      bx::memCopy(out, temp, len);
      bx::vsnprintf(out + len, total - len, _format, _argList);
   }
   out[total] = '\0';
   bx::debugOutput(out);
   if (total > 0 && out[total - 1] == '\n')
      out[total - 1] = '\0';
   PLOGI << out;
#endif
}

void RenderDevice::tBGFXCallback::screenShot(
   const char* _filePath, uint32_t _width, uint32_t _height, uint32_t _pitch, bgfx::TextureFormat::Enum _format, const void* _data, uint32_t _size, bool _yflip)
{
   // Note that BGFX has a few bugs regarding screenshots:
   // - DX11 applies an image swizzle to BGRA (like the doc state) but not accounting for the real backbuffer format, hence failing on anything but a RGBA backbuffer (for example HDR)
   // - DX12 does not implement the framebuffer selection and always captures from the base swapchain and returns data on the swapchain format
   // - Metal implements per-window framebuffer selection and returns data on the (per-window) swapchain format
   // - OpenGL & Vulkan seems to be ok (always returning 4 byte BGRA, eventually after conversion if backbuffer format is not BGRA)
   std::function<void(bool)> callback;
   bool fireCallback = false;
   bool callbackSuccess = false;
   {
      // The screenshot state is concurrently written by the logic thread in CaptureScreenshot
      std::lock_guard lock(m_rd.m_screenshotMutex);

      const std::filesystem::path path(_filePath);
      int index = -1;
      for (int i = 0; i < (int)m_rd.m_screenshotFilename.size(); i++)
         if (m_rd.m_screenshotFilename[i] == path)
         {
            index = i;
            break;
         }
      // Drop stale/duplicate captures that are no longer pending (e.g. a late delivery of a request
      // that was already re-issued by the timeout path), instead of saving them again or double-firing.
      if (index < 0)
         return;
      m_rd.m_screenshotFilename.erase(m_rd.m_screenshotFilename.begin() + index);

      bool success = false;
      if (auto tex = BaseTexture::Create(_width, _height, BaseTexture::SRGBA); tex)
      {
         switch (_format)
         {
         case bgfx::TextureFormat::RGBA8:
            if (_pitch == _width * 4)
               memcpy(tex->data(), _data, _size);
            else
            {
               for (unsigned int i = 0; i < _height; i++)
                  bx::memCopy(static_cast<uint8_t*>(tex->data()) + i * (_width * 4), static_cast<const uint8_t*>(_data) + i * _pitch, _width * 4);
            }
            success = true;
            break;

         case bgfx::TextureFormat::BGRA8:
            if (_pitch == _width * 4)
               copy_bgra_rgba<false>(static_cast<uint32_t*>(tex->data()), static_cast<const uint32_t*>(_data), (size_t)_width * _height);
            else
            {
               for (unsigned int i = 0; i < _height; i++)
               {
                  const uint32_t* src = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(_data) + i * _pitch);
                  uint32_t* dst = static_cast<uint32_t*>(tex->data()) + i * _width;
                  copy_bgra_rgba<false>(dst, src, _width);
               }
            }
            success = true;
            break;

         case bgfx::TextureFormat::RGB8: // Unsupported yet
         default: // HDR, ... are not supported either
            break;
         }

         if (success)
         {
            if (_yflip)
               tex->FlipY();
            success = tex->Save(_filePath);
         }
      }
      m_rd.m_screenshotSuccess &= success;
      if (m_rd.m_screenshotFilename.empty())
      {
         fireCallback = true;
         callbackSuccess = m_rd.m_screenshotSuccess;
         callback = m_rd.m_screenshotCallback;
      }
   }
   // Fire outside the lock: the callback may take other locks (e.g. the capture mutex) or re-enter CaptureScreenshot
   if (fireCallback)
      callback(callbackSuccess);
}

bgfx::TextureFormat::Enum RenderDevice::SelectBackBufferFormat(const VPX::Window* wnd, bgfx::TextureFormat::Enum defaultFormat, bool allowHDR10) const
{
   // If we already have a backbuffer on this display, use the same format (it seems to cause issues on Linux otherwise, and the selection process should lead to the same result anyway)
   SDL_DisplayID displayId = SDL_GetDisplayForWindow(wnd->GetCore());
   for (const VPX::Window* existingWnd : m_outputWnd)
   {
      if (existingWnd == nullptr || existingWnd == wnd || existingWnd->GetBackBuffer() == nullptr)
         continue;
      if (SDL_DisplayID existingDisplayId = SDL_GetDisplayForWindow(existingWnd->GetCore()); existingDisplayId == displayId)
      {
         return existingWnd->GetBackBuffer()->GetCoreColorFormat();
      }
   }

   // Use the display format as a default if no default is provided
   if (defaultFormat == bgfx::TextureFormat::Count)
   {
      const SDL_DisplayMode* displayMode = displayId == 0 ? nullptr : SDL_GetDesktopDisplayMode(displayId);
      if (displayMode)
      {
         switch (displayMode->format)
         {
         case SDL_PIXELFORMAT_RGB24: defaultFormat = bgfx::TextureFormat::RGB8; break;
         case SDL_PIXELFORMAT_BGR24: defaultFormat = bgfx::TextureFormat::RGB8; break;
         case SDL_PIXELFORMAT_XRGB8888: defaultFormat = bgfx::TextureFormat::RGBA8; break;
         case SDL_PIXELFORMAT_RGBX8888: defaultFormat = bgfx::TextureFormat::RGBA8; break;
         case SDL_PIXELFORMAT_XBGR8888: defaultFormat = bgfx::TextureFormat::BGRA8; break;
         case SDL_PIXELFORMAT_BGRX8888: defaultFormat = bgfx::TextureFormat::BGRA8; break;
         case SDL_PIXELFORMAT_ARGB8888: defaultFormat = bgfx::TextureFormat::RGBA8; break;
         case SDL_PIXELFORMAT_RGBA8888: defaultFormat = bgfx::TextureFormat::RGBA8; break;
         case SDL_PIXELFORMAT_ABGR8888: defaultFormat = bgfx::TextureFormat::BGRA8; break;
         case SDL_PIXELFORMAT_BGRA8888: defaultFormat = bgfx::TextureFormat::BGRA8; break;
         case SDL_PIXELFORMAT_RGB565: defaultFormat = bgfx::TextureFormat::R5G6B5; break;
         case SDL_PIXELFORMAT_BGR565: defaultFormat = bgfx::TextureFormat::B5G6R5; break;
         case SDL_PIXELFORMAT_ABGR1555: defaultFormat = bgfx::TextureFormat::BGR5A1; break;
         case SDL_PIXELFORMAT_BGRA5551: defaultFormat = bgfx::TextureFormat::BGR5A1; break;
         case SDL_PIXELFORMAT_ARGB1555: defaultFormat = bgfx::TextureFormat::RGB5A1; break;
         case SDL_PIXELFORMAT_RGBA5551: defaultFormat = bgfx::TextureFormat::RGB5A1; break;
         case SDL_PIXELFORMAT_XRGB2101010: defaultFormat = allowHDR10 ? bgfx::TextureFormat::RGB10A2 : bgfx::TextureFormat::RGBA8; break;
         case SDL_PIXELFORMAT_ARGB2101010: defaultFormat = allowHDR10 ? bgfx::TextureFormat::RGB10A2 : bgfx::TextureFormat::RGBA8; break;
         case SDL_PIXELFORMAT_XBGR2101010: defaultFormat = allowHDR10 ? bgfx::TextureFormat::RGB10A2 : bgfx::TextureFormat::BGRA8; break;
         case SDL_PIXELFORMAT_ABGR2101010: defaultFormat = allowHDR10 ? bgfx::TextureFormat::RGB10A2 : bgfx::TextureFormat::BGRA8; break;
         default:
            PLOGE << "Unsupported SDL pixel format encountered: " << SDL_GetPixelFormatName(displayMode->format);
            defaultFormat = bgfx::TextureFormat::RGBA8;
            break;
         }
      }
      else
      {
         PLOGE << "SDL failed to gather the screen back buffer format, defaulting to RGBA8 for " << SDL_GetWindowTitle(wnd->GetCore());
         defaultFormat = bgfx::TextureFormat::RGBA8;
      }
   }

   // Search and select in the list of texture format that can be used as a backbuffer target
   bgfx::TextureFormat::Enum selectedFormat = bgfx::TextureFormat::RGBA8;
   int colorSelect = INT_MIN;
   for (int i = 0; i < bgfx::TextureFormat::Count; i++)
   {
      if ((bgfx::getCaps()->formats[i] & BGFX_CAPS_FORMAT_TEXTURE_BACKBUFFER) != 0)
      {
         auto fmt = bimg::TextureFormat::Enum(i);
         if (bimg::isColor(fmt))
         {
            int heuristic = 0;
            // Search for a standard default 24 or 32 bit format (BGRA8 / RGBA8)
            heuristic += bimg::getBitsPerPixel(fmt) == 24 ? 10 : 0;
            heuristic += bimg::getBitsPerPixel(fmt) == 32 ? 100 : 0;
            heuristic += bgfx::TextureFormat::Enum(fmt) == defaultFormat ? 200: 0; // To avoid switching uselessly, and to favor display format
            heuristic += bimg::isCompressed(fmt) ? -1000 : 0;
            heuristic += bimg::isFloat(fmt) ? -1000 : 0;
#if defined(__ANDROID__)
            // Temporary: prefer RGBA8 over BGRA8 as some Android drivers reject BGRA8 Vulkan swapchains,
            // until the swapchain format is negotiated against vkGetPhysicalDeviceSurfaceFormatsKHR in bgfx
            heuristic += fmt == bimg::TextureFormat::RGBA8 ? 1 : 0;
#endif
            if (allowHDR10) // This needs a display that support RGB10A2 backbuffer and the HDR10 colorspace (see DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020)
               heuristic += fmt == bimg::TextureFormat::RGB10A2 ? 50000 : 0;
            // Note that RGB16F is not supported as BGFX does not report the swapchain capability (see DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709) and we don't have a tonemapper for this colorspace
            //if (allowHDR16F) heuristic += fmt == bimg::TextureFormat::RGBA16F ? 50000 : 0; 
            if (heuristic > colorSelect)
            {
               colorSelect = heuristic;
               selectedFormat = bgfx::TextureFormat::Enum(fmt);
            }
         }
      }
   }

   if (colorSelect == INT_MIN)
   {
      // Linux/Vulkan does not report backbuffer caps in headless mode, Still BGRA8 seems to be supported everywhere, so this is not fully clean but ok
      PLOGE << "Driver did not report any supported backbuffer format for " << SDL_GetWindowTitle(wnd->GetCore()) << ". Defaulting to BGRA8";
      selectedFormat = bgfx::TextureFormat::BGRA8;
   }

   return selectedFormat;
}

colorFormat RenderDevice::BGFXtoVPXTextureFormat(bgfx::TextureFormat::Enum format)
{
   colorFormat vpxFormat;
   switch (format)
   {
   case bgfx::TextureFormat::R16F: vpxFormat = colorFormat::RED16F; break;
   case bgfx::TextureFormat::RG16F: vpxFormat = colorFormat::RG16F; break;
   case bgfx::TextureFormat::RGBA16F: vpxFormat = colorFormat::RGB16F; break;
   case bgfx::TextureFormat::RGBA32F: vpxFormat = colorFormat::RGB32F; break;
   case bgfx::TextureFormat::RGB5A1: vpxFormat = colorFormat::RGB5; break;
   case bgfx::TextureFormat::RGB8: vpxFormat = colorFormat::RGB8; break;
   case bgfx::TextureFormat::RGBA8: vpxFormat = colorFormat::RGBA8; break;
   case bgfx::TextureFormat::BGRA8: vpxFormat = colorFormat::RGBA8; break; // FIXME incorrect format in VPX (should not have any effect but still...)
   case bgfx::TextureFormat::RGB10A2: vpxFormat = colorFormat::RGB10; break;
   case bgfx::TextureFormat::R8: vpxFormat = colorFormat::GREY8; break;
   default:
      PLOGE << "Unsupported format requested: " << bimg::getName(bimg::TextureFormat::Enum(format)) << " replacing by RGBA8";
      vpxFormat = colorFormat::RGBA8;
      break;
   }
   return vpxFormat;
}


static const string& bgfxRendererName(const bgfx::RendererType::Enum type);

// bgfx's render thread. From bgfx.h: calling renderFrame() before init stops bgfx creating its own,
// and "if both bgfx::renderFrame and bgfx::init are called from the same thread, bgfx operates in
// single-threaded mode" -- which is what VPX has done until now, and why a blocking eglSwapBuffers
// stalls the whole frame: there is no other thread to build the next one meanwhile.
//
// Splitting them gives bgfx's documented behaviour, where "the API thread and render thread run in
// parallel, overlapping CPU frame building with GPU rendering", without patching bgfx.
//
// This thread owns the GL context, so KMS presentation belongs here: renderFrame() returns once the
// frame has been rendered and swapped, which is exactly when the front buffer exists.
void RenderDevice::BGFXRenderThread(RenderDevice* rd)
{
   SetThreadName("BGFXRender"s);

   bgfx::renderFrame(); // before init, so bgfx does not spawn a render thread of its own
   rd->m_bgfxPreInitDone.release();

   // Loop until bgfx says Exiting, NOT until m_renderDeviceAlive goes false. bgfx::shutdown() runs
   // on the API thread, posts an exit, and then waits for this thread to consume it through
   // renderFrame(). Leaving early on our own flag means that exit is never consumed and shutdown
   // blocks forever -- which is exactly what hung on close.
   for (;;)
   {
      #ifdef __RK3588__
      const uint64_t t0 = usec();
      #endif
      const bgfx::RenderFrame::Enum r = bgfx::renderFrame(16);
      if (r == bgfx::RenderFrame::Exiting)
         break;
      #ifdef __RK3588__
      // Not during teardown: the presenters and windows are being freed on the other thread.
      if (r == bgfx::RenderFrame::Render && rd->m_renderDeviceAlive)
      {
         const uint64_t t1 = usec();
         rd->PresentKmsWindows();
         const uint64_t t2 = usec();
         s_rtRenderUs += t1 - t0;
         s_rtPresentUs += t2 - t1;
         s_rtFrames++;
      }
      #endif
   }
}

void RenderDevice::RenderThread(RenderDevice* rd, bgfx::Init init)
{
   SetThreadName("RenderThread"s);
   g_pplayer->m_renderProfiler->SetThreadLock();
#ifdef __LIBVPINBALL__
#ifdef __APPLE__
   // Set render thread to User-interactive QoS to match main thread and prevent priority inversion
   pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
#endif

   // Workflow and latency considerations:
   // - Visual latency is finger to photon latency, defined by the following sequence:
   //    >>> finger to game state, game state to GPU submit, GPU submit to rendered, rendered to displayed <<<
   //   . finger to game state is continuously done on the main thread at a sub millisecond pace
   //   . game state snapshot by the main thread is prepared when the render thread ask for it. It should be done as
   //     late as possible but to limit stutter, we perform it while the previous frame is submitted to the GPU which
   //     guarantee optimal parallelism between the CPU and GPU.
   //   . submit to GPU (via BGFX) is performed as late as possible after we have a free swapchain slot (to avoid 
   //     increasing latency, see below), and at display pace using sleeping either against VBlank or based on previous
   //     frame timings (with a small margin). We update ball position based on latest game state and expected time of 
   //     display as this is the most latency sensitive part of the frame.
   //   . rendered to displayed mostly depends on the operating system. On Windows, it mostly depends on the compositor
   //     behavior. If the compositor is in the way, the rendered frame goes through the compositor queue for composition,
   //     adding 1 frame of latency (PresentMon will report 'Composed Flip').
   // - The overall aim is to prepare the frame as late as possible, just before it is presented to the player, taking
   //   in account the latest game state. To reach this aim, we should never have multiple frames enqueued either 
   //   waiting for rendering (GPU render queue defined by BGFX's maxLatency) or waiting for presenting. This requires 
   //   us to know when the swapchain has an empty slot. We modified BGFX to add support for managing swapchain latency:
   //   . For DirectX, we use the 'waitable' swapchain offered by DXGI, that is to say that DXGI allows us to wait for
   //     the swapchain queue to have at least one empty slot. This needs the swapchain queue to be limited to 1 frame
   //     (maxFrameLatency = 1) to avoid having more than 1 frame enqueued (beside the displayed frame) for lowest latency.
   //     When running in fullscreen exclusive mode, there is no waitable support but the compostior is disabled so we
   //     can simply run at the display refresh rate (eventually aligning to the display with a VSYNC to limit tearing).
   //   . For Vulkan, we use the vkWaitForPresentKHR extension which allows to wait for a specific presented frame to be 
   //     displayed. We wait for the last presented frame to be displayed before submitting the next one (in turn 
   //     enforcing a maxFrameLatency of 1).
   //   . Metal & OpenGL do not have support for swapchain latency management yet
   // - OpenXR offers its own frame display time prediction that we use when in VR mode.

   init.resolution.numBackBuffers = 2; // Simple flip model with 2 buffers: one locked for the GPU (rendering), one locked for the swapchain (displayed or queued)
   init.resolution.maxFrameLatency = clamp(g_pplayer->m_ptable->m_settings.GetPlayer_MaxPrerenderedFrames(), 1, 3); // Default to 1 (User should set swapchain queue to 1 or 2 to limit latency)
   init.resolution.reset = 0; 
   init.resolution.reset |= BGFX_RESET_MAXANISOTROPY;
   //init.resolution.reset |= BGFX_RESET_FLUSH_AFTER_RENDER; // Not really needed as we are doing a present after submit which in turn triger sending the commands to the GPU
   init.resolution.reset |= BGFX_RESET_FLIP_AFTER_RENDER;
   // BGFX despite proposing a reset flag (BGFX_RESET_FULLSCREEN) does not implement exclusive fullscreen, so we do not support it on this backend (exclusive fullscreen is
   // somewhat deprecated anyway as some OS do not offer it at all, and others implement it through GPU multiplane overlay to actually achieve zero-overhead backbuffer flips)
   assert(rd->m_outputWnd[0]->GetWindowMode() != VPX::Window::WindowMode::ExclusiveFullscreen);

   const bool allowHDR10ColorSpace = true //
      && g_pplayer->m_playMode != Player::PlayMode::CaptureAttract // Disable WCG colorspace as it causes issues with video recording for the time being
      && !rd->m_isAnaglyph // Anaglyph stereo requires an sRGB colorspace
      && !g_pplayer->IsVR(); // Not yet supported (not sure if there exists HDR headset)

   // If using OpenXR, we need to create a graphics layer adapted to OpenXR requirements
   if (g_pplayer->IsVR())
   {
#ifdef ENABLE_XR
      assert((init.resolution.reset & BGFX_RESET_VSYNC) == 0); // Display VSync must be disabled as we are synced by OpenXR on the headset display
      init.type = g_pplayer->m_vrDevice->GetGraphicContextType();
      init.platformData.context = g_pplayer->m_vrDevice->GetGraphicContext();
      assert(init.platformData.context != nullptr);
      // For the time being, we do not support having a desktop swapchain along the headset swapchain under Vulkan, so we run BGFX in headless mode
      // Note that this is needed for native VR (running directly on the headset)
      if (init.type == bgfx::RendererType::Vulkan)
         init.platformData.nwh = nullptr;
#endif
   }

   // BGFX default behavior is to set its 'API' thread (the one where bgfx API calls are allowed)
   // as the one from which init is called, and spawn a BGFX render thread in charge of submitting
   // render queue from the CPU to the GPU.
   // Since VPX already splits the logic/prepare frame thread (CPU only) from the submit/flip (CPU-GPU)
   // we do not really need BGFX to create its additional thread. Calling bgfx::renderFrame allows
   // to do so, ending up with this thread being the only BGFX thread. It needs to be called before each bgfx::init
   // This is also required for OpenXR which needs all the GPU submission calls to be performed after WaitFrame (sync) and between Begin/EndFrame

   // We first run in headless mode to initialize the underlying backend and try to gather information to select a supported backbuffer format
   // This is needed to select a safe backbuffer format but will fail under OpenGL or Linux. For these, we start using BGRA8 which seems to be supported everywhere and adjust afterward
   init.resolution.formatColor = bgfx::TextureFormat::BGRA8;
   if (init.platformData.nwh && init.type != bgfx::RendererType::OpenGL && init.type != bgfx::RendererType::OpenGLES && init.type != bgfx::RendererType::Direct3D12)
   {
      const uint32_t width = init.resolution.width;
      const uint32_t height = init.resolution.height;
      void* nativeWindow = init.platformData.nwh;
      void* nativeDisplayType = init.platformData.ndt;
      void* context = init.platformData.context;
      init.resolution.width = 0;
      init.resolution.height = 0;
      init.resolution.reset &= ~BGFX_RESET_HDR10;
      init.platformData.nwh = nullptr;
      init.platformData.ndt = nullptr;
      init.platformData.context = nullptr;
      bgfx::renderFrame();
      if (bgfx::init(init))
      {
         // Select the backbuffer color format, after initializing in headless mode to have access to the list of supported backbuffer format
         // This may fail on some backends that need a surface to report its capabilities (for example Linux/Vulkan)
         init.resolution.formatColor = rd->SelectBackBufferFormat(rd->m_outputWnd[0], bgfx::TextureFormat::Count, allowHDR10ColorSpace && (bgfx::getCaps()->supported & BGFX_CAPS_HDR10));
         bgfx::shutdown();
      }
      else
      {
         PLOGE << "Failed to initialize BGFX for backbuffer format selection, defaulting to BGRA8";
      }
      init.resolution.width = width;
      init.resolution.height = height;
      init.platformData.nwh = nativeWindow;
      init.platformData.ndt = nativeDisplayType;
      init.platformData.context = context;
   }

   init.resolution.reset &= ~BGFX_RESET_HDR10; // Handle HDR10 color space (actually BGFX select colorspace based on the backbuffer format and discard this flag)
   init.resolution.reset |= init.resolution.formatColor == bgfx::TextureFormat::RGB10A2 ? BGFX_RESET_HDR10 : 0;
   // OpenXR is excluded deliberately: it needs all GPU submission on one thread between WaitFrame
   // and EndFrame. This hardware has no VR, but the path still exists.
   rd->m_bgfxMultithreaded = !g_pplayer->IsVR() && g_pplayer->m_ptable
      && g_pplayer->m_ptable->m_settings.GetStandalone_4kpBgfxMultithreaded();

   if (rd->m_bgfxMultithreaded)
   {
      rd->m_bgfxRenderThread = std::thread(&RenderDevice::BGFXRenderThread, rd);
      rd->m_bgfxPreInitDone.acquire(); // renderFrame() must land before init, or bgfx makes its own
      PLOGI << "BGFX running multithreaded: this thread is the API thread, presentation is on the render thread";
   }
   else
   {
      bgfx::renderFrame();
   }

   if (!bgfx::init(init))
   {
      PLOGE << "BGFX initialization failed";
      exit(-1);
   }

   #ifdef __RK3588__
   // Per-view GPU timing is bgfx's own Profiler (src/renderer.h), which only collects when this
   // flag is set. Our patch makes it round-robin one view per frame and fixes the GLES timer
   // imports; without the flag it costs nothing and reports nothing. Gated separately from the
   // frame-level stats: under owned scanout the profiler costs ~15 ms of CPU per frame on this
   // driver, so 4kpGpuTimers alone must stay cheap enough to leave on during play.
   if (AreFrameStatsEnabled() && g_pplayer->m_ptable->m_settings.GetStandalone_4kpGpuTimersPerPass())
      bgfx::setDebug(BGFX_DEBUG_PROFILER);
   #endif

   // A specific backend was requested but BGFX created a different one (init.fallback let it fall back to
   // the next best because the requested backend failed to initialize), so make that explicit.
   if (init.type != bgfx::RendererType::Count && bgfx::getRendererType() != init.type)
   {
      PLOGW << "Requested graphics backend " << bgfxRendererName(init.type) << " is unavailable; BGFX fell back to "
            << bgfx::getRendererName(bgfx::getRendererType());
   }

   if (init.platformData.nwh)
   {
      // Validate the backbuffer format now that we have a swapchain (handles buggy platforms like Linux/Vulkan where capabilities of the swapchain is only reported after creation of the swapchain...)
      const bgfx::TextureFormat::Enum initFormatColor = init.resolution.formatColor;
      init.resolution.formatColor = rd->SelectBackBufferFormat(rd->m_outputWnd[0], initFormatColor, allowHDR10ColorSpace && (bgfx::getCaps()->supported & BGFX_CAPS_HDR10));
      if (initFormatColor != init.resolution.formatColor)
      {
         init.resolution.reset &= ~BGFX_RESET_HDR10;
         init.resolution.reset |= init.resolution.formatColor == bgfx::TextureFormat::RGB10A2 ? BGFX_RESET_HDR10 : 0;
         bgfx::reset(init.resolution.width, init.resolution.height, init.resolution.reset, init.resolution.formatColor);
      }
   }

   PLOGI << "BGFX initialized using " << bgfx::getRendererName(bgfx::getRendererType()) << " backend (" << init.resolution.width << 'x' << init.resolution.height << " "
         << bimg::getName(bimg::TextureFormat::Enum(init.resolution.formatColor)) << ')';

   const uint16_t vendorId = bgfx::getCaps()->vendorId;
   string vendorString;
   switch (vendorId)
   {
      case BGFX_PCI_ID_SOFTWARE_RASTERIZER: vendorString = "Software Raster"s; break;
      case BGFX_PCI_ID_NVIDIA: vendorString = "NVIDIA"s; break;
      case BGFX_PCI_ID_AMD:
      case 0x1022: vendorString = "AMD"s; break;
      case BGFX_PCI_ID_INTEL: vendorString = "Intel"s; break;
      case BGFX_PCI_ID_ARM: vendorString = "arm"s; break;
      case 0x5143: vendorString = "Qualcomm"s; break;
      case 0x1010: vendorString = "ImgTec (PowerVR)"s; break;
      case BGFX_PCI_ID_APPLE: vendorString = "Apple"s; break;
      case BGFX_PCI_ID_MICROSOFT: vendorString = "Microsoft"s; break;
      default: vendorString = "Unknown"s; break;
   }
   rd->m_GPU_name = vendorString + '/' + std::to_string(bgfx::getCaps()->deviceId);
   rd->m_driver_name = bgfx::getRendererName(bgfx::getRendererType()) + " backend"s;

   if (g_pplayer->IsVR())
   {
#ifdef ENABLE_XR
      g_pplayer->m_vrDevice->CreateSession();
      rd->m_framePending = true; // Delay first frame preparation
#endif
   }
   else
   {
      RenderTarget* backbuffer = new RenderTarget(rd, SurfaceType::RT_DEFAULT, BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE, init.resolution.formatColor, BGFX_INVALID_HANDLE,
         init.resolution.formatDepthStencil, "BackBuffer", init.resolution.width, init.resolution.height, BGFXtoVPXTextureFormat(init.resolution.formatColor));
      rd->m_outputWnd[0]->SetBackBuffer(backbuffer, (init.resolution.reset & BGFX_RESET_HDR10) != 0);
      rd->m_framePending = false; // Request first frame to be prepared as soon as possible
   }

   // Unlock requesting thread and start render loop
   rd->m_rendererInitialized.release();

#ifdef __STANDALONE__
   std::this_thread::sleep_for(std::chrono::milliseconds(500));
#endif

#ifdef ENABLE_XR
   if (g_pplayer->m_vrDevice)
      rd->BGFXOpenXRRenderLoop(init);
   else
#endif
      rd->BGFXDesktopRenderLoop(init);

   // Signal that the render loop has fully exited (no more rendering) so the destructor can free render
   // resources without racing an in-flight frame still using them
   rd->m_renderThreadStopped.release();

   // Wait until main thread has released all native resources
   rd->m_rendererInitialized.acquire();
   bgfx::shutdown();
   rd->m_renderDeviceAlive = true;
}

#ifdef ENABLE_XR
void RenderDevice::BGFXOpenXRRenderLoop(const bgfx::Init& init)
{
   // OpenXR renderloop, synchronized on headset (using xrWaitFrame), with game logic preparing frames when headset request them
   m_frameIndex = 0;
   while (m_renderDeviceAlive)
   {
      // Process OpenXR events (headset status, ...)
      g_pplayer->m_vrDevice->PollEvents();

      // Let OpenXR throttle rendering, preparing frame on demand when view positions are acquired and predicted display time is defined
      g_pplayer->m_vrDevice->RenderFrame(this,
         [this](RenderTarget* vrRenderTarget)
         {
            // FIXME No VR target, we should still render to the preview window
            if (vrRenderTarget == nullptr)
               return;

            // Set acquired swapchain images as render target, request a new renderframe from GameLogic thread, and wait for it
            BEGIN_SPAN(tagSpanFF, "vpxWaitFrame")
            g_pplayer->m_renderProfiler->EnterProfileSection(FrameProfiler::PROFILE_RENDER_WAIT);
            m_outputWnd[0]->SetBackBuffer(vrRenderTarget, false);
            m_framePending = false;
            m_frameReadySem.acquire();
            m_outputWnd[0]->SetBackBuffer(nullptr, false); // as the vrRenderTarget is not valid outside of this scope
            g_pplayer->m_renderProfiler->ExitProfileSection();
            END_SPAN(tagSpanFF)
            if (!m_framePending)
            {
               // Block rendering until we will acquire swapchain again
               m_framePending = true;
               return;
            }

            // Submit frame to BGFX (which contains all rendering commands, for VR headset but also other windows like preview,...)
            {
               BEGIN_SPAN(tagSpan, "VPX->BGFX")
               std::lock_guard lock(m_frameMutex);
               g_pplayer->m_renderProfiler->NewFrame(g_pplayer->m_time_msec);
               g_pplayer->m_renderProfiler->EnterProfileSection(FrameProfiler::PROFILE_RENDER_SUBMIT);
               SubmitRenderFrame();
               g_pplayer->m_vrDevice->UpdateVisibilityMask(this);
               g_pplayer->m_renderProfiler->ExitProfileSection();
               END_SPAN(tagSpan)
            }

            // Request BGFX to submit to GPU (calls bgfx::frame())
            BEGIN_SPAN(tagSpan, "BGFX->GPU")
            g_pplayer->m_renderProfiler->EnterProfileSection(FrameProfiler::PROFILE_RENDER_FLIP);
            Flip();
            m_frameIndex++;
            {
               // Screenshot state is concurrently written by the logic thread in CaptureScreenshot
               std::lock_guard lock(m_screenshotMutex);
               if (m_screenshotFrameDelay > 0)
               {
                  m_screenshotFrameDelay--;
                  if (m_screenshotFrameDelay == 0)
                     for (size_t i = 0; i < m_screenshotWindow.size(); i++)
                        bgfx::requestScreenShot(m_screenshotWindow[i]->GetBackBuffer()->GetCoreFrameBuffer(), m_screenshotFilename[i].string().c_str());
               }
            }
            const bgfx::Stats* stats = bgfx::getStats();
            const uint64_t bgfxSubmit = (stats->cpuTimeEnd - stats->cpuTimeBegin) * 1000000ull / stats->cpuTimerFreq;
            g_pplayer->m_logicProfiler.OnPresented(usec() - bgfxSubmit);
            g_pplayer->m_renderProfiler->ExitProfileSection();
            g_pplayer->m_renderProfiler->AdjustBGFXSubmit(static_cast<uint32_t>(bgfxSubmit));
            END_SPAN(tagSpan)
         });
   }
   g_pplayer->m_vrDevice->ReleaseSession();
}
#endif

void RenderDevice::BGFXDesktopRenderLoop(const bgfx::Init& init)
{
   uint64_t lastSubmitTimestamp = 0;
   uint64_t lastSyncTimestamp = 0;
   bool bgfxVSync = false; // Is VSync requested on BGFX's Present operation (note that the VSync on Present will only block if the present queue is filled)
   int framePacingFlushing = 0;
   uint32_t lastFrameVSync = 0; // Id of the last frame when we performed a VBlank synchronization
   m_frameIndex = 0;
   std::array<uint64_t, 8> gpuLengths; // Ring buffer of last frame GPU lengths to compute average
   int gpuLengthPos = 0;
   uint32_t lastGpuFrameNum = 0;
   uint64_t avgGPUFrameLength = 0;

   const bool waitableSwapchain = (bgfx::getCaps()->supported & BGFX_CAPS_WAITABLE_SWAPCHAIN) != 0;
   if (waitableSwapchain)
      bgfx::waitForSwapchain();

#if BX_PLATFORM_WINDOWS
   // Use highest priority for better timing and lower jitter (as we are doing software pacing)
   SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

   m_presentMonProvider = PresentMonProvider_Initialize();
   if (m_presentMonProvider)
      PresentMonProvider_Application_SleepStart(m_presentMonProvider, m_frameIndex);
#endif

   // Desktop renderloop, synchronized on main display (playfield window), with game logic preparing frames as soon as possible
   while (m_renderDeviceAlive)
   {
      #ifdef __RK3588__
      m_loopTopUs = usec();
      #endif
      g_pplayer->m_renderProfiler->NewFrame(g_pplayer->m_time_msec);

      // wait for a frame to be prepared by the logic thread
      g_pplayer->m_renderProfiler->EnterProfileSection(FrameProfiler::PROFILE_RENDER_WAIT);
      m_frameReadySem.acquire();
      g_pplayer->m_renderProfiler->ExitProfileSection();

      if (!m_renderDeviceAlive)
         break;

      if (!m_framePending)
         continue;

      if (m_frameNoPresent)
      {
         std::lock_guard lock(m_frameMutex);
         SubmitRenderFrame();
         m_frameNoPresent = false;
         m_framePending = false;
         SubmitAndFlipFrame(false);
         continue;
      }

      const VideoSyncMode syncMode = g_pplayer->GetVideoSyncMode();
      const int64_t displayFrameLength = static_cast<int64_t>(1000000. / static_cast<double>(m_outputWnd[0]->GetRefreshRate())); // us

      // Toggle synchronisation against hardware VSync
      bool needsVSync;
      {
         if (syncMode != VideoSyncMode::VSM_FRAME_PACING)
         {
            // Use managed VSync setting
            needsVSync = syncMode != VideoSyncMode::VSM_NONE;
            m_renderLatency = !needsVSync ? -1.f : (static_cast<float>(m_lastPresentFrameIdx - bgfx::getStats()->gpuFrameNum) / m_outputWnd[0]->GetRefreshRate());
         }
         else if (waitableSwapchain)
         {
            // Perform a fixed pacing at the display rate and rely on swapchain synchronization to guarantee that we do not push more than one frame.
            const int64_t vpxToBGFX = static_cast<int64_t>(g_pplayer->m_renderProfiler->GetSlidingAvg(FrameProfiler::PROFILE_RENDER_SUBMIT));
            const int64_t renderLength = vpxToBGFX // VPX to BGFX submission
               + avgGPUFrameLength // GPU actual render length
               + 1000; // Magic value to account for the length of unmeasured operation (delay between submit and GPU start, present duration, ...)
            // Use VSync to prevent tearing if we have enough margin to not risk any stuttering
            needsVSync = renderLength < displayFrameLength;
            // If we are low on margin, we do still periodically realign on VBlank to prevent tearing but only when balls are stalled to avoid impacting gameplay
            needsVSync |= m_noMovingBalls && (lastFrameVSync + 200 < m_frameIndex);
         }
         else
         {
            // Evaluate number of 'frames in flight', that is to say frames that have been submitted to the GPU but not yet processed
            // We target 2 frames in flight (one just submitted, one being processed). If we have more than 3 we are in a situation
            // where the GPU is too much behind and we are piling up frames in the GPU queue, which is bad for latency. In this case,
            // we start a flush sequence:
            // - process a few frames without VSync to flush the queue (as they will be discarded or presented directly)
            // - then process a few frame with VSync enabled, to measure the new number of frames in flights
            // This is not really correct as gpuFrameNum is the last processed frame, not the last presented frame. Therefore
            // if all frames are quickly processed, gpuFrameNum will be the same as m_lastPresentFrameIdx, but the present queue
            // will be filled up anyway, leading to high latency. The user needs to limit the maximum number of prerendered frame to
            // avoid this situation. Still, the tests seem to show that the estimate is good enough.
            if (framePacingFlushing > 0)
               framePacingFlushing--;
            const uint32_t framesInFlight = m_lastPresentFrameIdx - bgfx::getStats()->gpuFrameNum;
            if (framesInFlight > 3)
               framePacingFlushing = 8;
            if (framesInFlight <= bgfx::getStats()->maxGpuLatency)
               m_renderLatency = framePacingFlushing ? -1.f : (static_cast<float>(framesInFlight) / m_outputWnd[0]->GetRefreshRate());
            needsVSync = framePacingFlushing < 4; // Frame pacing use VSync synchronization (not catching up or using swapchain synchronization)
         }
         if (needsVSync)
            lastFrameVSync = m_frameIndex;
         g_pplayer->m_lastFrameSyncOnVBlank = needsVSync;
      }
         
      // Handle backbuffer resize, surface lost and VSync toggling
      {
#if defined(__ANDROID__)
         void* nwh = SDL_GetPointerProperty(SDL_GetWindowProperties(m_outputWnd[0]->GetCore()), SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, NULL);
         static void* prevNwh = nwh;
         if (nwh != prevNwh)
         {
            prevNwh = nwh;
            if (nwh == nullptr)
               continue;

            bgfx::PlatformData pd = {};
            pd.nwh = nwh;
            bgfx::setPlatformData(pd);
            bgfxVSync = !needsVSync; // Force reset by making VSync state appear changed
         }
         if (nwh == nullptr)
            continue;
#endif
         if (bgfxVSync != needsVSync)
         {
            bgfxVSync = needsVSync;
            bgfx::reset(m_outputWnd[0]->GetBackBuffer()->GetWidth(), m_outputWnd[0]->GetBackBuffer()->GetHeight(), init.resolution.reset | (bgfxVSync ? BGFX_RESET_VSYNC : BGFX_RESET_NONE),
               init.resolution.formatColor);
         }
      }

      // Latency reduction by performing part of the sleep before submitting to BGFX (as we update ball position when submitting)
      // TODO This works but is disabled as it needs a dynamic stability margin evaluation to be fully robust
      if (false) {
         const int64_t latencySleepMargin = (displayFrameLength * 20) / 100; // 20% margin
         int64_t latencySleep;
         if (needsVSync)
         {
            // We do not have a direct measure of the sleep time (as it happens in the Present operation), estimate it from the render time against frame length
            const int64_t vpxToBGFX = static_cast<int64_t>(g_pplayer->m_renderProfiler->GetSlidingAvg(FrameProfiler::PROFILE_RENDER_SUBMIT));
            const int64_t renderLength = vpxToBGFX // VPX to BGFX submission
               + avgGPUFrameLength // GPU actual render length
               + 1000; // Magic value to account for the length of unmeasured operation (delay between submit and GPU start, present duration, ...)
            latencySleep = displayFrameLength - latencySleepMargin - renderLength;
         }
         else
         {
            // VSync may have been turned on/off making this measure imprecise but still a lower value than the the actual sleep (as the sleep time is 0 when VSync is on) so we can use it safely
            latencySleep = static_cast<int64_t>(g_pplayer->m_renderProfiler->GetSlidingAvg(FrameProfiler::PROFILE_RENDER_SLEEP)) - latencySleepMargin;
         }
         if (latencySleep > 0)
         {
            g_pplayer->m_renderProfiler->EnterProfileSection(FrameProfiler::PROFILE_RENDER_SLEEP);
            uSleep(latencySleep);
            g_pplayer->m_renderProfiler->ExitProfileSection();
         }
      }

#if BX_PLATFORM_WINDOWS
      if (m_presentMonProvider)
      {
         PresentMonProvider_Application_SleepEnd(m_presentMonProvider, m_frameIndex);
         PresentMonProvider_Application_SimulationStart(m_presentMonProvider, m_frameIndex);
      }
#endif

      // Lock prepared frame and let BGFX encode it (for PresentMon we consider this as the simulation since ball positions are updated here, this is not true for flipper bats though)
      {
         BEGIN_SPAN(tagSpan, "VPX->BGFX")
         g_pplayer->m_renderProfiler->EnterProfileSection(FrameProfiler::PROFILE_RENDER_SUBMIT);
         std::lock_guard lock(m_frameMutex);
         SubmitRenderFrame();

         // Handle swapchain resize while we hold the mutex on the render frame and before the backbuffer rendertarget are used for rendering
         for (VPX::Window* wnd : m_outputWnd)
         {
            const int windowWidth = wnd->GetPixelWidth();
            const int windowHeight = wnd->GetPixelHeight();
            const bool isMainSwpachain = wnd == m_outputWnd[0];
            if ((windowWidth != wnd->GetBackBuffer()->GetWidth()) || (windowHeight != wnd->GetBackBuffer()->GetHeight()))
            {
               // Request BGFX to process the submitted render frame before reseting / deleting the swapchain it rely on
               Flip();
               if (isMainSwpachain)
               {
                  bgfx::reset(windowWidth, windowHeight, init.resolution.reset | (bgfxVSync ? BGFX_RESET_VSYNC : BGFX_RESET_NONE), init.resolution.formatColor);
                  m_outputWnd[0]->GetBackBuffer()->SetSize(windowWidth, windowHeight);
               }
               else
               {
                  auto backbuffer = wnd->GetBackBuffer();
                  wnd->SetBackBuffer(nullptr);
                  RemoveWindow(wnd);
                  delete backbuffer;
                  bgfx::frame(BGFX_FRAME_FLUSH); // We must destroy the swapchain before attaching a new swapchain
                  AddWindow(wnd);
               }
               break;
            }
         }

         m_framePending = false;
         g_pplayer->m_renderProfiler->ExitProfileSection();
         END_SPAN(tagSpan)
      }

#if BX_PLATFORM_WINDOWS
      if (m_presentMonProvider)
      {
         PresentMonProvider_Application_SimulationEnd(m_presentMonProvider, m_frameIndex);
         // We do not track Render Start/End as BGFX performs the 2 directly and only the Present event is mandatory for PresentMon
         PresentMonProvider_Application_PresentStart(m_presentMonProvider, m_frameIndex);
      }
#endif

      // Submit from BGFX to GPU and schedule swapchain flip, eventually blocking until a VSYNC happens if enabled and the swapchain queue is filled
      {
         const uint64_t now = usec();
         BEGIN_SPAN(tagSpan, "BGFX->GPU")
         lastSubmitTimestamp = now;
         g_pplayer->m_renderProfiler->EnterProfileSection(FrameProfiler::PROFILE_RENDER_FLIP);
         Flip();
         g_pplayer->m_renderProfiler->ExitProfileSection();
         // Split time spent in Flip between GPU submission and time spent in GPU present
         if (!(syncMode == VideoSyncMode::VSM_FRAME_PACING && waitableSwapchain))
         {
            const bgfx::Stats* const stats = bgfx::getStats();
            const uint64_t bgfxSubmit = ((stats->cpuTimeEnd - stats->cpuTimeBegin) * 1000000ULL) / stats->cpuTimerFreq;
            g_pplayer->m_renderProfiler->AdjustBGFXSubmit(static_cast<uint32_t>(bgfxSubmit));
         }
         // If we have waited for a VSYNC, we can adjust the estimated present time to be just before the end of the wait
         if (needsVSync)
            m_presentTimestampReference = usec();
         END_SPAN(tagSpan)
      }

#if BX_PLATFORM_WINDOWS
      if (m_presentMonProvider)
         PresentMonProvider_Application_PresentEnd(m_presentMonProvider, m_frameIndex);
#endif

      // Next frame starts here (Sleep / Logic Thread -> Render Thread / VPX -> BGFX / BGFX -> GPU / Present)
      m_frameIndex++;

#if BX_PLATFORM_WINDOWS
      if (m_presentMonProvider)
         PresentMonProvider_Application_SleepStart(m_presentMonProvider, m_frameIndex);
#endif

      // Ensure we have an empty swapchain slot before submitting next frame to GPU
      if (syncMode == VideoSyncMode::VSM_FRAME_PACING && waitableSwapchain)
      {
         BEGIN_SPAN(tagSpan, "WaitSC")
         g_pplayer->m_renderProfiler->EnterProfileSection(FrameProfiler::PROFILE_RENDER_WAIT_SC);
         bgfx::waitForSwapchain();
         g_pplayer->m_renderProfiler->ExitProfileSection();
         // Evaluate latency as the delay between when we submitted the frame data and when the swapchain has an empty slot (as this denotes that the Present operation has been performed)
         const uint64_t now = usec();
         m_renderLatency = static_cast<float>((double)(now - lastSubmitTimestamp) / 1000000.0) // Time spent since pushing data to the GPU until consumed by swapchain
            + static_cast<float>(init.resolution.maxFrameLatency - 1) / m_outputWnd[0]->GetRefreshRate(); // Time that will be spent in the GPU queue before display (if any)
         END_SPAN(tagSpan)
      }

      // Software FPS throttling
      int64_t targetFrameLength = 0;
      if (syncMode == VideoSyncMode::VSM_FRAME_PACING && needsVSync)
      {
         // We rely on VSync for the sync, so disable software sync
      }
      else if (syncMode == VideoSyncMode::VSM_FRAME_PACING && !needsVSync)
      {
         // We are using frame pacing, that is to say we aim at low latency by trying to push frames in sync with the display rate to avoid piling up frames in the GPU queue
         targetFrameLength = displayFrameLength;
         // Little timing errors tends to accumulate over frames and would lead to a stutter when turning on VSync, so continuously compensate them
         int64_t accumulatedDeviation = (lastSyncTimestamp - m_presentTimestampReference) % targetFrameLength;
         if (accumulatedDeviation > targetFrameLength / 2)
            accumulatedDeviation -= targetFrameLength;
         targetFrameLength -= accumulatedDeviation;
      }
      else if (!needsVSync && g_pplayer->GetTargetRefreshRate() < 10000.f)
      {
         // User has disabled VSync with a FPS bound, so apply it
         targetFrameLength = static_cast<int64_t>(1000000. / static_cast<double>(g_pplayer->GetTargetRefreshRate()));
      }
      else if (needsVSync && g_pplayer->GetTargetRefreshRate() < m_outputWnd[0]->GetRefreshRate())
      {
         // User has enabled VSync with a max FPS below the display FPS
         // Keep some margin since, in the end, the sync will be done on hardware VSync (somewhat hacky, disallow VSync with low FPS ?)
         targetFrameLength = static_cast<int64_t>(1000000. / static_cast<double>(g_pplayer->GetTargetRefreshRate())) - 2000;
      }
      if (targetFrameLength)
      {
         BEGIN_SPAN(tagSpan, "WaitSync")
         g_pplayer->m_renderProfiler->EnterProfileSection(FrameProfiler::PROFILE_RENDER_SLEEP);
         const uint64_t now = usec();
         if (const uint64_t targetTimeStamp = lastSyncTimestamp + targetFrameLength; now < targetTimeStamp)
         {
            // PLOGI << std::format("Soft sleep: {:5.3f}ms", (targetTimeStamp - now) / 1000.);
            uSleep(targetTimeStamp - now);
         }
         g_pplayer->m_renderProfiler->ExitProfileSection();
         END_SPAN(tagSpan)
      }
      lastSyncTimestamp = usec();

      {
         // Push present event (used to evaluate input latency) as we have either waited for VSync or performed software FPS throttling
         g_pplayer->m_logicProfiler.OnPresented(lastSyncTimestamp);

         // Also collect frame stats as the frame is likely rendered at this point
         if (const bgfx::Stats* const stats = bgfx::getStats(); stats->gpuFrameNum != lastGpuFrameNum)
         {
            m_lastGPUFrameLength = (stats->gpuTimeEnd - stats->gpuTimeBegin) * 1000000ULL / stats->gpuTimerFreq;
            lastGpuFrameNum = stats->gpuFrameNum;
            gpuLengths[gpuLengthPos] = m_lastGPUFrameLength;
            gpuLengthPos = (gpuLengthPos + 1) % gpuLengths.size();
            uint64_t avg = 0;
            for (const uint64_t length : gpuLengths)
               avg += length;
            avgGPUFrameLength = avg / gpuLengths.size();
         }
      }

      // Screenshot handling (state is concurrently written by the logic thread in CaptureScreenshot)
      {
         std::lock_guard lock(m_screenshotMutex);
         if (!m_screenshotWindow.empty())
         {
            m_screenshotFrameDelay--;
            if (m_screenshotFrameDelay == 0)
               for (size_t i = 0; i < m_screenshotWindow.size(); i++)
                  bgfx::requestScreenShot(m_screenshotWindow[i]->GetBackBuffer()->GetCoreFrameBuffer(), m_screenshotFilename[i].string().c_str());
            else if (m_screenshotFrameDelay < -60)
            {
               // Sadly BGFX will silently fails screenshot capture, so if after 60 frames we did not get it, we try again
               PLOGE << "Screenshot capture timed out. Requesting it again";
               for (size_t i = 0; i < m_screenshotWindow.size(); i++)
                  bgfx::requestScreenShot(m_screenshotWindow[i]->GetBackBuffer()->GetCoreFrameBuffer(), m_screenshotFilename[i].string().c_str());
            }
         }
      }
   }

#if BX_PLATFORM_WINDOWS
   if (m_presentMonProvider)
   {
      PresentMonProvider_ShutDown(m_presentMonProvider);
      m_presentMonProvider = nullptr;
   }
#endif
}

#if BX_PLATFORM_WINDOWS
void RenderDevice::OnInputSampled()
{
   if (m_presentMonProvider)
      PresentMonProvider_Application_InputSample(m_presentMonProvider, m_frameIndex, PresentMonProvider_Input_NotSpecified);
}
#endif

#elif defined(ENABLE_OPENGL)
GLuint RenderDevice::m_samplerStateCache[3 * 3 * 6];
static const char* glErrorToString(const int error)
{
   switch (error)
   {
   case GL_INVALID_ENUM: return "GL_INVALID_ENUM";
   case GL_INVALID_VALUE: return "GL_INVALID_VALUE";
   case GL_INVALID_OPERATION: return "GL_INVALID_OPERATION";
#ifndef __OPENGLES__
   case GL_STACK_OVERFLOW: return "GL_STACK_OVERFLOW";
   case GL_STACK_UNDERFLOW: return "GL_STACK_UNDERFLOW";
#endif
   case GL_OUT_OF_MEMORY: return "GL_OUT_OF_MEMORY";
   case GL_INVALID_FRAMEBUFFER_OPERATION: return "GL_INVALID_FRAMEBUFFER_OPERATION";
   default: return "unknown";
   }
}

// Callback function for printing debug statements
#if defined(_DEBUG) && !defined(__OPENGLES__)
void APIENTRY GLDebugMessageCallback(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length, const GLchar* msg, const void* data)
{
   const char* _source;
   switch (source)
   {
   case GL_DEBUG_SOURCE_API: _source = "API"; break;
   case GL_DEBUG_SOURCE_WINDOW_SYSTEM: _source = "WINDOW SYSTEM"; break;
   case GL_DEBUG_SOURCE_SHADER_COMPILER: _source = "SHADER COMPILER"; break;
   case GL_DEBUG_SOURCE_THIRD_PARTY: _source = "THIRD PARTY"; break;
   case GL_DEBUG_SOURCE_APPLICATION: _source = "APPLICATION"; break;
   case GL_DEBUG_SOURCE_OTHER: _source = "UNKNOWN"; break;
   default: _source = "UNHANDLED"; break;
   }
   const char* _type;
   switch (type)
   {
   case GL_DEBUG_TYPE_ERROR: _type = "ERROR"; break;
   case GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR: _type = "DEPRECATED BEHAVIOR"; break;
   case GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR: _type = "UNDEFINED BEHAVIOR"; break;
   case GL_DEBUG_TYPE_PORTABILITY: _type = "PORTABILITY"; break;
   case GL_DEBUG_TYPE_PERFORMANCE: _type = "PERFORMANCE"; break;
   case GL_DEBUG_TYPE_OTHER: _type = "OTHER"; break;
   case GL_DEBUG_TYPE_MARKER: _type = "MARKER"; break;
   case GL_DEBUG_TYPE_PUSH_GROUP: _type = "GL_DEBUG_TYPE_PUSH_GROUP"; break;
   case GL_DEBUG_TYPE_POP_GROUP: _type = "GL_DEBUG_TYPE_POP_GROUP"; break;
   default: _type = "UNHANDLED"; break;
   }
   const char* _severity;
   switch (severity)
   {
   case GL_DEBUG_SEVERITY_HIGH: _severity = "HIGH"; break;
   case GL_DEBUG_SEVERITY_MEDIUM: _severity = "MEDIUM"; break;
   case GL_DEBUG_SEVERITY_LOW: _severity = "LOW"; break;
   case GL_DEBUG_SEVERITY_NOTIFICATION: _severity = "NOTIFICATION"; break;
   default: _severity = "UNHANDLED"; break;
   }
   //if (severity != GL_DEBUG_SEVERITY_NOTIFICATION)
   if (type != GL_DEBUG_TYPE_MARKER && type != GL_DEBUG_TYPE_PUSH_GROUP && type != GL_DEBUG_TYPE_POP_GROUP)
   {
      PLOGE << "OpenGL Msg #" << id << " [" << _severity << '/' << _type << " from " << _source << "]: " << msg;
   }
}
#endif
void RenderDevice::CaptureGLScreenshot()
{
   assert(m_screenshotFilename.size() == 1);
   const std::filesystem::path screenshotFilename = m_screenshotFilename[0];
   m_screenshotFilename.clear();
   m_screenshotFrameDelay = 0;
   bool success = false;
   int width = m_outputWnd[0]->GetWidth();
   int height = m_outputWnd[0]->GetHeight();
   if (auto tex = BaseTexture::Create(width, height, BaseTexture::SRGBA); tex)
   {
      m_outputWnd[0]->GetBackBuffer()->Activate();
      glPixelStorei(GL_PACK_ALIGNMENT, 1);
      glReadBuffer(GL_BACK);
      glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, tex->data());
      tex->FlipY();
      success = tex->Save(screenshotFilename);
   }
   m_screenshotCallback(success);
}

#elif defined(ENABLE_DX9)
#include <DxErr.h>
#pragma comment(lib, "legacy_stdio_definitions.lib") //dxerr.lib needs this
static constexpr D3DVERTEXELEMENT9 VertexTexelElement[] = { { 0, 0 * sizeof(float), D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 }, // pos
   { 0, 3 * sizeof(float), D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 }, // tex0
   D3DDECL_END() };
static constexpr D3DVERTEXELEMENT9 VertexNormalTexelElement[] = { { 0, 0 * sizeof(float), D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 }, // pos
   { 0, 3 * sizeof(float), D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL, 0 }, // normal
   { 0, 6 * sizeof(float), D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 }, // tex0
   D3DDECL_END() };

void RenderDevice::CaptureDX9Screenshot()
{
   assert(m_screenshotFilename.size() == 1);
   const std::filesystem::path screenshotFilename = m_screenshotFilename[0];
   m_screenshotFilename.clear();
   bool success = false;
   m_screenshotFrameDelay = 0;
   IDirect3DDevice9* pd3dDevice = GetCoreDevice();
   IDirect3DSurface9* pBackBuffer = NULL;
   if (FAILED(pd3dDevice->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &pBackBuffer)))
   {
      m_screenshotCallback(false);
      return;
   }
   D3DSURFACE_DESC desc;
   pBackBuffer->GetDesc(&desc);
   LPDIRECT3DSURFACE9 pSurface = NULL;
   if (FAILED(pd3dDevice->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &pSurface, NULL)))
   {
      pBackBuffer->Release();
      m_screenshotCallback(false);
      return;
   }
   if (FAILED(pd3dDevice->GetRenderTargetData(pBackBuffer, pSurface)))
   {
      pSurface->Release();
      pBackBuffer->Release();
      m_screenshotCallback(false);
      return;
   }
   D3DLOCKED_RECT lockedRect;
   if (FAILED(pSurface->LockRect(&lockedRect, nullptr, D3DLOCK_READONLY)))
   {
      pSurface->Release();
      pBackBuffer->Release();
      m_screenshotCallback(false);
      return;
   }
   auto tex = BaseTexture::Create(desc.Width, desc.Height, BaseTexture::SRGBA);
   if (tex)
   {
      uint8_t* const __restrict bits = static_cast<uint8_t*>(tex->data());
      const uint8_t* const __restrict pixels = static_cast<uint8_t*>(lockedRect.pBits);
      copy_bgra_rgba<true>((unsigned int*)(tex->data()), (const unsigned int*)lockedRect.pBits, desc.Width * desc.Height); // Backbuffer is BGRA
      //memcpy(bits, pixels, lockedRect.Pitch * desc.Height);
      for (unsigned int i = 0; i < desc.Height; ++i)
         for (unsigned int j = 0; j < desc.Width; ++j)
            bits[i * lockedRect.Pitch + j * 4 + 3] = 0xFF; // Make the image opaque
      success = tex->Save(screenshotFilename);
   }
   pSurface->Release();
   pBackBuffer->Release();
   m_screenshotCallback(success);
}

#endif


////////////////////////////////////////////////////////////////////

RenderDeviceState::RenderDeviceState(RenderDevice* rd)
   : m_rd(rd)
   , m_uiShaderState(new ShaderState(m_rd->m_uiShader, m_rd->UseLowPrecision()))
   , m_basicShaderState(new ShaderState(m_rd->m_basicShader, m_rd->UseLowPrecision()))
   , m_DMDShaderState(new ShaderState(m_rd->m_DMDShader, m_rd->UseLowPrecision()))
   , m_FBShaderState(new ShaderState(m_rd->m_FBShader, m_rd->UseLowPrecision()))
   , m_flasherShaderState(new ShaderState(m_rd->m_flasherShader, m_rd->UseLowPrecision()))
   , m_lightShaderState(new ShaderState(m_rd->m_lightShader, m_rd->UseLowPrecision()))
   , m_ballShaderState(new ShaderState(m_rd->m_ballShader, m_rd->UseLowPrecision()))
   , m_stereoShaderState(m_rd->m_stereoShader ? new ShaderState(m_rd->m_stereoShader, m_rd->UseLowPrecision()) : nullptr)
{
}

RenderDeviceState::~RenderDeviceState()
{
   delete m_uiShaderState;
   delete m_basicShaderState;
   delete m_DMDShaderState;
   delete m_FBShaderState;
   delete m_flasherShaderState;
   delete m_lightShaderState;
   delete m_ballShaderState;
   delete m_stereoShaderState;
}

////////////////////////////////////////////////////////////////////

#if defined(ENABLE_BGFX)
// Human-readable name for a bgfx renderer type. Index bgfx::RendererType::Count maps to "Default"
// (let bgfx auto-select the platform default). Keep aligned with the bgfx::RendererType enum.
static const string& bgfxRendererName(const bgfx::RendererType::Enum type)
{
   // One entry per bgfx::RendererType, plus a trailing "Default" for RendererType::Count (auto-select).
   static const string names[]
      = { "Noop"s, "Agc"s, "Direct3D11"s, "Direct3D12"s, "Gnm"s, "Metal"s, "Nvn"s, "OpenGLES"s, "OpenGL"s, "Vulkan"s, "WebGPU"s, "Default"s };
   static_assert(std::size(names) == bgfx::RendererType::Count + 1,
      "bgfxRendererName is out of sync with bgfx::RendererType - add/remove a name when bgfx changes its renderer list");
   return names[type];
}

std::vector<std::string> RenderDevice::GetSelectableBackendNames()
{
   bgfx::RendererType::Enum supported[bgfx::RendererType::Count];
   const int n = bgfx::getSupportedRenderers(bgfx::RendererType::Count, supported);
   std::vector<std::string> result;
   for (int i = 0; i < n; ++i)
   {
      const bgfx::RendererType::Enum renderer = supported[i];
      if (renderer == bgfx::RendererType::Noop || renderer == bgfx::RendererType::WebGPU)
         continue; // no-op / web backend, not a usable desktop choice
      #if !defined(_DEBUG) && !defined(ENABLE_BGFX_DX12)
      if (renderer == bgfx::RendererType::Direct3D12)
         continue;
      #endif
      result.push_back(bgfxRendererName(renderer));
   }
   return result;
}
#endif

RenderDevice::RenderDevice(
   VPX::Window* const wnd, const bool isStereo, const bool isAnaglyph, const bool isVR, const bool useNvidiaApi, const bool compressTextures, int nMSAASamples, VideoSyncMode& syncMode)
   : m_texMan(*this)
   , m_compressTextures(compressTextures)
   , m_nEyes(isStereo ? 2 : 1)
   , m_isAnaglyph(isAnaglyph)
   , m_isVR(isVR)
   #ifdef ENABLE_BGFX
   , m_bgfxCallback(*this)
   #endif
{
   // Main render target (playfield window or VR target)
   m_outputWnd.push_back(wnd);
   VPX::Window* swapchainWnd = wnd;

   // Create preview in the render device as it holds the desktop swapchain (not really clean and should be refactored for all windows to be added/removed by the client)
   if (isVR && !g_isAndroid)
   {
      VPX::Window* previewWnd = new VPX::Window("Visual Pinball VR Preview"s, g_pplayer->m_ptable->m_settings, VPXWindowId::VPXWINDOW_VRPreview);
#ifdef ENABLE_BGFX
      // Color and depth format are likely wrong => use the ones selected by the OpenXR backend
      RenderTarget* backbuffer = new RenderTarget(this, SurfaceType::RT_DEFAULT, BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE, bgfx::TextureFormat::RGBA8, BGFX_INVALID_HANDLE,
         bgfx::TextureFormat::D32F, "BackBuffer", previewWnd->GetPixelWidth(), previewWnd->GetPixelHeight(), colorFormat::RGBA8);
#else
      RenderTarget* backbuffer = new RenderTarget(this, SurfaceType::RT_DEFAULT, previewWnd->GetPixelWidth(), previewWnd->GetPixelHeight(), colorFormat::RGBA8);
#endif
      previewWnd->SetBackBuffer(backbuffer, false);
      previewWnd->Show();
      previewWnd->RaiseAndFocus();
      m_outputWnd.push_back(previewWnd);
      swapchainWnd = previewWnd;
   }

   assert(!isVR || m_nEyes == 2);

   #if defined(ENABLE_DX9)
      m_useNvidiaApi = useNvidiaApi;
      m_INTZ_support = false;
      NVAPIinit = false;
   #endif

   #if !defined(__STANDALONE__) && !defined(ENABLE_BGFX)
      BOOL dwm = 0;
      DwmIsCompositionEnabled(&dwm);
      m_dwm_enabled = !!dwm;
   #endif

   assert(g_pplayer != nullptr); // Player must be created to give access to the output window

   // 0 means disable limiting of draw-ahead queue
   int maxPrerenderedFrames = isVR ? 0 : g_pplayer->m_ptable->m_settings.GetPlayer_MaxPrerenderedFrames();

#if defined(ENABLE_BGFX)
   ///////////////////////////////////
   // BGFX device initialization
   bgfx::Init init;

   #ifdef __RK3588__
   // Mali dynamic-buffer write-hazard fix (mali-optimized.md §8): drawn dynamic buffers take one
   // transient snapshot of their whole shared block per frame, so the ring must hold every drawn
   // dynamic block, not just the quad traffic. 32/8 MB is generous headroom; the frame stats'
   // 'transient peak' line reports actual use.
   RenderDevice::s_dynBufferShadow = g_pplayer->m_ptable->m_settings.GetStandalone_4kpDynamicBufferShadow();
   if (RenderDevice::s_dynBufferShadow)
   {
      init.limits.maxTransientVbSize = 32u << 20;
      init.limits.maxTransientIbSize = 8u << 20;
   }
   #endif

   // Adaptive VSync is not implemented for BGFX
   if (syncMode == VideoSyncMode::VSM_ADAPTIVE_VSYNC)
      syncMode = VideoSyncMode::VSM_VSYNC;
   
   // Select backend
   const string& gfxBackend = g_pplayer->m_ptable->m_settings.GetPlayer_GfxBackend();
   bgfx::RendererType::Enum supportedRenderers[bgfx::RendererType::Count];
   const int nRendererSupported = bgfx::getSupportedRenderers(bgfx::RendererType::Count, supportedRenderers);
   init.type = bgfx::RendererType::Count; // Tells BGFX to select the default backend for the running platform
   bool backendMatched = false;
   for (int i = 0; i < nRendererSupported; ++i)
      if (gfxBackend == bgfxRendererName(supportedRenderers[i]))
      {
         init.type = supportedRenderers[i];
         backendMatched = true;
      }
   // Valid GfxBackend values: 'Default' (let BGFX auto-select the platform default) plus the backends
   // usable on this platform (same list as the in-game graphics settings).
   string validBackends = "Default"s;
   for (const string& name : GetSelectableBackendNames())
      validBackends += ", " + name;
   // The setting is case sensitive and an unknown/unsupported value silently falls back to the platform
   // default, so warn rather than leave the user guessing (e.g. 'opengl' instead of 'OpenGL').
   if (!backendMatched && !gfxBackend.empty() && gfxBackend != "Default"s) {
      PLOGW << "Ignoring unknown or unsupported graphics backend '" << gfxBackend << "' (case sensitive), using platform default. Valid values: " << validBackends;
   }
#if !defined(_DEBUG) && !defined(ENABLE_BGFX_DX12)
   if (init.type == bgfx::RendererType::Direct3D12)
      init.type = bgfx::RendererType::Count;
#endif
   if (init.type == bgfx::RendererType::Noop)
      init.type = bgfx::RendererType::Count;
   if (g_pplayer->m_vrDevice == nullptr)
   {
      // Requested only: BGFX may fall back to another backend if the requested one cannot initialize. The
      // backend actually selected is logged from the render thread once BGFX is initialized ("BGFX
      // initialized using ... backend").
      PLOGI << "Requested graphics backend: " << (init.type == bgfx::RendererType::Count ? "Default (auto-selected by BGFX)"s : bgfxRendererName(init.type))
            << " (valid values: " << validBackends << ')';
   }

   #ifndef __LIBVPINBALL__
   m_useLowPrecision = init.type == bgfx::RendererType::OpenGLES;
   #else
   m_useLowPrecision = true;
   #endif

   init.callback = &m_bgfxCallback;
   init.fallback = true;
   init.resolution.width = swapchainWnd->GetPixelWidth();
   init.resolution.height = swapchainWnd->GetPixelHeight();
   init.platformData.context = nullptr;
   init.platformData.backBuffer = nullptr;
   init.platformData.backBufferDS = nullptr;
   #if BX_PLATFORM_LINUX || BX_PLATFORM_BSD
   if (SDL_GetCurrentVideoDriver() == "x11"sv) {
      init.platformData.ndt = SDL_GetPointerProperty(SDL_GetWindowProperties(swapchainWnd->GetCore()), SDL_PROP_WINDOW_X11_DISPLAY_POINTER, NULL);
      init.platformData.nwh = (void*)SDL_GetNumberProperty(SDL_GetWindowProperties(swapchainWnd->GetCore()), SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
   }
   else if (SDL_GetCurrentVideoDriver() == "wayland"sv) {
      init.platformData.type = bgfx::NativeWindowHandleType::Wayland;
      init.platformData.ndt = SDL_GetPointerProperty(SDL_GetWindowProperties(swapchainWnd->GetCore()), SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, NULL);
      init.platformData.nwh = SDL_GetPointerProperty(SDL_GetWindowProperties(swapchainWnd->GetCore()), SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, NULL);
   }
   else if (SDL_GetCurrentVideoDriver() == "kmsdrm"sv) {
      /* VPINBALL/4kp: upstream handles only x11 and wayland here, so on KMSDRM ndt/nwh stayed null
         and BGFX init failed outright ("BGFX initialization failed").

         SDL owns the KMSDRM/GBM objects; hand them to BGFX as the native display/window. A
         gbm_device/gbm_surface pair is not something the legacy EGL entry points can classify, so
         BGFX_USE_GBM tells our patched glcontext_egl to use the GBM EGL platform for them (see
         BGFX_PATCHSET=kmsdrm-gbm-egl-001). Must be set before the render thread runs bgfx::init(). */
      const SDL_PropertiesID wndProps = SDL_GetWindowProperties(swapchainWnd->GetCore());
      init.platformData.ndt = SDL_GetPointerProperty(wndProps, SDL_PROP_WINDOW_KMSDRM_GBM_DEVICE_POINTER, NULL);
      init.platformData.nwh = SDL_GetPointerProperty(wndProps, "SDL.window.kmsdrm.gbm_surface", NULL);
      setenv("BGFX_USE_GBM", "1", 1);

      /* Vulkan is not usable on this Mali/KMSDRM stack, and BGFX's auto-selection prefers it. Pin
         the GLES backend unless the user explicitly asked for something in the settings. */
      if (init.type == bgfx::RendererType::Count)
         init.type = bgfx::RendererType::OpenGLES;

      PLOGI << "KMSDRM detected: handing BGFX gbm_dev=" << init.platformData.ndt
            << " gbm_surface=" << init.platformData.nwh << ", backend " << bgfxRendererName(init.type);
   }
   #elif BX_PLATFORM_OSX
   init.platformData.nwh = SDL_GetRenderMetalLayer(SDL_CreateRenderer(swapchainWnd->GetCore(), "Metal"));
   #elif BX_PLATFORM_IOS
   init.platformData.nwh = VPinballLib::VPinballLib::Instance().GetMetalLayer();
   #elif BX_PLATFORM_ANDROID
   init.platformData.nwh = SDL_GetPointerProperty(SDL_GetWindowProperties(swapchainWnd->GetCore()), SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, NULL);
   #elif BX_PLATFORM_WINDOWS
   init.platformData.nwh = swapchainWnd->GetNativeHWND();
   #elif BX_PLATFORM_STEAMLINK
   init.platformData.ndt = wmInfo.info.vivante.display;
   init.platformData.nwh = wmInfo.info.vivante.window;
   #endif // BX_PLATFORM_
   #ifdef DEBUG
   // Disable Direct3D12 debug layer as it crashes on some NVIDIA drivers
   init.debug = true && (init.type != bgfx::RendererType::Direct3D12);
   //init.profile = true;
   #endif

   ResetActiveView();

   m_frameMutex.lock();
   m_renderDeviceAlive = true;
   m_renderThread = std::thread(&RenderThread, this, init);
   while (!m_rendererInitialized.try_acquire())
   {
      g_pplayer->ProcessOSMessages(false);
      Sleep(0);
   }

#elif defined(ENABLE_OPENGL)
   ///////////////////////////////////
   // OpenGL device initialization
   const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(m_outputWnd[0]->GetCore()));
   if (mode == nullptr)
   {
      ShowError("Failed to setup OpenGL context");
      exit(-1);
   }
   colorFormat back_buffer_format;
   switch (mode->format)
   {
   case SDL_PIXELFORMAT_RGB565: back_buffer_format = colorFormat::RGB5; break;
   case SDL_PIXELFORMAT_XRGB8888: back_buffer_format = colorFormat::RGB8; break;
   case SDL_PIXELFORMAT_ARGB8888: back_buffer_format = colorFormat::RGBA8; break;
   case SDL_PIXELFORMAT_ARGB2101010: back_buffer_format = colorFormat::RGBA10; break;
   #ifdef __OPENGLES__
   case SDL_PIXELFORMAT_ABGR8888: back_buffer_format = colorFormat::RGBA8; break;
   case SDL_PIXELFORMAT_RGBX8888: back_buffer_format = colorFormat::RGBA8; break;
   case SDL_PIXELFORMAT_RGBA8888: back_buffer_format = colorFormat::RGBA8; break;
   #endif
   default:
   {
      ShowError("Invalid Output format: " + std::to_string(mode->format));
      exit(-1);
   }
   }

   memset(m_samplerStateCache, 0, sizeof(m_samplerStateCache));

   #if defined(__OPENGLES__) || (defined(__APPLE__) && (defined(TARGET_OS_IOS) && TARGET_OS_IOS))
   m_useLowPrecision = true;
   #else
   m_useLowPrecision = false;
   #endif

   // FIXME We only set bit depth for fullscreen desktop modes (otherwise, use the desktop bit depth)
   int channelDepth = m_outputWnd[0]->GetBitDepth() == 32 ?  8 :
                      m_outputWnd[0]->GetBitDepth() == 30 ? 10 :
                                                             5;
   if (m_outputWnd[0]->GetWindowMode() == VPX::Window::WindowMode::ExclusiveFullscreen)
   {
      SDL_GL_SetAttribute(SDL_GL_RED_SIZE, channelDepth);
      SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, channelDepth);
      SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, channelDepth);
      SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 0);
      SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
   }

   // Multisampling is performed on the offscreen buffers, not the window framebuffer
   SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 0);
   SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 0);

   #ifndef __OPENGLES__
      #if defined(__APPLE__) && defined(TARGET_OS_MAC)
         SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
         SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
         SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
         SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
      #else
         SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
         //This would enforce a 4.1 context, disabling all recent features (storage buffers, debug information,...)
         //SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
         //SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
      #endif
   #else
      SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
      SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
      SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
   #endif

   SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

   m_sdl_context = SDL_GL_CreateContext(m_outputWnd[0]->GetCore());

   SDL_GL_MakeCurrent(m_outputWnd[0]->GetCore(), m_sdl_context);

   #ifndef __OPENGLES__
   if (!gladLoadGL((GLADloadfunc)SDL_GL_GetProcAddress))
   #else
   if (!gladLoadGLES2((GLADloadfunc)SDL_GL_GetProcAddress))
   #endif
   {
      ShowError("Glad failed");
      exit(-1);
   }

   #ifdef __STANDALONE__
   unsigned int num_exts_i = 0;
   glad_glGetIntegerv(GL_NUM_EXTENSIONS, (int*) &num_exts_i);
   PLOGD.printf("%d extensions available", num_exts_i);
   for(int index = 0; index < num_exts_i; index++) {
      PLOGD << glad_glGetStringi(GL_EXTENSIONS, index);
   }
   #ifdef __OPENGLES__
   int range[2];
   int precision;
   glGetShaderPrecisionFormat(GL_VERTEX_SHADER, GL_HIGH_FLOAT, range, &precision);
   PLOGD.printf("Vertex shader high precision float range: %d %d precision: %d", range[0], range[1], precision);
   glGetShaderPrecisionFormat(GL_FRAGMENT_SHADER, GL_HIGH_FLOAT, range, &precision);
   PLOGD.printf("Fragment shader high precision float range: %d %d precision: %d", range[0], range[1], precision);
   #endif
   #endif

   const char* renderer = (char*)glGetString(GL_RENDERER);
   const char* vendor = (char*)glGetString(GL_VENDOR);
   if (renderer)
      m_GPU_name = renderer;
   if (vendor)
      m_driver_name = vendor;

   int gl_majorVersion = 0;
   int gl_minorVersion = 0;
   glGetIntegerv(GL_MAJOR_VERSION, &gl_majorVersion);
   glGetIntegerv(GL_MINOR_VERSION, &gl_minorVersion);

   #ifndef __STANDALONE__
   if (gl_majorVersion < 4 || (gl_majorVersion == 4 && gl_minorVersion < 3))
   {
      const string errorMsg = "Your graphics card only supports OpenGL " + std::to_string(gl_majorVersion) + '.' + std::to_string(gl_minorVersion) + ", but VPX requires OpenGL 4.3 or newer.";
      ShowError(errorMsg);
      exit(-1);
   }
   #endif

   m_driver_name += "(OpenGL " + std::to_string(gl_majorVersion) + '.' + std::to_string(gl_minorVersion) + ')';

   m_GLversion = gl_majorVersion * 100 + gl_minorVersion;

   // Enable debugging layer of OpenGL
   #if defined(_DEBUG) && !defined(__OPENGLES__)
   glEnable(GL_DEBUG_OUTPUT); // on its own is the 'fast' version
   //glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS); // callback is in sync with errors, so a breakpoint can be placed on the callback in order to get a stacktrace for the GL error
   if (glad_glDebugMessageCallback)
   {
      glDebugMessageCallback(GLDebugMessageCallback, nullptr);
   }
   #endif
   #if 0
   glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DONT_CARE, 0, nullptr, GL_FALSE); // disable all
   glDebugMessageControl(GL_DEBUG_SOURCE_API, GL_DEBUG_TYPE_ERROR, GL_DONT_CARE, 0, nullptr, GL_TRUE); // enable only errors
   #endif

   // Flip scheduling: 0 for immediate, 1 for synchronized with the vertical retrace, -1 for adaptive vsync (i.e. synchronized on vsync except for late frame)
   switch (syncMode)
   {
   case VideoSyncMode::VSM_NONE: SDL_GL_SetSwapInterval(0); break;
   case VideoSyncMode::VSM_VSYNC: SDL_GL_SetSwapInterval(1); break;
   case VideoSyncMode::VSM_ADAPTIVE_VSYNC: SDL_GL_SetSwapInterval(-1); break;
   case VideoSyncMode::VSM_FRAME_PACING: SDL_GL_SetSwapInterval(0); break;
   default: break;
   }

   m_maxaniso = 0;
   glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &m_maxaniso);
   int max_frag_unit, max_combined_unit;
   glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &max_frag_unit);
   glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &max_combined_unit);
   int n_tex_units = min(max_frag_unit, max_combined_unit);
   for (int i = 0; i < n_tex_units; i++)
   {
      Sampler::SamplerBinding* binding = new Sampler::SamplerBinding();
      binding->unit = i;
      binding->use_rank = i;
      binding->sampler = nullptr;
      binding->filter = SamplerFilter::SF_UNDEFINED;
      binding->clamp_u = SamplerAddressMode::SA_UNDEFINED;
      binding->clamp_v = SamplerAddressMode::SA_UNDEFINED;
      m_samplerBindings.push_back(binding);
   }

   SetRenderState(RenderState::ZFUNC, RenderState::Z_LESSEQUAL);

   // Retrieve a reference to the back buffer.
   wnd->SetBackBuffer(new RenderTarget(this, SurfaceType::RT_DEFAULT, wnd->GetWidth(), wnd->GetHeight(), back_buffer_format));

#elif defined(ENABLE_DX9)
   ///////////////////////////////////
   // DirectX 9 device initialization

   m_pD3DEx = nullptr;
   m_pD3DDeviceEx = nullptr;

   m_useLowPrecision = false;

   HRESULT hr = Direct3DCreate9Ex(D3D_SDK_VERSION, &m_pD3DEx);
   if (FAILED(hr) || (m_pD3DEx == nullptr))
   {
      ShowError("Could not create D3D9Ex object.");
      throw 0;
   }
   m_pD3DEx->QueryInterface(__uuidof(IDirect3D9), reinterpret_cast<void**>(&m_pD3D));

   constexpr UINT adapterId = D3DADAPTER_DEFAULT;

   D3DADAPTER_IDENTIFIER9 adapterInfo;
   if (SUCCEEDED(m_pD3DEx->GetAdapterIdentifier(adapterId, 0, &adapterInfo)))
   {
      m_GPU_name = adapterInfo.Description;
      m_driver_name = adapterInfo.Driver;
   }

   constexpr D3DDEVTYPE devtype = D3DDEVTYPE_HAL;
   D3DCAPS9 caps;
   m_pD3D->GetDeviceCaps(adapterId, devtype, &caps);

    // check which parameters can be used for anisotropic filter
    m_mag_aniso = (caps.TextureFilterCaps & D3DPTFILTERCAPS_MAGFANISOTROPIC) != 0;
    m_maxaniso = caps.MaxAnisotropy;
    memset(m_bound_filter, 0xCC, TEXTURESET_STATE_CACHE_SIZE * sizeof(SamplerFilter));
    memset(m_bound_clampu, 0xCC, TEXTURESET_STATE_CACHE_SIZE * sizeof(SamplerAddressMode));
    memset(m_bound_clampv, 0xCC, TEXTURESET_STATE_CACHE_SIZE * sizeof(SamplerAddressMode));

    if (((caps.TextureCaps & D3DPTEXTURECAPS_NONPOW2CONDITIONAL) != 0) || ((caps.TextureCaps & D3DPTEXTURECAPS_POW2) != 0))
        ShowError("D3D device does only support power of 2 textures");

    // get the current display format
    D3DFORMAT format;
    if (m_outputWnd[0]->GetWindowMode() != VPX::Window::WindowMode::ExclusiveFullscreen)
    {
       D3DDISPLAYMODE mode;
       CHECKD3D(m_pD3D->GetAdapterDisplayMode(adapterId, &mode));
       format = mode.Format;
    }
    else
    {
       format = m_outputWnd[0]->GetBitDepth() == 32 ? D3DFMT_X8R8G8B8 :
                m_outputWnd[0]->GetBitDepth() == 30 ? D3DFMT_A2R10G10B10 :
                                                      D3DFMT_R5G6B5;
    }
    colorFormat back_buffer_format;
    switch (format)
    {
    case D3DFMT_R5G6B5: back_buffer_format = colorFormat::RGB5; break;
    case D3DFMT_X8R8G8B8: back_buffer_format = colorFormat::RGB8; break;
    case D3DFMT_A8R8G8B8: back_buffer_format = colorFormat::RGBA8; break;
    case D3DFMT_A2R10G10B10: back_buffer_format = colorFormat::RGBA10; break;
    default:
    {
        ShowError("Invalid Output format: " + std::to_string(format));
        exit(-1);
    }
    }

    D3DPRESENT_PARAMETERS params;
    params.BackBufferWidth = wnd->GetWidth();
    params.BackBufferHeight = wnd->GetHeight();
    params.BackBufferFormat = format;
    params.BackBufferCount = 1;
    params.MultiSampleType = D3DMULTISAMPLE_NONE;
    params.MultiSampleQuality = 0;
    params.SwapEffect = D3DSWAPEFFECT_DISCARD;
    params.hDeviceWindow = m_outputWnd[0]->GetNativeHWND();
    params.Windowed = m_outputWnd[0]->GetWindowMode() != VPX::Window::WindowMode::ExclusiveFullscreen;
    params.EnableAutoDepthStencil = FALSE;
    params.AutoDepthStencilFormat = D3DFMT_UNKNOWN; // ignored
    params.Flags = /*fullscreen ? D3DPRESENTFLAG_LOCKABLE_BACKBUFFER :*/ /*(stereo3D ?*/ 0 /*: D3DPRESENTFLAG_DISCARD_DEPTHSTENCIL)*/
       ; // D3DPRESENTFLAG_LOCKABLE_BACKBUFFER only needed for SetDialogBoxMode() below, but makes rendering slower on some systems :/
    params.FullScreen_RefreshRateInHz = m_outputWnd[0]->GetWindowMode() == VPX::Window::WindowMode::ExclusiveFullscreen ? (UINT)m_outputWnd[0]->GetRefreshRate() : 0;
    params.PresentationInterval = syncMode == VideoSyncMode::VSM_VSYNC ? D3DPRESENT_INTERVAL_ONE : D3DPRESENT_INTERVAL_IMMEDIATE;

   // check if our HDR texture format supports/does sRGB conversion on texture reads, which must NOT be the case as we always set SRGBTexture=true independent of the format!
   hr = m_pD3D->CheckDeviceFormat(adapterId, devtype, params.BackBufferFormat, D3DUSAGE_QUERY_SRGBREAD, D3DRTYPE_TEXTURE, (D3DFORMAT)colorFormat::RGBA32F);
   if (SUCCEEDED(hr))
      ShowError("D3D device does support D3DFMT_A32B32G32R32F SRGBTexture reads (which leads to wrong tex colors)");
   // now the same for our LDR/8bit texture format the other way round
   hr = m_pD3D->CheckDeviceFormat(adapterId, devtype, params.BackBufferFormat, D3DUSAGE_QUERY_SRGBREAD, D3DRTYPE_TEXTURE, (D3DFORMAT)colorFormat::RGBA8);
   if (!SUCCEEDED(hr))
      ShowError("D3D device does not support D3DFMT_A8R8G8B8 SRGBTexture reads (which leads to wrong tex colors)");

   // check if auto generation of mipmaps can be used, otherwise will be done via d3dx
   m_autogen_mipmap = (caps.Caps2 & D3DCAPS2_CANAUTOGENMIPMAP) != 0;
   if (m_autogen_mipmap)
      m_autogen_mipmap = (m_pD3D->CheckDeviceFormat(adapterId, devtype, params.BackBufferFormat, textureUsage::AUTOMIPMAP, D3DRTYPE_TEXTURE, (D3DFORMAT)colorFormat::RGBA8) == D3D_OK);

   //m_autogen_mipmap = false; //!! could be done to support correct sRGB/gamma correct generation of mipmaps which is not possible with auto gen mipmap in DX9! at the moment disabled, as the sRGB software path is super slow for similar mipmap filter quality

   #ifndef DISABLE_FORCE_NVIDIA_OPTIMUS
   if (!NVAPIinit && NvAPI_Initialize() == NVAPI_OK)
      NVAPIinit = true;
   #endif

   // Determine if INTZ is supported
   m_INTZ_support = (m_pD3D->CheckDeviceFormat(adapterId, devtype, params.BackBufferFormat, D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_TEXTURE, ((D3DFORMAT)(MAKEFOURCC('I','N','T','Z'))))) == D3D_OK;

   // check if requested MSAA is possible
   DWORD MultiSampleQualityLevels;
   if (!SUCCEEDED(m_pD3D->CheckDeviceMultiSampleType(adapterId,
      devtype, params.BackBufferFormat,
      params.Windowed, params.MultiSampleType, &MultiSampleQualityLevels)))
   {
      ShowError("D3D device does not support this MultiSampleType");
      params.MultiSampleType = D3DMULTISAMPLE_NONE;
      params.MultiSampleQuality = 0;
   }
   else
      params.MultiSampleQuality = min(params.MultiSampleQuality, MultiSampleQualityLevels);

   const bool softwareVP = g_pplayer->m_ptable->m_settings.GetPlayer_SoftwareVertexProcessing();
   const DWORD flags = softwareVP ? D3DCREATE_SOFTWARE_VERTEXPROCESSING : D3DCREATE_HARDWARE_VERTEXPROCESSING;

   // Create the D3Dex device. This optionally goes to the proper fullscreen mode.
   // It also creates the default swap chain (front and back buffer).
   {
      D3DDISPLAYMODEEX mode;
      mode.Size = sizeof(D3DDISPLAYMODEEX);
      if (m_outputWnd[0]->GetWindowMode() == VPX::Window::WindowMode::ExclusiveFullscreen)
      {
         mode.Format = params.BackBufferFormat;
         mode.Width = params.BackBufferWidth;
         mode.Height = params.BackBufferHeight;
         mode.RefreshRate = params.FullScreen_RefreshRateInHz;
         mode.ScanLineOrdering = D3DSCANLINEORDERING_PROGRESSIVE;
      }

      hr = m_pD3DEx->CreateDeviceEx(
         adapterId,
         devtype, m_outputWnd[0]->GetNativeHWND(),
         flags /*| D3DCREATE_PUREDEVICE*/,
         &params,
         m_outputWnd[0]->GetWindowMode() == VPX::Window::WindowMode::ExclusiveFullscreen ? &mode : nullptr,
         &m_pD3DDeviceEx);
      if (FAILED(hr))
      {
         if (m_outputWnd[0]->GetWindowMode() == VPX::Window::WindowMode::ExclusiveFullscreen)
         {
            const int result = GetSystemMetrics(SM_REMOTESESSION);
            const bool isRemoteSession = (result != 0);
            if (isRemoteSession)
               ShowError("Try disabling exclusive Fullscreen Mode for Remote Desktop Connections");
         }
         ReportFatalError(hr, __FILE__, __LINE__);
      }

      m_pD3DDeviceEx->QueryInterface(__uuidof(IDirect3DDevice9), reinterpret_cast<void**>(&m_pD3DDevice));

      // Get the display mode so that we can report back the actual refresh rate.
      // Not done anymore as the refresh rate is validated before creation
      // CHECKD3D(m_pD3DDeviceEx->GetDisplayModeEx(0, &mode, nullptr)); //!! what is the actual correct value for the swapchain here?
      // refreshrate = mode.RefreshRate;
   }

   if (maxPrerenderedFrames > 0 && maxPrerenderedFrames <= 20)
   {
      CHECKD3D(m_pD3DDeviceEx->SetMaximumFrameLatency(maxPrerenderedFrames));
   }

   // Retrieve a reference to the back buffer.
   wnd->SetBackBuffer(new RenderTarget(this, SurfaceType::RT_DEFAULT, wnd->GetWidth(), wnd->GetHeight(), back_buffer_format));

   /*if (m_outputWnd[0]->GetWindowMode() == WindowMode::ExclusiveFullscreen)
       hr = m_pD3DDevice->SetDialogBoxMode(TRUE);*/ // needs D3DPRESENTFLAG_LOCKABLE_BACKBUFFER, but makes rendering slower on some systems :/
#endif

   // Create default texture
   {
      std::shared_ptr<BaseTexture> surf = std::shared_ptr<BaseTexture>(BaseTexture::Create(1, 1, BaseTexture::Format::RGBA));
      memset(surf->data(), 0, 4);
      m_nullTexture = std::make_shared<Sampler>(this, "Null"s, surf, false);
   }

   // create default vertex declarations for shaders
   #if defined(ENABLE_BGFX)
   m_pVertexTexelDeclaration = new bgfx::VertexLayout; // TODO remove Pos/TexCoord format and only use one Pos/Normal/TexCoord
   m_pVertexTexelDeclaration->begin()
      .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
      .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
      .end();
   m_pVertexNormalTexelDeclaration = new bgfx::VertexLayout;
   m_pVertexNormalTexelDeclaration->begin()
      .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
      .add(bgfx::Attrib::Normal, 3, bgfx::AttribType::Float)
      .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
      .end();
   #elif defined(ENABLE_DX9)
   CHECKD3D(m_pD3DDevice->CreateVertexDeclaration(VertexTexelElement, &m_pVertexTexelDeclaration));
   CHECKD3D(m_pD3DDevice->CreateVertexDeclaration(VertexNormalTexelElement, &m_pVertexNormalTexelDeclaration));
   #endif

   // Vertex buffers
   static constexpr float verts[4 * 5] =
   {
       1.0f,  1.0f, 0.0f, 1.0f, 0.0f,
      -1.0f,  1.0f, 0.0f, 0.0f, 0.0f,
       1.0f, -1.0f, 0.0f, 1.0f, 1.0f,
      -1.0f, -1.0f, 0.0f, 0.0f, 1.0f
   };
   std::shared_ptr<VertexBuffer> quadVertexBuffer = std::make_shared<VertexBuffer>(this, 4, verts, false, VertexFormat::VF_POS_TEX);
   m_quadMeshBuffer = std::make_shared<MeshBuffer>("Fullscreen Quad"s, quadVertexBuffer);

   #if defined(ENABLE_OPENGL)
   std::shared_ptr<VertexBuffer> quadPNTDynVertexBuffer = std::make_shared<VertexBuffer>(this, 4, nullptr, true, VertexFormat::VF_POS_NORMAL_TEX);
   m_quadPNTDynMeshBuffer = std::make_shared<MeshBuffer>(quadPNTDynVertexBuffer);

   std::shared_ptr<VertexBuffer> quadPTDynVertexBuffer = std::make_shared<VertexBuffer>(this, 4, nullptr, true, VertexFormat::VF_POS_TEX);
   m_quadPTDynMeshBuffer = std::make_shared<MeshBuffer>(quadPTDynVertexBuffer);
   #endif

   // Force applying a defined initial render state
   m_current_renderstate.m_state = (~m_renderstate.m_state) & ((1 << 21) - 1);
   m_current_renderstate.m_depthBias = m_renderstate.m_depthBias - 1.0f;
   ApplyRenderStates();
   
   // Ensure we have a VSync source for frame pacing
   bool hasVSync = false;
   #if !defined(__STANDALONE__) && !defined(ENABLE_BGFX)
      if (m_dwm_enabled)
      {
         PLOGI << "VSync source set to Windows Desktop compositor (DwmFlush)";
         hasVSync = true;
      }
   #endif
   #if defined(ENABLE_DX9)
      else
      {
         PLOGI << "VSync source set to DX9Ex WaitForBlank";
         hasVSync = true;
      }
   #elif defined(ENABLE_OPENGL) && !defined(__STANDALONE__)
      // DXGI VSync source (Windows 7+, only used for Win32 SDL with OpenGL)
      else if (syncMode == VideoSyncMode::VSM_FRAME_PACING)
      {
         DXGIRegistry::Output* out = m_DXGIRegistry.GetForWindow(m_outputWnd[0]->GetNativeHWND());
         if (out != nullptr)
            m_DXGIOutput = out->m_Output;
         if (m_DXGIOutput != nullptr)
         {
            PLOGI << "VSync source set to DXGI WaitForBlank";
            hasVSync = true;
         }
      }
   #elif defined(ENABLE_BGFX)
      // BGFX implements frame pacing by monitoring frames in flight (instead of relying on a VSync source)
      hasVSync = true;
   #endif
   
   if (syncMode == VideoSyncMode::VSM_FRAME_PACING && !hasVSync)
   {
      // This may happen on some old config where DWM is disabled
      ShowError("Failed to create the synchronization device.\r\nSynchronization switched to adaptive sync.");
      PLOGE << "Failed to create the synchronization device for frame pacing. Synchronization switched to adaptive sync.";
      syncMode = VideoSyncMode::VSM_ADAPTIVE_VSYNC;
      #if defined(ENABLE_OPENGL)
      SDL_GL_SetSwapInterval(-1);
      #endif
   }

   m_uiShader = new Shader(this, Shader::UI_SHADER, m_nEyes == 2);
   m_basicShader = new Shader(this, Shader::BASIC_SHADER, m_nEyes == 2);
   m_ballShader = new Shader(this, Shader::BALL_SHADER, m_nEyes == 2);
   m_DMDShader = new Shader(this, Shader::DMD_SHADER, m_nEyes == 2);
   m_flasherShader = new Shader(this, Shader::FLASHER_SHADER, m_nEyes == 2);
   m_lightShader = new Shader(this, Shader::LIGHT_SHADER, m_nEyes == 2);
   m_stereoShader = m_nEyes == 2 ? new Shader(this, Shader::STEREO_SHADER, true) : nullptr;
   m_FBShader = new Shader(this, Shader::POSTPROCESS_SHADER, m_nEyes == 2);

   if ((m_stereoShader != nullptr && m_stereoShader->HasError()) || m_basicShader->HasError() || m_ballShader->HasError() || m_DMDShader->HasError() || m_FBShader->HasError()
      || m_flasherShader->HasError() || m_lightShader->HasError())
   {
      ReportError("Fatal Error: shader compilation failed!"s, -1, __FILE__, __LINE__);
      throw(-1);
   }

   #if defined ENABLE_BGFX
   m_uniformState = std::make_unique<ShaderState>(UseLowPrecision());
   #endif

   // Initialize uniform to default value
   m_basicShader->SetVector(ShaderUniform::staticColor_Alpha, 1.0f, 1.0f, 1.0f, 1.0f); // No tinting
   // FIXME XR
   #ifndef ENABLE_XR
   m_DMDShader->SetFloat(ShaderUniform::alphaTestValue, 1.0f); // No alpha clipping
   #endif

   #if !defined(__OPENGLES__)
      // Always load the (small) SMAA textures since SMAA can be toggled at runtime through the live UI
      UploadAndSetSMAATextures();
   #endif

   m_renderFrame = std::make_unique<RenderFrame>(this);
}

RenderDevice::~RenderDevice()
{
   #if defined(ENABLE_BGFX)
      // Suspend rendering before deleting anything that could be used
      m_renderDeviceAlive = false;
      m_frameReadySem.release();
      // Wait for the render thread to actually leave its render loop: it may still be mid-frame (using the
      // shaders, meshes and textures freed below) since it only re-checks m_renderDeviceAlive between frames
      m_renderThreadStopped.acquire();
   #endif

   m_quadMeshBuffer = nullptr;

   #if defined(ENABLE_DX9)
      m_pD3DDevice->SetStreamSource(0, nullptr, 0, 0);
      m_pD3DDevice->SetIndices(nullptr);
      m_pD3DDevice->SetVertexShader(nullptr);
      m_pD3DDevice->SetPixelShader(nullptr);
      m_pD3DDevice->SetFVF(D3DFVF_XYZ);
      m_pD3DDevice->SetDepthStencilSurface(nullptr);
      SAFE_RELEASE(m_pVertexTexelDeclaration);
      SAFE_RELEASE(m_pVertexNormalTexelDeclaration);
   #endif

   delete m_uiShader;
   m_uiShader = nullptr;
   delete m_basicShader;
   m_basicShader = nullptr;
   delete m_DMDShader;
   m_DMDShader = nullptr;
   delete m_FBShader;
   m_FBShader = nullptr;
   delete m_stereoShader;
   m_stereoShader = nullptr;
   delete m_flasherShader;
   m_flasherShader = nullptr;
   delete m_lightShader;
   m_lightShader = nullptr;
   delete m_ballShader;
   m_ballShader = nullptr;

   m_nullTexture = nullptr;
   m_SMAAareaTexture = nullptr;
   m_SMAAsearchTexture = nullptr;
   m_texMan.UnloadAll();
   #if defined(ENABLE_BGFX)
      // Samplers still queued here own BGFX textures: release them before BGFX is shut down
      m_pendingTextureUploads.clear();
   #endif

   m_renderFrame = nullptr;

   for (auto wnd : m_outputWnd)
   {
      delete wnd->GetBackBuffer();
      wnd->SetBackBuffer(nullptr);
   }

   // Delete preview window we eventually created in constructor
   if (g_pplayer->IsVR() && m_outputWnd.size() > 1)
      delete m_outputWnd[1];


#if defined(ENABLE_BGFX)
   delete m_pVertexTexelDeclaration;
   delete m_pVertexNormalTexelDeclaration;

   if (bgfx::isValid(m_srgbMipmapProgram))
      bgfx::destroy(m_srgbMipmapProgram);

   // Shutdown BGFX once all native resources have been cleaned up
   m_rendererInitialized.release();
   while (!m_renderDeviceAlive)
   {
      g_pplayer->ProcessOSMessages(false);
      Sleep(0);
   }
   // Order matters: the API thread is the one that calls bgfx::shutdown(), and the render thread
   // only leaves its loop once that shutdown reaches it. Joining the render thread first would wait
   // for an exit that has not been posted yet.
   if (m_renderThread.joinable())
      m_renderThread.join();
   if (m_bgfxRenderThread.joinable())
      m_bgfxRenderThread.join();

#elif defined(ENABLE_OPENGL)
   m_quadPNTDynMeshBuffer = nullptr;
   m_quadPTDynMeshBuffer = nullptr;

   for (auto binding : m_samplerBindings)
   {
      std::shared_ptr<const Sampler> sampler = binding->sampler;
      if (sampler)
         const_cast<Sampler*>(sampler.get())->Unbind();
      delete binding;
   }
   m_samplerBindings.clear();

   for (size_t i = 0; i < std::size(m_samplerStateCache); i++)
   {
      if (m_samplerStateCache[i] != 0)
      {
         glDeleteSamplers(1, &m_samplerStateCache[i]);
         m_samplerStateCache[i] = 0;
      }
   }
   
   SDL_GL_DestroyContext(m_sdl_context);

   assert(m_sharedVAOs.empty());

#elif defined(ENABLE_DX9)
   // Check for resource leak on debug builds
   #ifdef _DEBUG
   IDirect3DSwapChain9* swapChain;
   CHECKD3D(m_pD3DDevice->GetSwapChain(0, &swapChain));

   D3DPRESENT_PARAMETERS pp;
   CHECKD3D(swapChain->GetPresentParameters(&pp));
   SAFE_RELEASE(swapChain);
   pp.SwapEffect = D3DSWAPEFFECT_DISCARD;

   // idea: device can't be reset if there are still allocated resources
   HRESULT hr = m_pD3DDevice->Reset(&pp);
   if (FAILED(hr))
   {
      ShowError("WARNING! Direct3D resource leak detected!");
   }
   #endif

   SAFE_RELEASE_NO_RCC(m_pD3DDeviceEx);
   #ifdef DEBUG_REFCOUNT_TRIGGER
   SAFE_RELEASE(m_pD3DDevice);
   #else
   FORCE_RELEASE(m_pD3DDevice); //!! why is this necessary for some setups? is the refcount still off for some settings?
   #endif

   #ifndef DISABLE_FORCE_NVIDIA_OPTIMUS
   if (NVAPIinit) //!! meh
      CHECKNVAPI(NvAPI_Unload());
   NVAPIinit = false;
   #endif

   SAFE_RELEASE_NO_RCC(m_pD3DEx);
   #ifdef DEBUG_REFCOUNT_TRIGGER
   SAFE_RELEASE(m_pD3D);
   #else
   FORCE_RELEASE(m_pD3D); //!! why is this necessary for some setups? is the refcount still off for some settings?
   #endif

   /*
    * D3D sets the FPU to single precision/round to nearest int mode when it's initialized,
    * but doesn't bother to reset the FPU when it's destroyed. We reset it manually here.
    */
   _fpreset();
#endif

   assert(m_pendingSharedIndexBuffers.empty());
   assert(m_pendingSharedVertexBuffers.empty());
}

void RenderDevice::AddWindow(VPX::Window* wnd)
{
   assert(wnd->GetBackBuffer() == nullptr);

#if defined(ENABLE_BGFX)
   if ((bgfx::getCaps()->supported & BGFX_CAPS_SWAP_CHAIN) == 0)
      return;
   bgfx::TextureFormat::Enum bgfxFormat = SelectBackBufferFormat(wnd, bgfx::TextureFormat::Count, false);
   colorFormat vpxFormat = BGFXtoVPXTextureFormat(bgfxFormat);
   PLOGD << "Creating BGFX swap chain for window " << SDL_GetWindowTitle(wnd->GetCore()) << " (" << wnd->GetPixelWidth() << 'x' << wnd->GetPixelHeight() << " "
         << bimg::getName(bimg::TextureFormat::Enum(bgfxFormat)) << ')';
   SDL_Window* sdlWnd = wnd->GetCore();
   // VPINBALL/4kp: initialised, because the platform block below does not assign it for every video
   // driver. On KMSDRM neither the x11 nor the wayland branch runs, so this used to reach
   // bgfx::createFrameBuffer() as an INDETERMINATE pointer and crash as soon as a second output
   // window (backglass / scoreview) was enabled.
   void* nwh = nullptr;
#if BX_PLATFORM_LINUX || BX_PLATFORM_BSD
   void* ndt = nullptr;
   (void)ndt; // assigned per-driver below but unused here; BGFX takes only the native window
   if (SDL_GetCurrentVideoDriver() == "x11"sv) {
      ndt = SDL_GetPointerProperty(SDL_GetWindowProperties(sdlWnd), SDL_PROP_WINDOW_X11_DISPLAY_POINTER, NULL);
      nwh = (void*)SDL_GetNumberProperty(SDL_GetWindowProperties(sdlWnd), SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
   }
   else if (SDL_GetCurrentVideoDriver() == "wayland"sv) {
      ndt = SDL_GetPointerProperty(SDL_GetWindowProperties(sdlWnd), SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, NULL);
      nwh = SDL_GetPointerProperty(SDL_GetWindowProperties(sdlWnd), SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, NULL);
   }
   else if (SDL_GetCurrentVideoDriver() == "kmsdrm"sv) {
      // Each KMSDRM window owns its own gbm_surface; that is the native window BGFX renders into,
      // exactly as for the playfield swapchain (see the kmsdrm branch in RenderDevice()).
      nwh = SDL_GetPointerProperty(SDL_GetWindowProperties(sdlWnd), "SDL.window.kmsdrm.gbm_surface", NULL);
   }
#elif BX_PLATFORM_OSX
   {
      SDL_Renderer* renderer = SDL_GetRenderer(sdlWnd);
      if (renderer == nullptr)
         renderer = SDL_CreateRenderer(sdlWnd, "Metal");
      nwh = SDL_GetRenderMetalLayer(renderer);
   }
#elif BX_PLATFORM_IOS
   nwh = VPinballLib::VPinballLib::Instance().GetMetalLayer();
#elif BX_PLATFORM_ANDROID
   nwh = SDL_GetPointerProperty(SDL_GetWindowProperties(sdlWnd), SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, NULL);
#elif BX_PLATFORM_WINDOWS
   nwh = wnd->GetNativeHWND();
#elif BX_PLATFORM_STEAMLINK
   nwh = wmInfo.info.vivante.window;
#else
   return;
#endif // BX_PLATFORM_
   if (nwh == nullptr)
   {
      PLOGE << "No native window handle for '" << SDL_GetWindowTitle(sdlWnd) << "' on video driver '" << SDL_GetCurrentVideoDriver()
            << "'; skipping its swap chain rather than handing BGFX an invalid handle.";
      return;
   }
   bgfx::FrameBufferHandle fbh = bgfx::createFrameBuffer(nwh, uint16_t(wnd->GetPixelWidth()), uint16_t(wnd->GetPixelHeight()), bgfxFormat);
   m_outputWnd.push_back(wnd);
   wnd->SetBackBuffer(new RenderTarget(this, SurfaceType::RT_DEFAULT, fbh, BGFX_INVALID_HANDLE, bgfxFormat, BGFX_INVALID_HANDLE, bgfx::TextureFormat::Count,
      "BackBuffer #" + std::to_string(m_outputWnd.size()), wnd->GetPixelWidth(), wnd->GetPixelHeight(), vpxFormat));
#endif
}

void RenderDevice::RemoveWindow(VPX::Window* wnd)
{
   std::erase(m_outputWnd, wnd);
}

bool RenderDevice::DepthBufferReadBackAvailable() const
{
#if defined(ENABLE_OPENGL) || defined(ENABLE_BGFX)
   return true;
#elif defined(ENABLE_DX9)
   if (m_INTZ_support && !m_useNvidiaApi)
      return true;
   // fall back to NVIDIAs NVAPI, only handle DepthBuffer ReadBack if API was initialized
   return NVAPIinit;
#endif
}

void RenderDevice::CaptureScreenshot(const vector<VPX::Window*>& wnd, const vector<std::filesystem::path>& filename, const std::function<void(bool)>& callback, int frameDelay)
{
   assert(frameDelay >= 1);
   {
      // The render thread concurrently reads this state in its screenshot request loop and BGFX callback
      std::lock_guard lock(m_screenshotMutex);
      if (m_screenshotFilename.empty())
      {
         m_screenshotSuccess = true;
         m_screenshotWindow = wnd;
         m_screenshotFilename = filename;
         m_screenshotCallback = callback;
         m_screenshotFrameDelay = frameDelay;
         return;
      }
   }
   // Fire outside the lock as the callback may re-enter CaptureScreenshot or take other locks
   PLOGE << "Screenshot capture already in progress.";
   callback(false);
}

float RenderDevice::GetVisualLatency() const
{
   // FIXME implement for VR using OpenXR predicted display time
   if (g_pplayer->m_vrDevice)
   {
      return 0.f;
   }

   // Visual latency is the sum of these 3 estimates:
   // - finger to frame preparation latency => average estimate as half of the frame time (since the input is not synced to the frame, it can happen at any time during the frame, so on average at mid frame
   // - render latency (frame preparation to frame presentation) => use BGFX estimate or estimate based on sync strategy (note that both ways are somewhat imprecise)
   // - display latency (frame presentation to display) => varies a lot between displays, from just a few ms on high end gaming monitor to ~15ms on TV with gaming mode (and even more on cheaper TV or without gaming mode)
   float delay = 0.5f / g_pplayer->GetTargetRefreshRate();
#ifdef ENABLE_BGFX
   if (m_renderLatency > 0.f)
      delay += m_renderLatency;
   else
      delay += 2.f / g_pplayer->GetTargetRefreshRate();
#else
   if (g_pplayer->GetVideoSyncMode() == VideoSyncMode::VSM_VSYNC || g_pplayer->GetVideoSyncMode() == VideoSyncMode::VSM_ADAPTIVE_VSYNC)
      delay += 5.f / g_pplayer->GetTargetRefreshRate();
   else
      delay += 2.f / g_pplayer->GetTargetRefreshRate();
#endif
   delay += 0.005f; // basic display latency estimate
   return delay;
}

unsigned int RenderDevice::GetTargetFrameLength() const
{
   const VideoSyncMode syncMode = g_pplayer->GetVideoSyncMode();
   if (syncMode == VideoSyncMode::VSM_FRAME_PACING)
   {
      // Frame pacing targets the display refresh rate
      return static_cast<unsigned int>(1000000. / (double)m_outputWnd[0]->GetRefreshRate());
   }
   else if (syncMode == VideoSyncMode::VSM_VSYNC || syncMode == VideoSyncMode::VSM_ADAPTIVE_VSYNC)
   {
      if (g_pplayer->GetTargetRefreshRate() < m_outputWnd[0]->GetRefreshRate())
      {
         // The user has enabled VSync with a max FPS below the display FPS
         return static_cast<unsigned int>(1000000. / (double)g_pplayer->GetTargetRefreshRate());
      }
      else
      {
         // The user has enabled VSync with a max FPS above the display FPS => target is the display FPS
         return static_cast<unsigned int>(1000000. / (double)m_outputWnd[0]->GetRefreshRate());
      }
   }
   else if (g_pplayer->GetTargetRefreshRate() < 10000.f)
   {
      // The user has disabled VSync with a custom target FPS
      return static_cast<unsigned int>(1000000. / (double)g_pplayer->GetTargetRefreshRate());
   }
   else
   {
      // Unbound target FPS without any synchronization (so aiming at the slowest possible frame time)
      return 0;
   }
}

float RenderDevice::GetPredictedDisplayDelay() const
{
   const uint64_t now = usec();
   if (g_pplayer->m_vrDevice)
   {
      // Use OpenXR display time prediction
      const float nowS = (float)((double)now / 1000000.);
      const float displayTimestamp = g_pplayer->m_vrDevice->GetPredictedDisplayTimestamp();
      return nowS < displayTimestamp ? displayTimestamp - nowS : 0.f;
   }
   else if (const uint64_t targetFrameLength = GetTargetFrameLength(); targetFrameLength == 0)
   {
      // No synchronization (run as fast as possible), just disable predicted time correction
      return 0.f;
   }
   else
   {
      // We evaluate the next frame presentation as the delay to next displayed frame (from a fixed reference) + an integral number of GPU queue frames
      uint64_t delayToNextFrame = targetFrameLength - ((now - m_presentTimestampReference) % targetFrameLength);
      #ifdef ENABLE_BGFX
      if (delayToNextFrame < m_lastGPUFrameLength)
         delayToNextFrame += targetFrameLength;
      #else
      if (delayToNextFrame < g_pplayer->m_renderProfiler->GetAvg(FrameProfiler::ProfileSection::PROFILE_RENDER_SUBMIT))
         delayToNextFrame += targetFrameLength;
      #endif
      if (g_pplayer->GetVideoSyncMode() != VideoSyncMode::VSM_FRAME_PACING && g_pplayer->m_ptable->m_settings.GetPlayer_MaxPrerenderedFrames() > 1)
      {
         const uint64_t displayFrameLength = static_cast<uint64_t>(1000000. / (double)m_outputWnd[0]->GetRefreshRate());
         delayToNextFrame += (g_pplayer->m_ptable->m_settings.GetPlayer_MaxPrerenderedFrames() - 1) * displayFrameLength;
      }
      // PLOGI << std::format("Display Delay: {:5.3f}ms / Now: {:5.3f}ms / VSync: {:5.3f}ms", delayToNextFrame / 1000., now / 1000., m_presentTimestampReference / 1000.);
      return static_cast<float>(static_cast<double>(delayToNextFrame) / 1000000.);
   }
}

void RenderDevice::WaitForVSync(const bool asynchronous)
{
   // - DWM can be either on or off for Windows Vista/7, it is always enabled for Windows 8+ except on stripped down versions of Windows like Ghost Spectre
   // - Windows XP does not offer any way to sync beside the present parameter on device creation, so this is enforced there and the vsync parameter will be ignored here
   //   (note that the present parameter does not directly sync: it schedules the flip on vsync, leading the GPU to block on another render call, since no backbuffer is available for drawing then)
   auto lambda = [this]()
   {
#ifndef __STANDALONE__
      #if !defined(ENABLE_BGFX)
      if (m_dwm_enabled)
         DwmFlush(); // Flush all commands submitted by this process including the 'Present' command. This actually syncs to the vertical blank
      #endif
      #if defined(ENABLE_OPENGL)
      else if (m_DXGIOutput != nullptr)
         m_DXGIOutput->WaitForVBlank();
      #elif defined(ENABLE_DX9)
      // When DWM is disabled (Windows Vista/7), exclusive fullscreen without DWM (pre-windows 10), special Windows builds with DWM stripped out (Ghost Spectre Windows 10)
      else
         m_pD3DDeviceEx->WaitForVBlank(0);
      #endif
#endif
      m_vsyncCount++;
      m_presentTimestampReference = usec();
   };
   if (asynchronous)
      std::thread(lambda).detach(); // Reuse thread ? (we always at most one running at a time)
   else
      lambda();
}

#if defined(ENABLE_BGFX)
void RenderDevice::NextView()
{
   if (m_activeViewId == bgfx::getCaps()->limits.maxViews - 1)
   {
      PLOGE << "Frame submitted and flipped since BGFX view limit was reached. [BGFX was compiled with a maximum of " << bgfx::getCaps()->limits.maxViews << " views]";
      SubmitRenderFrame();
      bgfx::frame(BGFX_FRAME_FLUSH);
      ResetActiveView();
   }
   m_activeViewId++;
   bgfx::resetView(m_activeViewId);
   bgfx::setViewMode(m_activeViewId, bgfx::ViewMode::Sequential);
   bgfx::setViewClear(m_activeViewId, BGFX_CLEAR_NONE);
   bgfx::touch(m_activeViewId);
}

void RenderDevice::ResetActiveView()
{
   RenderTarget::OnFrameFlushed();
   m_activeViewId = 1; // view 0 & 1 are reserved for mipmap generation (so 1 is before the first available for rendering)
}

#ifdef __RK3588__
uint64_t RenderDevice::s_preSubmitUs = 0;
uint64_t RenderDevice::s_uploadUs = 0;
uint64_t RenderDevice::s_rtRenderUs = 0;
uint64_t RenderDevice::s_rtPresentUs = 0;
uint64_t RenderDevice::s_rtFrames = 0;
uint64_t RenderDevice::s_drainWaitUs = 0;
uint64_t RenderDevice::s_auxBusySkips = 0;
uint64_t RenderDevice::s_boundaryPumpUs = 0;
uint32_t RenderDevice::s_pfCommits = 0;
uint32_t RenderDevice::s_pfQueueEmpty = 0;
uint32_t RenderDevice::s_pfLastFbId = 0;
uint32_t RenderDevice::s_pfFbIdsSeen = 0;
uint32_t RenderDevice::s_glErrors = 0;
bool RenderDevice::s_dynBufferShadow = false;
uint32_t RenderDevice::s_frameIndex = 0;
uint64_t RenderDevice::s_bgfxFrameUs = 0;
uint32_t RenderDevice::s_dynVbUpdates = 0;
uint32_t RenderDevice::s_dynIbUpdates = 0;
uint64_t RenderDevice::s_dynVbBytes = 0;
uint64_t RenderDevice::s_dynIbBytes = 0;
#endif

void RenderDevice::SubmitAndFlipFrame(bool present)
{
   #ifdef __RK3588__
   const uint64_t tSubmitBegin = usec();
   s_preSubmitUs = (m_loopTopUs != 0 && tSubmitBegin > m_loopTopUs) ? tSubmitBegin - m_loopTopUs : 0;
   // Owned scanout bookkeeping: passes for this frame are encoded by now, so the slot they
   // rendered into can be handed to the present thread and the windows rotated onto their next
   // buffers before bgfx::frame() kicks the render.
   if (present)
      UpdateOwnedScanout();
   // Keys the per-frame transient snapshots of the dynamic-buffer shadows; bump after encoding so
   // the next frame's draws take fresh copies.
   ++s_frameIndex;
   #endif
   // Process pending texture upload/mipmap generation before flipping the frame
   for (auto it = m_pendingTextureUploads.cbegin(); it != m_pendingTextureUploads.cend();)
   {
      (*it)->GetCoreTexture(true);
      if (!(*it)->IsUploadPending())
      {
         it = m_pendingTextureUploads.erase(it);
      }
      else
      {
         ++it;
      }
   }
   #ifdef __RK3588__
   // The texture upload loop above sits inside the measured submit phase but outside the window
   // bgfx reports as render thread time, so it has to be timed separately to be accounted for.
   const uint64_t tUploads = usec();
   // ST mode: the GL context is current on this thread (bgfx executed last frame's render here).
   // Pump the reclamation boundary now, before this frame's GL work is issued, so its swap only
   // waits out the previous frame's residue. See PumpBoundarySurface.
   if (present && !m_bgfxMultithreaded)
      PumpBoundarySurface();
   #endif
   const uint32_t frameIdx = bgfx::frame(present ? BGFX_FRAME_NONE : BGFX_FRAME_FLUSH);
   #ifdef __RK3588__
   s_uploadUs = tUploads - tSubmitBegin;
   s_bgfxFrameUs = usec() - tUploads;
   #endif
   if (present)
      m_lastPresentFrameIdx = frameIdx;
   ResetActiveView();
}
#endif

// Schedule frame presentation (usually by flipping the front & back buffer)
#if defined(ENABLE_BGFX) && defined(__RK3588__)
// Upstream never presents on KMSDRM: SDL owns the gbm_surface, BGFX renders into it, and nothing
// scans the front buffer out (see standalone/KmsBgfxPresenter.h). Drive one presenter per output
// window from the render thread, right after BGFX has swapped, so the front buffer exists and the
// EGL context is current for minting the IN_FENCE_FD.
// Frame pacing phase 2, step 2a: hand the slot textures to BGFX and build a framebuffer over each.
//
// This is the step with the real unknown in it. bgfx::overrideInternal swaps a bgfx texture's
// backing for an externally created GL texture, but nothing promises it will accept one that is
// EGLImage-backed rather than allocated by the driver in the usual way, nor that a framebuffer built
// over the result will be valid. Find that out here, where the answer costs a log line -- the render
// path is not redirected yet, so a failure changes nothing.
//
// Verified rather than assumed: getInternal() must hand back the same GL id we supplied, since
// overrideInternal returning silently without taking effect would otherwise look identical to
// success right up until the display showed the wrong buffer.
// Defined by our bgfx patch (vpx-patches/bgfx-skip-present.patch).
extern "C" void bgfx_set_skip_present(bool skip);

void RenderDevice::DisableOwnedScanout(const char* why)  // NOLINT
{
   PLOGE << "[4kpDebug][owned_scanout] " << why << "; falling back to the EGL surface path";
   bgfx_set_skip_present(false);
   m_ownedScanoutPresentSkipped = false;
   for (size_t i = 0; i < m_ownedScanout.size() && i < m_outputWnd.size(); ++i)
   {
      if (m_ownedScanout[i].active.load(std::memory_order_relaxed) && m_ownedScanout[i].originalBackBuffer != nullptr)
         m_outputWnd[i]->SetBackBuffer(m_ownedScanout[i].originalBackBuffer, false);
      m_ownedScanout[i].active.store(false, std::memory_order_release);
      m_ownedScanout[i].disableRequested.store(false, std::memory_order_relaxed);
      m_ownedScanout[i].bindStep = 3; // do not try again this session
   }
}

// Give one window's scanout buffers to BGFX and point that window's back buffer at them.
//
// Two frames: bgfx defers texture creation to the next frame(), so overriding in the same frame is
// dropped (returns 0) while the framebuffer still reports valid -- wrapping bgfx's own allocation.
void RenderDevice::BindOwnedScanoutToBgfx(const size_t idx, VPX::Window* wnd)
{
   OwnedScanout& own = m_ownedScanout[idx];
   const VPX::Kms::ScanoutSlots* const slotsPtr = own.slots.load(std::memory_order_acquire);
   if (own.bindStep > 1 || slotsPtr == nullptr)
      return;
   const VPX::Kms::ScanoutSlots& slots = *slotsPtr;
   if (!slots.IsReady())
      return;

   const uint16_t w = uint16_t(wnd->GetPixelWidth());
   const uint16_t h = uint16_t(wnd->GetPixelHeight());

   if (own.bindStep == 0)
   {
      for (int i = 0; i < slots.Count(); ++i)
      {
         own.tex[i] = bgfx::createTexture2D(w, h, false, 1, bgfx::TextureFormat::BGRA8, BGFX_TEXTURE_RT);
         if (!bgfx::isValid(own.tex[i]))
         {
            PLOGE.printf("[4kpDebug][owned_scanout] window %zu slot %d: createTexture2D failed", idx, i);
            own.bindStep = 2;
            return;
         }
      }
      own.stagingTex = bgfx::createTexture2D(w, h, false, 1, bgfx::TextureFormat::BGRA8, BGFX_TEXTURE_RT);
      if (!bgfx::isValid(own.stagingTex))
      {
         PLOGE.printf("[4kpDebug][owned_scanout] window %zu: staging createTexture2D failed", idx);
         own.bindStep = 2;
         return;
      }
      own.bindStep = 1;
      return;
   }

   // Overrides first, framebuffers only once every override has taken. Under BGFX multithreading
   // the createTexture2D from the previous step may not have been processed by the render thread
   // yet when this runs (its commands render only after the frame that recorded them is kicked),
   // in which case overrideInternal is silently dropped -- seen live as window 0 failing while
   // windows whose create happened seconds earlier bound fine. overrideInternal is idempotent for
   // an already-taken handle, so retrying across frames is safe.
   bool allTook = true;
   for (int i = 0; i < slots.Count(); ++i)
   {
      const VPX::Kms::ScanoutSlots::Slot& slot = slots.GetSlot(i);
      const uintptr_t bound = bgfx::overrideInternal(own.tex[i], uintptr_t(slot.texture));
      if (bound != uintptr_t(slot.texture))
         allTook = false;
   }
   // The staging texture is ScanoutSlots' ordinary (non-imported) GL texture: bgfx renders the
   // frame into it, and the present thread glBlitFramebuffers it into the imported slot before
   // each commit -- the 10.8.0 KmsGbmProducer data path.
   if (bgfx::overrideInternal(own.stagingTex, uintptr_t(slots.GetStagingTexture())) != uintptr_t(slots.GetStagingTexture()))
      allTook = false;
   if (!allTook)
   {
      if (++own.bindAttempts < 16)
         return; // creation not processed yet; retry next presented frame
      own.bindStep = 2;
      PLOGE.printf("[4kpDebug][owned_scanout] window %zu: override never took after %d attempts; staying on the EGL surface path", idx, own.bindAttempts);
      return;
   }

   own.bindStep = 2;
   bool allValid = true;

   for (int i = 0; i < slots.Count(); ++i)
   {
      const VPX::Kms::ScanoutSlots::Slot& slot = slots.GetSlot(i);
      // Only the playfield composites with depth; the ancillary panels are 2D and their existing
      // back buffers carry no depth attachment either.
      if (idx == 0)
      {
         const bgfx::TextureHandle depth = bgfx::createTexture2D(w, h, false, 1,
            bgfx::TextureFormat::D24S8, BGFX_TEXTURE_RT_WRITE_ONLY);
         if (bgfx::isValid(depth))
         {
            bgfx::TextureHandle attachments[2] = { own.tex[i], depth };
            own.fb[i] = bgfx::createFrameBuffer(2, attachments, false);
         }
      }
      else
      {
         own.fb[i] = bgfx::createFrameBuffer(1, &own.tex[i], false);
      }

      if (!bgfx::isValid(own.fb[i]))
         allValid = false;

      PLOGI.printf("[4kpDebug][owned_scanout] window %zu slot %d: gl texture %u -> bgfx texture %u, override MATCH (attempt %d), framebuffer %s",
         idx, i, slot.texture, own.tex[i].idx, own.bindAttempts + 1,
         bgfx::isValid(own.fb[i]) ? "valid" : "INVALID");
   }

   if (!allValid)
   {
      PLOGE.printf("[4kpDebug][owned_scanout] window %zu: a slot was unusable; staying on the EGL surface path", idx);
      return;
   }

   for (int i = 0; i < slots.Count(); ++i)
      own.rt[i] = new RenderTarget(this, SurfaceType::RT_DEFAULT, own.fb[i], own.tex[i], bgfx::TextureFormat::BGRA8,
         BGFX_INVALID_HANDLE, idx == 0 ? bgfx::TextureFormat::D24S8 : bgfx::TextureFormat::Count,
         "OwnedScanout" + std::to_string(idx) + '.' + std::to_string(i), wnd->GetPixelWidth(), wnd->GetPixelHeight(),
         BGFXtoVPXTextureFormat(bgfx::TextureFormat::BGRA8));

   // The staging indirection is the 10.8.0 fork's proven shape (KmsGbmProducer: content enters the
   // imported buffer through exactly one glBlitFramebuffer, never as a render target for scene
   // passes and never via glCopyImageSubData -- bgfx::blit into the sibling produced black panels
   // on this blob). own.stagingTex was overridden above to ScanoutSlots' ordinary GL texture, so
   // the present thread can source its blit from the matching GL FBO.
   {
      bgfx::FrameBufferHandle stagingFb = BGFX_INVALID_HANDLE;
      if (idx == 0)
      {
         const bgfx::TextureHandle stagingDepth = bgfx::createTexture2D(w, h, false, 1, bgfx::TextureFormat::D24S8, BGFX_TEXTURE_RT_WRITE_ONLY);
         if (bgfx::isValid(stagingDepth))
         {
            bgfx::TextureHandle attachments[2] = { own.stagingTex, stagingDepth };
            stagingFb = bgfx::createFrameBuffer(2, attachments, false);
         }
      }
      else
         stagingFb = bgfx::createFrameBuffer(1, &own.stagingTex, false);
      if (!bgfx::isValid(stagingFb))
      {
         PLOGE.printf("[4kpDebug][owned_scanout] window %zu: staging target creation failed; staying on the EGL surface path", idx);
         return;
      }
      own.staging = new RenderTarget(this, SurfaceType::RT_DEFAULT, stagingFb, own.stagingTex, bgfx::TextureFormat::BGRA8,
         BGFX_INVALID_HANDLE, idx == 0 ? bgfx::TextureFormat::D24S8 : bgfx::TextureFormat::Count,
         "ScanoutStaging" + std::to_string(idx), wnd->GetPixelWidth(), wnd->GetPixelHeight(),
         BGFXtoVPXTextureFormat(bgfx::TextureFormat::BGRA8));
   }

   own.originalBackBuffer = wnd->GetBackBuffer();
   own.slot = 0;
   own.staging->RequestClear();
   wnd->SetBackBuffer(own.staging, false);
   own.active.store(true, std::memory_order_release);
   PLOGI.printf("[4kpDebug][owned_scanout] window %zu redirected via staging blit; cycling %d owned buffers", idx, slots.Count());
}

// bgfx API thread, once per frame after pass encoding and before bgfx::frame(): execute any
// fallback the present thread requested, drive the two-step bind, hand rendered slots to the
// present thread, and rotate each owned window onto its next buffer for the frame about to be
// built. All bgfx object creation and back-buffer swapping lives here because the present thread
// runs concurrently with pass encoding for the NEXT frame.
void RenderDevice::UpdateOwnedScanout()
{
   for (size_t i = 0; i < m_outputWnd.size() && i < m_ownedScanout.size(); ++i)
      if (m_ownedScanout[i].disableRequested.load(std::memory_order_acquire))
      {
         DisableOwnedScanout("present thread reported a commit failure");
         return;
      }

   if (!g_pplayer || !g_pplayer->m_ptable || !g_pplayer->m_ptable->m_settings.GetStandalone_4kpOwnedScanout())
      return;
   const bool playfieldOnly = g_pplayer->m_ptable->m_settings.GetStandalone_4kpOwnedScanoutPlayfieldOnly();

   for (size_t i = 0; i < m_outputWnd.size() && i < m_ownedScanout.size(); ++i)
   {
      if (playfieldOnly && i != 0)
         continue; // ancillary windows stay on the swap path (experiment 1, see the setting)
      OwnedScanout& own = m_ownedScanout[i];
      if (!own.active.load(std::memory_order_relaxed))
      {
         BindOwnedScanoutToBgfx(i, m_outputWnd[i]);
         continue;
      }
      // Override integrity: re-assert every slot's texture override each frame. overrideInternal
      // is idempotent when the override is intact, and if bgfx ever reverted to its own allocation
      // (which would leave it rendering off-screen while we commit stale gbm buffers -- no GL
      // error, frozen panel) this both detects and heals it. API thread, as required.
      {
         const VPX::Kms::ScanoutSlots* const slotsPtr = own.slots.load(std::memory_order_relaxed);
         if (slotsPtr != nullptr)
            for (int s = 0; s < slotsPtr->Count(); ++s)
            {
               const uintptr_t want = uintptr_t(slotsPtr->GetSlot(s).texture);
               const uintptr_t got = bgfx::overrideInternal(own.tex[s], want);
               if (got != want)
               {
                  static uint32_t s_reverts = 0;
                  if (++s_reverts <= 5)
                     PLOGE.printf("[4kpDebug][owned_scanout] window %zu slot %d override DIVERGED (got %p want %p); re-asserted",
                        i, s, (void*)got, (void*)want);
               }
            }
      }
      // The pending-clear flag is armed when the frame's staging content is handed off and
      // consumed by the first pass that targets the staging buffer, so a still-armed flag means
      // nothing rendered this frame (AncillaryFrameDivider skipped the window): hand nothing to
      // the present thread, and it keeps scanning the last committed buffer.
      RenderTarget* const cur = own.staging;
      if (cur == nullptr || cur->m_pendingClear)
         continue;
      const uint32_t push = own.slotQPush.load(std::memory_order_relaxed);
      if (push - own.slotQPop.load(std::memory_order_acquire) >= sizeof(own.slotQ))
         continue; // present thread has stalled; do not wrap the ring
      // The staging -> slot copy happens on the present thread (ScanoutSlots::BlitStagingToSlot),
      // AFTER bgfx has executed this frame's GL and right before the commit. Only the slot index
      // is handed over here; the slot still rotates so the display never latches mid-copy.
      own.slotQ[push % sizeof(own.slotQ)] = uint8_t(own.slot);
      own.slotQPush.store(push + 1, std::memory_order_release);
      own.slot = (own.slot + 1) % VPX::Kms::kScanoutSlotCount;
      // Re-arm: consumed by next frame's first staging pass, read back here as the "did this
      // window render" signal.
      cur->RequestClear();
   }

   // In full mode, presentation is skipped once EVERY window is owned: eglSwapBuffers on any
   // surface drains the whole context, so one window left on the EGL path re-serialises all of
   // them. In playfield-only mode (experiment 1) only the primary swap is skipped (the -012 bgfx
   // patch never skips window swap chains), so the skip engages as soon as the playfield is owned
   // -- the ancillary swaps keep their measured wait, which is the price of the experiment.
   if (!m_ownedScanoutPresentSkipped && !m_outputWnd.empty())
   {
      size_t active = 0;
      const size_t needed = playfieldOnly ? 1 : m_outputWnd.size();
      for (size_t i = 0; i < m_outputWnd.size() && i < m_ownedScanout.size(); ++i)
         if (m_ownedScanout[i].active.load(std::memory_order_relaxed))
            ++active;
      if (active >= needed)
      {
         m_ownedScanoutPresentSkipped = true;
         bgfx_set_skip_present(true);
         PLOGI.printf("[4kpDebug][owned_scanout] %zu of %zu windows owned (%s); primary BGFX present skipped",
            active, m_outputWnd.size(), playfieldOnly ? "playfield-only mode" : "all windows");
         // Frozen-playfield hunt: dump the pass graph (submitted + sorted, with dependencies) for
         // the first owned frame, so a pruned playfield chain names itself in the log.
         LogNextFrame();
      }
   }
}

bool RenderDevice::IsScanoutRenderTarget(const RenderTarget* const rt) const
{
   if (rt == nullptr)
      return false;
   for (const OwnedScanout& own : m_ownedScanout)
      if (own.active.load(std::memory_order_acquire))
      {
         if (own.staging == rt)
            return true; // all scanout-bound passes render (flipped) into the staging target
         for (const RenderTarget* const slotRt : own.rt)
            if (slotRt == rt)
               return true;
      }
   return false;
}

bool RenderDevice::IsCurrentPassScanout() const
{
   return m_currentPass != nullptr && IsScanoutRenderTarget(m_currentPass->m_rt);
}

static std::unordered_map<SDL_Window*, VPX::Kms::WindowPresenter> s_presenters;

void RenderDevice::PresentKmsWindows()
{
   // With BGFX presentation skipped there is no eglSwapBuffers, and eglSwapBuffers was the only
   // per-frame flush: without one the GPU may not start this frame's work until CommitFb's fence
   // flush -- which sits AFTER the playfield drain, serialising GPU start behind the previous
   // frame's latch. Flush here, at exactly the point the swap used to, so the GPU overlaps the
   // drain wait.
   if (m_ownedScanoutPresentSkipped)
   {
      static void (*s_glFlush)() = reinterpret_cast<void (*)()>(eglGetProcAddress("glFlush"));
      if (s_glFlush != nullptr)
         s_glFlush();
      // Shadow-sync diagnostic (mixed-mode frozen playfield): the working theory is that libmali
      // shadows EGLImage-sibling render targets and only syncs the private backing to the dmabuf
      // during its slow-path frame finalization -- which fast-path swaps (mixed mode) never run.
      // A full glFinish forces completion of everything; if the panel comes alive with this on,
      // the theory is proven and the fix becomes finding a cheaper finalization kick.
      static int s_scanoutFinish = -1;
      if (s_scanoutFinish < 0)
         s_scanoutFinish = (g_pplayer && g_pplayer->m_ptable
            && g_pplayer->m_ptable->m_settings.GetStandalone_4kpScanoutFinish()) ? 1 : 0;
      if (s_scanoutFinish != 0)
      {
         static void (*s_glFinish)() = reinterpret_cast<void (*)()>(eglGetProcAddress("glFinish"));
         if (s_glFinish != nullptr)
         {
            const uint64_t tFin = usec();
            s_glFinish();
            s_boundaryPumpUs += usec() - tFin; // reuse the pump counter so the cost shows in the stats split
         }
      }
      // Stage probe (rate-limited): read the scene texture back to see whether the dynamic content
      // (ball, flashers) is ever rendered into it -- the one measurement that splits "scene draws
      // do not execute" from "a later stage samples something stale". bgfx::readTexture completes
      // two frames later, so each tick logs the previous request's pixels then fires a new one.
      // Single-threaded configuration only: readTexture is an API-thread call, and in ST mode this
      // present thread IS the API thread.
      if (!m_bgfxMultithreaded && g_pplayer && g_pplayer->m_renderer)
      {
         static uint64_t s_stageProbeUs = 0;
         static bgfx::TextureHandle s_staging = BGFX_INVALID_HANDLE;
         static uint8_t s_probeBuf[16 * 8] = {};
         static bool s_probePending = false;
         const uint64_t nowP = usec();
         if (nowP - s_stageProbeUs > 5000000)
         {
            s_stageProbeUs = nowP;
            // Probe the OWNED playfield texture through bgfx's own blit+read path -- unlike the
            // GL-side slot-fbo readback this uses the handle bgfx actually renders through, so a
            // fresh value here with a frozen panel isolates an override/aliasing fault.
            RenderTarget* scene = nullptr;
            if (m_ownedScanout[0].active.load(std::memory_order_relaxed))
               scene = m_ownedScanout[0].rt[m_ownedScanout[0].slot];
            if (scene == nullptr)
               scene = g_pplayer->m_renderer->GetBackBufferTexture();
            if (scene != nullptr && bgfx::isValid(scene->GetColorTexHandle()))
            {
               if (s_probePending)
               {
                  // Two 4-texel clusters blitted from the scene: centre (ball territory) and lower
                  // third (flipper territory). Raw hex is enough -- the question is whether they
                  // are nonzero and CHANGE between ticks while the ball moves.
                  uint64_t a, b, c, d;
                  memcpy(&a, &s_probeBuf[0], 8);
                  memcpy(&b, &s_probeBuf[8], 8);
                  memcpy(&c, &s_probeBuf[64], 8);
                  memcpy(&d, &s_probeBuf[72], 8);
                  PLOGI.printf("[4kpDebug][stage_probe] scene centre=%016llx %016llx lowerthird=%016llx %016llx",
                     (unsigned long long)a, (unsigned long long)b, (unsigned long long)c, (unsigned long long)d);
               }
               if (!bgfx::isValid(s_staging))
               {
                  s_staging = bgfx::createTexture2D(16, 1, false, 1, scene->GetColorTexBgfxFormat(),
                     BGFX_TEXTURE_READ_BACK | BGFX_TEXTURE_BLIT_DST);
                  static bool s_creationLogged = false;
                  if (!s_creationLogged)
                  {
                     s_creationLogged = true;
                     PLOGI.printf("[4kpDebug][stage_probe] staging texture (fmt %d): %s",
                        (int)scene->GetColorTexBgfxFormat(), bgfx::isValid(s_staging) ? "created" : "CREATION FAILED -- probe disabled");
                  }
               }
               if (bgfx::isValid(s_staging))
               {
                  // These calls encode into the NEXT frame (view 0 executes first); the result is
                  // read on the following probe tick, long past availability.
                  bgfx::blit(0, bgfx::TextureRegion(s_staging, 0, 0, 4, 1),
                     bgfx::TextureRegion(scene->GetColorTexHandle(), uint16_t(scene->GetWidth() / 2), uint16_t(scene->GetHeight() / 2), 4, 1));
                  bgfx::blit(0, bgfx::TextureRegion(s_staging, 8, 0, 4, 1),
                     bgfx::TextureRegion(scene->GetColorTexHandle(), uint16_t(scene->GetWidth() / 3), uint16_t(2 * scene->GetHeight() / 3), 4, 1));
                  bgfx::read(bgfx::TextureRegion(s_staging, 0, 0, 16, 1), s_probeBuf);
                  s_probePending = true;
               }
            }
         }
      }
      // Frozen-playfield probe: a GL error stream would mean the frame's draws are being dropped
      // before they reach the owned buffers. Drain and count; the stats line reports the total.
      static unsigned int (*s_glGetError)() = reinterpret_cast<unsigned int (*)()>(eglGetProcAddress("glGetError"));
      if (s_glGetError != nullptr)
         for (int i = 0; i < 8; ++i)
         {
            const unsigned int err = s_glGetError();
            if (err == 0)
               break;
            ++s_glErrors;
            if (s_glErrors <= 3)
               PLOGE.printf("[4kpDebug][owned_probe] GL error 0x%04x on the present thread", err);
         }
   }


   VPX::Kms::WindowPresenter* boundaryPresenter = nullptr;
   for (size_t wndIdx = 0; wndIdx < m_outputWnd.size(); ++wndIdx)
   {
      VPX::Window* wnd = m_outputWnd[wndIdx];
      SDL_Window* core = wnd ? wnd->GetCore() : nullptr;
      if (core == nullptr)
         continue;

      VPX::Kms::WindowPresenter& presenter = s_presenters[core];
      if (!presenter.IsReady())
      {
         const SDL_PropertiesID props = SDL_GetWindowProperties(core);
         const int drmFd = (int)SDL_GetNumberProperty(props, SDL_PROP_WINDOW_KMSDRM_DRM_FD_NUMBER, -1);
         const uint32_t crtcId = (uint32_t)SDL_GetNumberProperty(props, "SDL.window.kmsdrm.crtc_id", 0);
         struct gbm_surface* gs = (struct gbm_surface*)SDL_GetPointerProperty(props, "SDL.window.kmsdrm.gbm_surface", nullptr);
         if (drmFd < 0 || crtcId == 0 || gs == nullptr)
            continue;
         // Panel size, not buffer size -- this is the plane's DESTINATION rect (see GetPanelPixelWidth).
         if (!presenter.Init(drmFd, crtcId, gs, wnd->GetPanelPixelWidth(), wnd->GetPanelPixelHeight()))
         {
            PLOGE << "KMS presenter init failed for CRTC " << crtcId << " (no usable primary plane); this window will not display.";
            continue;
         }
         PLOGI << "KMS presenter ready: CRTC " << crtcId << ' ' << wnd->GetPanelPixelWidth() << 'x' << wnd->GetPanelPixelHeight()
               << " (scanout buffer " << wnd->GetPixelWidth() << 'x' << wnd->GetPixelHeight() << ')';
      }
      // Live, because it is re-read every frame: no window resize, no SDL geometry, no desktop
      // coordinates. Identity by default (scale 1, offset 0), so an unconfigured window fills its
      // panel exactly as before. Ported from the 10.8.0 fork's DMD window adjust model.
      // Only the ancillary windows carry an adjustment -- matching the fork, where this is a DMD/
      // backglass placement tool. The playfield has no entry in these property arrays, and the
      // playfield is not something to shrink on a cabinet anyway.
      const VPXWindowId wid = wnd->GetWindowId();
      if (wid == VPXWindowId::VPXWINDOW_Backglass || wid == VPXWindowId::VPXWINDOW_ScoreView || wid == VPXWindowId::VPXWINDOW_Topper)
      {
         const Settings& setts = g_pplayer->m_ptable->m_settings;
         presenter.SetAdjust(setts.GetWindow_Scale(wid), setts.GetWindow_OffsetX(wid), setts.GetWindow_OffsetY(wid));
      }
      else
      {
         presenter.SetAdjust(1.0f, 0, 0);
      }

      // Owned scanout, present-thread half: build the pool once the presenter has a live template
      // buffer (EGL/GBM work needs the GL context, which lives on this thread) and publish it to
      // the bgfx API thread, then commit whatever fully rendered slots that thread handed over.
      // Any commit failure requests a fallback; the API thread performs the actual restore, since
      // back buffers may only be swapped between frames.
      if (wndIdx < m_ownedScanout.size())
      {
         OwnedScanout& own = m_ownedScanout[wndIdx];
         if (g_pplayer && g_pplayer->m_ptable && g_pplayer->m_ptable->m_settings.GetStandalone_4kpOwnedScanout()
            && !(wndIdx != 0 && g_pplayer->m_ptable->m_settings.GetStandalone_4kpOwnedScanoutPlayfieldOnly())
            && own.slots.load(std::memory_order_relaxed) == nullptr)
         {
            presenter.ProbeOwnedScanout();
            if (presenter.GetOwnedSlots().IsReady())
               own.slots.store(&presenter.GetOwnedSlots(), std::memory_order_release);
         }
         if (own.active.load(std::memory_order_acquire))
         {
            // Heal the EGLImage sibling bindings every frame: the ancillary swap chains' surface
            // switches orphan the imported textures on this driver, silently dropping every draw
            // into the owned framebuffers (GL_INVALID_FRAMEBUFFER_OPERATION). Cheap, and it runs
            // before the next frame's draws are issued. const_cast: the pointer is stored const
            // for the commit path, but the slots object is ours and this thread owns the context.
            // The per-frame sibling re-target is itself a suspect in mixed mode: the 2026-09-01
            // run showed draws still dropped WITH it running (committed buffers read zero), while
            // rss ballooned at ~60 allocations/s and GPU time tripled -- consistent with libmali
            // giving the texture fresh private backing on every re-target after a surface switch
            // instead of rebinding the dmabuf. Full-owned (no surface switches) is flat with it
            // on, so default stays on; the setting exists to split heal from disease on device.
            static int s_siblingRefresh = -1;
            if (s_siblingRefresh < 0)
               s_siblingRefresh = (g_pplayer && g_pplayer->m_ptable
                  && g_pplayer->m_ptable->m_settings.GetStandalone_4kpSiblingRefresh()) ? 1 : 0;
            if (s_siblingRefresh != 0)
               const_cast<VPX::Kms::ScanoutSlots*>(own.slots.load(std::memory_order_relaxed))->RefreshImageBindings();
            // The driver reclamation boundary pumps after the commit loop -- see end of function.
            if (wndIdx == 0)
               boundaryPresenter = &presenter;
            const uint32_t pop = own.slotQPop.load(std::memory_order_relaxed);
            if (pop == own.slotQPush.load(std::memory_order_acquire))
            {
               if (wndIdx == 0)
                  s_pfQueueEmpty++; // frozen-playfield probe: the API thread handed nothing over
               continue; // nothing newly rendered; the last committed buffer stays on scanout
            }
            const int slotIdx = own.slotQ[pop % sizeof(own.slotQ)];
            const VPX::Kms::ScanoutSlots::Slot& slot = own.slots.load(std::memory_order_relaxed)->GetSlot(slotIdx);
            // Move the frame from the ordinary staging texture into the imported buffer -- the
            // one GL op that ever writes a sibling (10.8.0 shape). This thread executed the
            // frame's GL, so the blit is ordered after the rendering; the commit fence is minted
            // after the blit, so scanout waits for it.
            const_cast<VPX::Kms::ScanoutSlots*>(own.slots.load(std::memory_order_relaxed))->BlitStagingToSlot(slotIdx);
            // Only the playfield's drain may block: the three panels run three different refresh
            // clocks, and serially waiting on every CRTC's latch cost ~1.5 misaligned vblanks per
            // frame (38 fps with a 10 ms GPU). An ancillary commit that cannot land yet stays
            // queued and is retried next frame; its previous buffer remains on scanout.
            if (wndIdx == 0)
            {
               const uint64_t tDrain = usec();
               const bool ok = presenter.PresentOwnedFb(slot.fbId, wnd->GetPixelWidth(), wnd->GetPixelHeight());
               s_drainWaitUs += usec() - tDrain;
               if (ok)
               {
                  s_pfCommits++; // frozen-playfield probe
                  s_pfLastFbId = slot.fbId;
                  s_pfFbIdsSeen |= 1u << (slotIdx & 7);
                  // Content probe, once per ~5 s: read four pixels back from the buffer just
                  // committed. Frozen values across windows = the GPU work never lands in the
                  // owned buffers; changing values = content is fresh and the freeze is at scanout.
                  // A tiny readback stalls, which is why it is rate-limited this hard.
                  static uint64_t s_lastProbeUs = 0;
                  const uint64_t probeNow = usec();
                  if (probeNow - s_lastProbeUs > 5000000)
                  {
                     s_lastProbeUs = probeNow;
                     static void (*s_glBindFramebuffer)(unsigned int, unsigned int)
                        = reinterpret_cast<void (*)(unsigned int, unsigned int)>(eglGetProcAddress("glBindFramebuffer"));
                     static void (*s_glReadPixels)(int, int, int, int, unsigned int, unsigned int, void*)
                        = reinterpret_cast<void (*)(int, int, int, int, unsigned int, unsigned int, void*)>(eglGetProcAddress("glReadPixels"));
                     if (s_glBindFramebuffer && s_glReadPixels)
                     {
                        constexpr unsigned int GL_FRAMEBUFFER_ = 0x8D40, GL_RGBA_ = 0x1908, GL_UNSIGNED_BYTE_ = 0x1401;
                        static unsigned int (*s_glCheckFramebufferStatus)(unsigned int)
                           = reinterpret_cast<unsigned int (*)(unsigned int)>(eglGetProcAddress("glCheckFramebufferStatus"));
                        uint32_t px[4] = {};
                        const int w = wnd->GetPixelWidth(), h = wnd->GetPixelHeight();
                        s_glBindFramebuffer(GL_FRAMEBUFFER_, slot.fbo);
                        // 0x8CD5 = GL_FRAMEBUFFER_COMPLETE. Anything else names the drop mechanism:
                        // an incomplete attachment here means the imported sibling lost its storage.
                        const unsigned int fboStatus = s_glCheckFramebufferStatus ? s_glCheckFramebufferStatus(GL_FRAMEBUFFER_) : 0;
                        s_glReadPixels(w / 2, h / 2, 1, 1, GL_RGBA_, GL_UNSIGNED_BYTE_, &px[0]);
                        s_glReadPixels(w / 4, h / 4, 1, 1, GL_RGBA_, GL_UNSIGNED_BYTE_, &px[1]);
                        s_glReadPixels(3 * w / 4, h / 4, 1, 1, GL_RGBA_, GL_UNSIGNED_BYTE_, &px[2]);
                        s_glReadPixels(w / 2, 3 * h / 4, 1, 1, GL_RGBA_, GL_UNSIGNED_BYTE_, &px[3]);
                        s_glBindFramebuffer(GL_FRAMEBUFFER_, 0);
                        PLOGI.printf("[4kpDebug][owned_probe] content slot %d fb %u: %08x %08x %08x %08x | fbo status 0x%04x",
                           slotIdx, slot.fbId, px[0], px[1], px[2], px[3], fboStatus);
                     }
                  }
                  own.slotQPop.store(pop + 1, std::memory_order_release);
                  continue;
               }
            }
            else
            {
               const VPX::Kms::WindowPresenter::OwnedPresentResult r
                  = presenter.PresentOwnedFbIfIdle(slot.fbId, wnd->GetPixelWidth(), wnd->GetPixelHeight());
               if (r == VPX::Kms::WindowPresenter::OwnedPresentResult::Busy)
               {
                  s_auxBusySkips++;
                  continue; // deferred, retry next frame
               }
               if (r == VPX::Kms::WindowPresenter::OwnedPresentResult::Committed)
               {
                  own.slotQPop.store(pop + 1, std::memory_order_release);
                  continue;
               }
            }
            own.disableRequested.store(true, std::memory_order_release);
            // Fall through to the EGL present below until the API thread restores the back buffers;
            // the surface holds a stale frame but stays correctly oriented.
         }
      }
      presenter.Present();
   }

   // Publish the playfield presenter for the boundary pump. In MT mode this thread owns the GL
   // context for the whole frame, so pump here; in ST mode SubmitAndFlipFrame pumps BEFORE
   // bgfx::frame() instead -- see PumpBoundarySurface for why placement is everything.
   m_boundaryPresenter = boundaryPresenter;
   if (m_bgfxMultithreaded)
      PumpBoundarySurface();
}

// The driver reclamation boundary (see KmsBgfxPresenter BoundarySurface): without a real
// eglSwapBuffers somewhere on this context, libmali's per-frame GPU mappings accumulate without
// bound (~4 GB file-rss at OOM kill, on every owned build ever measured; a per-frame pump measured
// rss flat at 4.4 GB over a full session, and every-8th-frame pumping leaked ~8 MB/s to a 6.3 GB
// OOM, so the cadence stays at every frame).
//
// Placement is the entire cost model: the GPU queue is in-order, so the boundary swap waits for
// everything issued before it. Pumped after this frame's submission it drains the whole frame
// (25.9 ms measured -- CPU and GPU fully serialized, 30 fps). Pumped BEFORE bgfx::frame() it only
// waits out the previous frame's residue, which is near zero when the GPU beats the vblank.
void RenderDevice::PumpBoundarySurface()
{
   if (m_boundaryPresenter == nullptr)
      return;
   static VPX::Kms::WindowPresenter::BoundarySurface s_boundary;
   const uint64_t tPump = usec();
   s_boundary.Init(m_boundaryPresenter->GetGbmDevice());
   s_boundary.Pump();
   s_boundaryPumpUs += usec() - tPump;
}
#endif

#ifdef __RK3588__
// Minimal periodic frame stats, deliberately in the same shape as the 10.8.0 fork's 4kpGpuTimers
// header line so logs from the two trees can be compared directly.
//
// Only two numbers, but they answer the question that matters first: is a deficit on the GPU at all?
// frames/window is ground truth for throughput and does not care that the GPU governor cannot be
// pinned on these cabinets, while bgfx's whole-frame GPU time says how much of the frame budget the
// GPU actually consumed. If GPU time approaches the frame period the work is on the GPU; if it sits
// far below while frames are still slow, the cost is submit or present pacing instead.
//
// Per-view timings come from our bgfx patch; see the note at the accumulation site below. Enable with
// Standalone/4kpGpuTimers, matching 10.8.0.
// Several passes carry a per-frame counter in their name (e.g. "Transmitted Light 940"), which would
// shatter the aggregation into thousands of single-sample buckets and grow the table without bound.
// Fold the digits away. Same approach as the 10.8.0 fork's profiler, so the reports line up.
static string FoldPassName(const char* name)
{
   string key;
   bool lastWasDigit = false;
   for (const char* c = name; *c != '\0'; ++c)
   {
      const bool isDigit = (*c >= '0' && *c <= '9');
      if (isDigit)
      {
         if (!lastWasDigit)
            key += '#';
      }
      else
         key += *c;
      lastWasDigit = isDigit;
   }
   return key;
}

bool RenderDevice::AreFrameStatsEnabled()
{
   static bool s_enabled = false;
   static bool s_checked = false;
   if (!s_checked)
   {
      s_checked = true;
      s_enabled = g_pplayer && g_pplayer->m_ptable
         && g_pplayer->m_ptable->m_settings.GetStandalone_4kpGpuTimers();
   }
   return s_enabled;
}

void RenderDevice::LogFrameStats(uint64_t submitUs, uint64_t presentUs)
{
   if (!AreFrameStatsEnabled())
      return;

   static uint64_t s_windowStartUs = 0;
   static uint32_t s_frames = 0;
   static double s_gpuMsSum = 0.0;
   static uint64_t s_submitUsSum = 0;
   static uint64_t s_presentUsSum = 0;
   s_submitUsSum += submitUs;
   s_presentUsSum += presentUs;

   const uint64_t nowUs = usec();
   if (s_windowStartUs == 0)
      s_windowStartUs = nowUs;
   ++s_frames;

   // Frame-to-frame spike tracking: an episodic stall ("slight delay at points") vanishes into a
   // 5-second average, so keep the worst frame and count the outliers.
   static uint64_t s_lastFrameUs = 0;
   static uint64_t s_worstFrameUs = 0;
   static uint32_t s_framesOver25Ms = 0;
   if (s_lastFrameUs != 0)
   {
      const uint64_t delta = nowUs - s_lastFrameUs;
      s_worstFrameUs = max(s_worstFrameUs, delta);
      if (delta > 25000)
         ++s_framesOver25Ms;
   }
   s_lastFrameUs = nowUs;

   // Per-view GPU times come from our bgfx patch (vpx-patches/bgfx-gles-timer-and-view-stats.patch):
   // upstream bgfx allocates the per-view result slots and never fills them, and on GLES its timer is
   // disabled outright because it looks for unsuffixed glQueryCounter rather than the EXT-suffixed
   // GLES entry points. A bgfx view is a VPX pass, so this is the per-pass attribution.
   //
   // The patch times one view per frame, round-robin, so a pass is sampled once every N frames rather
   // than every frame. Average per sample, not per frame, or a pass's cost scales with how many other
   // passes the table happens to have.
   // bgfx's Profiler::end() reads m_result[view] straight after issuing the end query, so the result
   // it publishes is whichever one last completed for that view, not the one just issued. Upstream
   // times every view every frame, so that is at most a frame or two stale; with the round-robin
   // patch a view is re-timed only every N frames and the same result is published in between.
   // gpuFrameNum identifies the frame a result came from, so use it to count each result once.
   struct PassTime { double ms; uint32_t samples; uint32_t lastFrameNum; double cpuMs; uint32_t cpuSamples; };
   static std::map<std::string, PassTime> s_passes;
   static uint64_t s_drawSum = 0, s_primSum = 0, s_viewSum = 0;
   static int32_t s_transientVb = 0, s_transientIb = 0;
   static uint32_t s_dynVbCount = 0;
   static double s_renderCpuMs = 0.0, s_waitRenderMs = 0.0, s_waitSubmitMs = 0.0;
   static uint64_t s_uploadUsSum = 0, s_bgfxFrameUsSum = 0, s_preSubmitUsSum = 0;
   s_preSubmitUsSum += s_preSubmitUs;
   s_uploadUsSum += s_uploadUs;
   s_bgfxFrameUsSum += s_bgfxFrameUs;
   if (const bgfx::Stats* stats = bgfx::getStats(); stats != nullptr && stats->gpuTimerFreq > 0)
   {
      const double toMs = 1000.0 / double(stats->gpuTimerFreq);
      s_gpuMsSum += toMs * double(stats->gpuTimeEnd - stats->gpuTimeBegin);
      // Splits the submit phase: how much of it is the render thread issuing GL calls, and how much
      // is either thread waiting on the other. Whatever is left is the GPU.
      const double toCpuMs = stats->cpuTimerFreq > 0 ? 1000.0 / double(stats->cpuTimerFreq) : 0.0;
      if (stats->cpuTimerFreq > 0)
      {
         s_renderCpuMs  += toCpuMs * double(stats->cpuTimeEnd - stats->cpuTimeBegin);
         s_waitRenderMs += toCpuMs * double(stats->waitRender);
         s_waitSubmitMs += toCpuMs * double(stats->waitSubmit);
      }
      s_transientVb = max(s_transientVb, stats->transientVbUsed);
      s_transientIb = max(s_transientIb, stats->transientIbUsed);
      s_dynVbCount = max(s_dynVbCount, uint32_t(stats->numDynamicVertexBuffers));
      s_drawSum += stats->numDraw;
      s_viewSum += stats->numViews;
      for (uint8_t t = 0; t < BX_COUNTOF(stats->numPrims); ++t)
         s_primSum += stats->numPrims[t];
      for (uint16_t i = 0; i < stats->numViews; ++i)
      {
         const bgfx::ViewStats& vs = stats->viewStats[i];
         PassTime& pt = s_passes[vs.name[0] ? FoldPassName(vs.name) : ("view " + std::to_string(vs.view))];
         // CPU encode time of the view on the render thread, every frame (unlike the round-robin
         // GPU sample below). This is what attributes a driver stall -- e.g. a glBufferSubData
         // syncing on a GPU-busy buffer -- to the pass whose encode blocks.
         pt.cpuMs += toCpuMs * double(vs.cpuTimeEnd - vs.cpuTimeBegin);
         ++pt.cpuSamples;
         if (vs.gpuFrameNum == pt.lastFrameNum)
            continue; // already counted this result
         pt.lastFrameNum = vs.gpuFrameNum;
         pt.ms += toMs * double(vs.gpuTimeEnd - vs.gpuTimeBegin);
         ++pt.samples;
      }
   }

   constexpr uint64_t windowUs = 5000000ULL;
   if (nowUs - s_windowStartUs >= windowUs)
   {
      // submit is bgfx::frame (which blocks once the GPU queue is full, so it absorbs GPU-bound
      // time), present is our atomic commit plus the flip wait (which absorbs vblank pacing).
      // Whatever is left is CPU work elsewhere in the frame.
      const double frameMs = 0.001 * double(nowUs - s_windowStartUs) / double(s_frames ? s_frames : 1);
      const double submitMs = 0.001 * double(s_submitUsSum) / double(s_frames ? s_frames : 1);
      const double presentMs = 0.001 * double(s_presentUsSum) / double(s_frames ? s_frames : 1);
      const double gpuMs = s_frames ? s_gpuMsSum / s_frames : 0.0;
      // drain/pump only accumulate in single-threaded mode (in MT mode the present-thread line
      // reports them); they are inside 'present', shown here so the ST split is visible too.
      const double drainMs = 0.001 * double(s_drainWaitUs) / double(s_frames ? s_frames : 1);
      const double pumpMs = 0.001 * double(s_boundaryPumpUs) / double(s_frames ? s_frames : 1);
      PLOGI.printf("[4kpDebug][gpu_timers] ===== %.1f fps over %u frames | frame %.2f ms = submit %.2f + present %.2f (drain %.2f, pump %.2f) + other %.2f | gpu %.2f ms | %zu passes =====",
         double(s_frames) * 1000000.0 / double(nowUs - s_windowStartUs), s_frames,
         frameMs, submitMs, presentMs, drainMs, pumpMs, frameMs - submitMs - presentMs, gpuMs, s_passes.size());
      if (!m_bgfxMultithreaded)
         s_drainWaitUs = s_boundaryPumpUs = 0;
      // RSS alongside the spikes: the libmali swapless mode leaked GPU buffer mappings to a
      // 6.5 GB OOM kill before this existed; a per-5s figure makes any leak's rate visible in the
      // first minute instead of at the kill.
      long rssMb = -1;
      if (FILE* const statm = fopen("/proc/self/statm", "r"))
      {
         long pages = 0, resident = 0;
         if (fscanf(statm, "%ld %ld", &pages, &resident) == 2)
            rssMb = resident * (sysconf(_SC_PAGESIZE) / 1024) / 1024;
         fclose(statm);
      }
      PLOGI.printf("[4kpDebug][gpu_timers]   frame spikes: worst %.1f ms, %u frames over 25 ms | rss %ld MB",
         0.001 * double(s_worstFrameUs), s_framesOver25Ms, rssMb);
      s_worstFrameUs = 0;
      s_framesOver25Ms = 0;
      // The GPU is busy for only part of the frame, so what bgfx::frame() spends beyond that is the
      // CPU issuing commands. These are the counts that cost drives.
      const double perFrame = double(s_frames ? s_frames : 1);
      // Whether static parts are being composited from the prepass or re-rendered every frame. The
      // latter re-bins the whole table's static geometry per frame, which is invisible to any
      // fragment side measurement and is the shape of wall this table is hitting.
      const bool usingPrepass = g_pplayer && g_pplayer->m_renderer && g_pplayer->m_renderer->IsUsingStaticPrepass();
      // Where the frame's non-submit time goes. The logic-thread wait measures zero -- the logic
      // thread always has a frame ready, so it never holds the render loop up and double buffering
      // the render frame would remove nothing. What is left is the loop's own overhead, split at the
      // moment the frame is handed to BGFX.
      if (g_pplayer->m_renderProfiler != nullptr)
         PLOGI.printf("[4kpDebug][gpu_timers]   render loop: %.2f ms waiting on the logic thread, %.2f ms submitting | logic thread frame %.2f ms",
            0.001 * g_pplayer->m_renderProfiler->GetSlidingAvg(FrameProfiler::PROFILE_RENDER_WAIT),
            0.001 * g_pplayer->m_renderProfiler->GetSlidingAvg(FrameProfiler::PROFILE_RENDER_SUBMIT),
            0.001 * g_pplayer->m_logicProfiler.GetSlidingAvg(FrameProfiler::PROFILE_FRAME));
      const double otherMs = frameMs - submitMs - presentMs;
      const double preSubmitMs = 0.001 * double(s_preSubmitUsSum) / perFrame;
      PLOGI.printf("[4kpDebug][gpu_timers]   other %.2f ms = %.2f before submit + %.2f after flip",
         otherMs, preSubmitMs, otherMs - preSubmitMs);
      PLOGI.printf("[4kpDebug][gpu_timers]   submitted per frame: %.0f draws, %.0f primitives | static prepass %s",
         double(s_drawSum) / perFrame, double(s_primSum) / perFrame, usingPrepass ? "IN USE" : "DISABLED (statics re-rendered every frame)");
      if (g_pplayer->m_renderer != nullptr)
      {
         PLOGI.printf("[4kpDebug][gpu_timers]   ancillary windows: %u rendered, %u skipped by Standalone/AncillaryFrameDivider",
            g_pplayer->m_renderer->m_ancillaryWndRenders, g_pplayer->m_renderer->m_ancillaryWndSkips);
         g_pplayer->m_renderer->m_ancillaryWndRenders = 0;
         g_pplayer->m_renderer->m_ancillaryWndSkips = 0;
      }
      if (s_rtFrames > 0)
      {
         // The present thread's own split. renderFrame = GL issuing; present = the presenters,
         // of which drain = the playfield's blocking wait for its previous flip to latch.
         PLOGI.printf("[4kpDebug][gpu_timers]   present thread: renderFrame %.2f ms + present %.2f ms (of which playfield drain+commit %.2f, boundary pump %.2f) | aux busy-deferred %llu",
            0.001 * double(s_rtRenderUs) / double(s_rtFrames), 0.001 * double(s_rtPresentUs) / double(s_rtFrames),
            0.001 * double(s_drainWaitUs) / double(s_rtFrames), 0.001 * double(s_boundaryPumpUs) / double(s_rtFrames),
            (unsigned long long)s_auxBusySkips);
         s_rtRenderUs = s_rtPresentUs = s_rtFrames = s_drainWaitUs = s_auxBusySkips = s_boundaryPumpUs = 0;
      }
      if (s_pfCommits + s_pfQueueEmpty > 0 || s_glErrors > 0)
      {
         PLOGI.printf("[4kpDebug][owned_probe] playfield: %u commits, %u empty-queue skips, last fb %u, slot mask 0x%x | %u GL errors",
            s_pfCommits, s_pfQueueEmpty, s_pfLastFbId, s_pfFbIdsSeen, s_glErrors);
         s_pfCommits = s_pfQueueEmpty = s_pfFbIdsSeen = s_glErrors = 0;
      }
      // waitRender/waitSubmit are zero by construction here: VPX makes the calling thread the only
      // bgfx thread, so bgfx::frame() runs the backend inline and neither side ever blocks on the
      // other. What is left of bgfx::frame() after issuing is the swap, which blocks on the GPU.
      // A dynamic buffer written while the GPU may still be reading it forces a driver sync, which
      // shows up as an idle GPU and a long frame rather than as cost in any pass.
      PLOGI.printf("[4kpDebug][gpu_timers]   dynamic buffers: %.0f vb updates (%.0f KB) + %.0f ib updates (%.0f KB) per frame | %u dyn vb | transient peak %d/%d KB",
         double(s_dynVbUpdates) / perFrame, double(s_dynVbBytes) / perFrame / 1024.0,
         double(s_dynIbUpdates) / perFrame, double(s_dynIbBytes) / perFrame / 1024.0,
         s_dynVbCount, s_transientVb / 1024, s_transientIb / 1024);
      // PHASE 1 DIAGNOSTIC (temporary): the bgfx probe puts the playfield GPU fence wait in
      // waitRender and the residual eglSwapBuffers in waitSubmit. Fence long / swap short means
      // genuinely GPU bound; fence short / swap long means the producer is waiting for a scanout
      // slot, which is what 10.8.0's three-slot comment describes.
      PLOGI.printf("[4kpDebug][gpu_timers]   playfield: gpu fence %.2f ms + residual swap %.2f ms  (PHASE1)",
         s_waitRenderMs / perFrame, s_waitSubmitMs / perFrame);
      PLOGI.printf("[4kpDebug][gpu_timers]   submit %.2f ms = uploads %.2f + bgfx::frame %.2f (of which %.2f issuing, %.2f swap)",
         submitMs, 0.001 * double(s_uploadUsSum) / perFrame, 0.001 * double(s_bgfxFrameUsSum) / perFrame,
         s_renderCpuMs / perFrame, 0.001 * double(s_bgfxFrameUsSum) / perFrame - s_renderCpuMs / perFrame);
      // Descending, so the expensive pass is the first line rather than buried.
      std::vector<std::pair<std::string, PassTime>> passes(s_passes.begin(), s_passes.end());
      std::sort(passes.begin(), passes.end(),
         [](const auto& a, const auto& b) { return a.second.ms / a.second.samples > b.second.ms / b.second.samples; });
      for (const auto& [name, pt] : passes)
      {
         if (pt.samples == 0)
            continue; // no fresh result for this pass in this window
         const double perPass = pt.ms / double(pt.samples);
         const double perPassCpu = pt.cpuSamples > 0 ? pt.cpuMs / double(pt.cpuSamples) : 0.0;
         if (perPass >= 0.005 || perPassCpu >= 0.5) // below this it is noise, and the list gets long
            PLOGI.printf("[4kpDebug][gpu_timers]   %-40s %6.2f ms (%4.1f%% of gpu, n=%u) | cpu %.2f ms", name.c_str(), perPass,
               gpuMs > 0.0 ? 100.0 * perPass / gpuMs : 0.0, pt.samples, perPassCpu);
      }

      s_windowStartUs = nowUs;
      s_frames = 0;
      s_gpuMsSum = 0.0;
      s_submitUsSum = 0;
      s_presentUsSum = 0;
      for (auto& [name, pt] : s_passes)
      {
         pt.ms = 0.0;
         pt.samples = 0; // keep lastFrameNum: the result it refers to has already been counted
         pt.cpuMs = 0.0;
         pt.cpuSamples = 0;
      }
      s_drawSum = s_primSum = s_viewSum = 0;
      s_renderCpuMs = s_waitRenderMs = s_waitSubmitMs = 0.0;
      s_uploadUsSum = s_bgfxFrameUsSum = s_preSubmitUsSum = 0;
      s_dynVbUpdates = s_dynIbUpdates = 0;
      s_dynVbBytes = s_dynIbBytes = 0;
      s_transientVb = s_transientIb = 0;
      s_dynVbCount = 0;
   }
}
#endif

void RenderDevice::Flip()
{
   // The calls below may or may not block, depending on the device configuration and the state of its frame queue. The driver may also
   // block on the first draw call that needs to access a backbuffer when they are all waiting to be presented. To ensure non blocking 
   // calls, we need to schedule frames at a pace adjusted to the actual render speed (to avoid filling up the queue, leading to subsequent call to wait).
   //
   // This matters and should be avoided since these blocking calls will delay the input/physics update (they catchup afterward) and that 
   // it will break some PinMAME video modes (since input events will be fast forwarded, the controller misses some like in Lethal 
   // Weapon 3 fight) and make the gameplay (input lag, input-physics sync, input-controller sync) to depend on the framerate.

   // reset performance counters
   m_frameDrawCalls = m_curDrawCalls;
   m_curDrawCalls = 0;
   m_frameStateChanges = m_curStateChanges;
   m_curStateChanges = 0;
   m_frameTextureChanges = m_curTextureChanges;
   m_curTextureChanges = 0;
   m_frameParameterChanges = m_curParameterChanges;
   m_curParameterChanges = 0;
   m_frameTechniqueChanges = m_curTechniqueChanges;
   m_curTechniqueChanges = 0;
   m_frameDrawnTriangles = m_curDrawnTriangles;
   m_curDrawnTriangles = 0;
   m_frameTextureUpdates = m_curTextureUpdates;
   m_curTextureUpdates = 0;
   m_frameLockCalls = m_curLockCalls;
   m_curLockCalls = 0;

   // Schedule frame presentation (non blocking call, simply queueing the present command in the driver's render queue with a schedule for execution)
   #if defined(ENABLE_BGFX)
   #ifdef __RK3588__
   const uint64_t t0 = usec();
   SubmitAndFlipFrame(true);
   const uint64_t t1 = usec();
   // Multithreaded: the render thread presents, right after renderFrame() reports a frame. Calling
   // it here too would present from a thread with no GL context and race the one that does.
   if (!m_bgfxMultithreaded)
      PresentKmsWindows();
   const uint64_t t2 = usec();
   LogFrameStats(t1 - t0, t2 - t1);
   #else
   SubmitAndFlipFrame(true);
   #endif

   #elif defined(ENABLE_OPENGL)
   SDL_GL_SwapWindow(m_outputWnd[0]->GetCore());
   if (!m_isVR)
      g_pplayer->m_logicProfiler.OnPresented(usec());
   if (m_screenshotFrameDelay > 0)
      CaptureGLScreenshot();

   #elif defined(ENABLE_DX9)
   CHECKD3D(m_pD3DDevice->Present(nullptr, nullptr, nullptr, nullptr));
   if (!m_isVR)
      g_pplayer->m_logicProfiler.OnPresented(usec());
   if (m_screenshotFrameDelay > 0)
      CaptureDX9Screenshot();

   #endif

   // A frame is now actually on screen: tell the launcher we are up. Fired from the render thread
   // right after the present, so we never signal a frame that has only been queued. Self-guarded,
   // so calling it on every frame is free, and a no-op when not launched by go-vpx-launcher.
   vpx_ready_signal_fire();
}

void RenderDevice::UploadAndSetSMAATextures()
{
   // TODO use standard BaseTexture / Sampler code instead
   /* std::shared_ptr<BaseTexture> searchBaseTex = BaseTexture::Create(SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, BaseTexture::BW);
   memcpy(searchBaseTex->data(), searchTexBytes, SEARCHTEX_SIZE);
   m_SMAAsearchTexture = std::make_shared<Sampler>(this, "SMAA Search"s, searchBaseTex, true);
   m_SMAAsearchTexture->SetName("SMAA Search"s); */

#if defined(ENABLE_BGFX)
   bgfx::TextureHandle smaaAreaTex = bgfx::createTexture2D(AREATEX_WIDTH, AREATEX_HEIGHT, false, 1, bgfx::TextureFormat::RG8, BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP, bgfx::makeRef(areaTexBytes, AREATEX_SIZE));
   m_SMAAareaTexture = std::make_shared<Sampler>(this, "SMAA Area"s, SurfaceType::RT_DEFAULT, smaaAreaTex, bgfx::TextureFormat::RG8, AREATEX_WIDTH, AREATEX_HEIGHT, true);

   bgfx::TextureHandle smaaSearchTex = bgfx::createTexture2D(SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, false, 1, bgfx::TextureFormat::R8, BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP, bgfx::makeRef(searchTexBytes, SEARCHTEX_SIZE));
   m_SMAAsearchTexture = std::make_shared<Sampler>(this, "SMAA Search"s, SurfaceType::RT_DEFAULT, smaaSearchTex, bgfx::TextureFormat::R8, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, true);

#elif defined(ENABLE_OPENGL)
   auto tex_unit = m_samplerBindings.back();
   if (tex_unit->sampler != nullptr)
      tex_unit->sampler->m_bindings.erase(tex_unit);
   tex_unit->sampler = nullptr;
   glActiveTexture(GL_TEXTURE0 + tex_unit->unit);
   GLuint glTexture[2];
   glGenTextures(2, glTexture);

   glBindTexture(GL_TEXTURE_2D, glTexture[0]);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
   glTexStorage2D(GL_TEXTURE_2D, 1, GL_R8, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT);
   glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
   glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, GL_RED, GL_UNSIGNED_BYTE, (void*)searchTexBytes);
   m_SMAAsearchTexture = std::make_shared<Sampler>(this, "SMAA Search"s, SurfaceType::RT_DEFAULT, glTexture[0], true);

   glBindTexture(GL_TEXTURE_2D, glTexture[1]);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
   glTexStorage2D(GL_TEXTURE_2D, 1, GL_RG8, AREATEX_WIDTH, AREATEX_HEIGHT);
   glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
   glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, AREATEX_WIDTH, AREATEX_HEIGHT, GL_RG, GL_UNSIGNED_BYTE, (void*)areaTexBytes);
   m_SMAAareaTexture = std::make_shared<Sampler>(this, "SMAA Area"s, SurfaceType::RT_DEFAULT, glTexture[1], true);

#elif defined(ENABLE_DX9)
   {
      IDirect3DTexture9 *sysTex, *tex;
      HRESULT hr = m_pD3DDevice->CreateTexture(SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, 0, 0, D3DFMT_L8, D3DPOOL_SYSTEMMEM, &sysTex, nullptr);
      if (FAILED(hr))
         ReportError("Fatal Error: unable to create texture!"s, hr, __FILE__, __LINE__);
      hr = m_pD3DDevice->CreateTexture(SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, 0, 0, D3DFMT_L8, D3DPOOL_DEFAULT, &tex, nullptr);
      if (FAILED(hr))
         ReportError("Fatal Error: out of VRAM!"s, hr, __FILE__, __LINE__);

      //!! use D3DXLoadSurfaceFromMemory
      D3DLOCKED_RECT locked;
      CHECKD3D(sysTex->LockRect(0, &locked, nullptr, 0));
      void* const pdest = locked.pBits;
      const void* const psrc = searchTexBytes;
      memcpy(pdest, psrc, SEARCHTEX_SIZE);
      CHECKD3D(sysTex->UnlockRect(0));

      CHECKD3D(m_pD3DDevice->UpdateTexture(sysTex, tex));
      SAFE_RELEASE(sysTex);

      m_SMAAsearchTexture = std::make_shared<Sampler>(this, "SMAA Search"s, tex, true);
   }
   {
      IDirect3DTexture9 *sysTex, *tex;
      HRESULT hr = m_pD3DDevice->CreateTexture(AREATEX_WIDTH, AREATEX_HEIGHT, 0, 0, D3DFMT_A8L8, D3DPOOL_SYSTEMMEM, &sysTex, nullptr);
      if (FAILED(hr))
         ReportError("Fatal Error: unable to create texture!"s, hr, __FILE__, __LINE__);
      hr = m_pD3DDevice->CreateTexture(AREATEX_WIDTH, AREATEX_HEIGHT, 0, 0, D3DFMT_A8L8, D3DPOOL_DEFAULT, &tex, nullptr);
      if (FAILED(hr))
         ReportError("Fatal Error: out of VRAM!"s, hr, __FILE__, __LINE__);

      //!! use D3DXLoadSurfaceFromMemory
      D3DLOCKED_RECT locked;
      CHECKD3D(sysTex->LockRect(0, &locked, nullptr, 0));
      void* const pdest = locked.pBits;
      const void* const psrc = areaTexBytes;
      memcpy(pdest, psrc, AREATEX_SIZE);
      CHECKD3D(sysTex->UnlockRect(0));

      CHECKD3D(m_pD3DDevice->UpdateTexture(sysTex, tex));
      SAFE_RELEASE(sysTex);

      m_SMAAareaTexture = std::make_shared<Sampler>(this, "SMAA Area"s, tex, true);
   }
#endif

   m_FBShader->SetTexture(ShaderUniform::areaTex, m_SMAAareaTexture);
   m_FBShader->SetTexture(ShaderUniform::searchTex, m_SMAAsearchTexture);
}

void RenderDevice::UploadTexture(ITexManCacheable* texture, const bool linearRGB)
{
   std::shared_ptr<Sampler> sampler = m_texMan.LoadTexture(texture, linearRGB);
   #if defined(ENABLE_BGFX)
   // BGFX dispatch operations to the render thread, so the texture manager does not actually loads data to the GPU nor perform mipmap generation
   std::lock_guard lock(m_frameMutex);
   m_pendingTextureUploads.push_back(sampler);
   SubmitRenderFrame(); // Submit texture upload to render thread
   SubmitRenderFrame(); // Block until render thread has processed the pending texture uploads and mipmap generations
   #endif
}

void RenderDevice::SetSamplerState(int unit, SamplerFilter filter, SamplerAddressMode clamp_u, SamplerAddressMode clamp_v)
{
#if defined(ENABLE_BGFX)
#elif defined(ENABLE_OPENGL)
   assert(std::size(m_samplerStateCache) == 3*3*6);
   int samplerStateId = min((int)clamp_u, 2) * 6 * 3
                      + min((int)clamp_v, 2) * 6
                      + min((int)filter, 5);
   GLuint sampler_state = m_samplerStateCache[samplerStateId];
   if (sampler_state == 0)
   {
      m_curStateChanges += 5;
      glGenSamplers(1, &sampler_state);
      m_samplerStateCache[samplerStateId] = sampler_state;
      static constexpr int glAddress[] = { GL_REPEAT, GL_CLAMP_TO_EDGE, GL_MIRRORED_REPEAT, GL_REPEAT };
      glSamplerParameteri(sampler_state, GL_TEXTURE_WRAP_S, glAddress[static_cast<unsigned int>(clamp_u)]);
      glSamplerParameteri(sampler_state, GL_TEXTURE_WRAP_T, glAddress[static_cast<unsigned int>(clamp_v)]);
      switch (filter)
      {
      default: assert(!"unknown filter");
      case SamplerFilter::SF_NONE: // No mipmapping
         glSamplerParameteri(sampler_state, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
         glSamplerParameteri(sampler_state, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
         glSamplerParameterf(sampler_state, GL_TEXTURE_MAX_ANISOTROPY, 1.0f);
         break;
      case SamplerFilter::SF_BILINEAR: // Bilinear texture filtering.
         glSamplerParameteri(sampler_state, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
         glSamplerParameteri(sampler_state, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
         glSamplerParameterf(sampler_state, GL_TEXTURE_MAX_ANISOTROPY, 1.0f);
         break;
      case SamplerFilter::SF_TRILINEAR: // Trilinear texture filtering.
         glSamplerParameteri(sampler_state, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
         glSamplerParameteri(sampler_state, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
         glSamplerParameterf(sampler_state, GL_TEXTURE_MAX_ANISOTROPY, 1.0f);
         break;
      case SamplerFilter::SF_ANISOTROPIC: // Anisotropic texture filtering.
         glSamplerParameteri(sampler_state, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
         glSamplerParameteri(sampler_state, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
         glSamplerParameterf(sampler_state, GL_TEXTURE_MAX_ANISOTROPY, m_maxaniso);
         break;
      case SamplerFilter::SF_PIXELATED: // Point magnification, filtered (anisotropic) minification.
         glSamplerParameteri(sampler_state, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
         glSamplerParameteri(sampler_state, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
         glSamplerParameterf(sampler_state, GL_TEXTURE_MAX_ANISOTROPY, m_maxaniso);
         break;
      }
   }
   glBindSampler(unit, sampler_state);
   m_curStateChanges++;
#elif defined(ENABLE_DX9)
   if (filter != m_bound_filter[unit])
   {
      switch (filter)
      {
      default:
      case SamplerFilter::SF_NONE:
         // Don't filter textures, no mipmapping.
         CHECKD3D(m_pD3DDevice->SetSamplerState(unit, D3DSAMP_MAGFILTER, D3DTEXF_POINT));
         CHECKD3D(m_pD3DDevice->SetSamplerState(unit, D3DSAMP_MINFILTER, D3DTEXF_POINT));
         CHECKD3D(m_pD3DDevice->SetSamplerState(unit, D3DSAMP_MIPFILTER, D3DTEXF_NONE));
         m_curStateChanges+=3;
         break;

      case SamplerFilter::SF_BILINEAR:
         // Interpolate in 2x2 texels, no mipmapping.
         CHECKD3D(m_pD3DDevice->SetSamplerState(unit, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR));
         CHECKD3D(m_pD3DDevice->SetSamplerState(unit, D3DSAMP_MINFILTER, D3DTEXF_LINEAR));
         CHECKD3D(m_pD3DDevice->SetSamplerState(unit, D3DSAMP_MIPFILTER, D3DTEXF_NONE));
         m_curStateChanges += 3;
         break;

      case SamplerFilter::SF_TRILINEAR:
         // Filter textures on 2 mip levels (interpolate in 2x2 texels). And filter between the 2 mip levels.
         CHECKD3D(m_pD3DDevice->SetSamplerState(unit, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR));
         CHECKD3D(m_pD3DDevice->SetSamplerState(unit, D3DSAMP_MINFILTER, D3DTEXF_LINEAR));
         CHECKD3D(m_pD3DDevice->SetSamplerState(unit, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR));
         m_curStateChanges += 3;
         break;

      case SamplerFilter::SF_ANISOTROPIC:
         // Full HQ anisotropic Filter. Should lead to driver doing whatever it thinks is best.
         CHECKD3D(m_pD3DDevice->SetSamplerState(unit, D3DSAMP_MAGFILTER, m_mag_aniso ? D3DTEXF_ANISOTROPIC : D3DTEXF_LINEAR));
         CHECKD3D(m_pD3DDevice->SetSamplerState(unit, D3DSAMP_MINFILTER, D3DTEXF_ANISOTROPIC));
         CHECKD3D(m_pD3DDevice->SetSamplerState(unit, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR));
         CHECKD3D(m_pD3DDevice->SetSamplerState(unit, D3DSAMP_MAXANISOTROPY, min(m_maxaniso, (DWORD)16)));
         m_curStateChanges += 4;
         break;

      case SamplerFilter::SF_PIXELATED:
         // Keep crisp texels when magnified, but filter (and mipmap) when minified to avoid aliasing.
         CHECKD3D(m_pD3DDevice->SetSamplerState(unit, D3DSAMP_MAGFILTER, D3DTEXF_POINT));
         CHECKD3D(m_pD3DDevice->SetSamplerState(unit, D3DSAMP_MINFILTER, D3DTEXF_ANISOTROPIC));
         CHECKD3D(m_pD3DDevice->SetSamplerState(unit, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR));
         CHECKD3D(m_pD3DDevice->SetSamplerState(unit, D3DSAMP_MAXANISOTROPY, min(m_maxaniso, (DWORD)16)));
         m_curStateChanges += 4;
         break;
      }
      m_bound_filter[unit] = filter;
   }
   if (clamp_u != m_bound_clampu[unit])
   {
      switch (clamp_u)
      {
         case SamplerAddressMode::SA_REPEAT: CHECKD3D(m_pD3DDevice->SetSamplerState(unit, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP)); m_curStateChanges++; break;
         case SamplerAddressMode::SA_CLAMP: CHECKD3D(m_pD3DDevice->SetSamplerState(unit, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP)); m_curStateChanges++; break;
         case SamplerAddressMode::SA_MIRROR: CHECKD3D(m_pD3DDevice->SetSamplerState(unit, D3DSAMP_ADDRESSU, D3DTADDRESS_MIRROR)); m_curStateChanges++; break;
      }
      m_bound_clampu[unit] = clamp_u;
   }
   if (clamp_v != m_bound_clampv[unit])
   {
      switch (clamp_v)
      {
         case SamplerAddressMode::SA_REPEAT: CHECKD3D(m_pD3DDevice->SetSamplerState(unit, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP)); m_curStateChanges++; break;
         case SamplerAddressMode::SA_CLAMP: CHECKD3D(m_pD3DDevice->SetSamplerState(unit, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP)); m_curStateChanges++; break;
         case SamplerAddressMode::SA_MIRROR: CHECKD3D(m_pD3DDevice->SetSamplerState(unit, D3DSAMP_ADDRESSV, D3DTADDRESS_MIRROR)); m_curStateChanges++; break;
      }
      m_bound_clampv[unit] = clamp_v;
   }
#endif
}

void RenderDevice::SetRenderState(const RenderState::RenderStates p1, const RenderState::RenderStateValue p2) 
{
   m_renderstate.SetRenderState(p1, p2);
}

void RenderDevice::SetRenderStateDepthBias(float bias)
{
   m_renderstate.SetRenderStateDepthBias(bias);
}

void RenderDevice::EnableAlphaBlend(const bool additiveBlending, const bool set_dest_blend, const bool set_blend_op)
{
   SetRenderState(RenderState::ALPHABLENDENABLE, RenderState::RS_TRUE);
   SetRenderState(RenderState::SRCBLEND, RenderState::SRC_ALPHA);
   if (set_dest_blend)
      SetRenderState(RenderState::DESTBLEND, additiveBlending ? RenderState::ONE : RenderState::INVSRC_ALPHA);
   if (set_blend_op)
      SetRenderState(RenderState::BLENDOP, RenderState::BLENDOP_ADD);
}

void RenderDevice::CopyRenderStates(const bool copyTo, RenderState& state)
{
   if (copyTo)
   {
      state.m_state = m_renderstate.m_state;
      state.m_depthBias = m_renderstate.m_depthBias;
   }
   else
   {
      m_renderstate.m_state = state.m_state;
      m_renderstate.m_depthBias = state.m_depthBias;
   }
}

void RenderDevice::ApplyRenderStates()
{
   m_renderstate.Apply(this);
}

void RenderDevice::CopyRenderAndShaderStates(const bool copyTo, RenderDeviceState& state)
{
   assert(state.m_rd == this);
   CopyRenderStates(copyTo, state.m_renderState);
   m_uiShader->m_state->CopyTo(copyTo, state.m_uiShaderState);
   m_basicShader->m_state->CopyTo(copyTo, state.m_basicShaderState);
   m_DMDShader->m_state->CopyTo(copyTo, state.m_DMDShaderState);
   m_FBShader->m_state->CopyTo(copyTo, state.m_FBShaderState);
   m_flasherShader->m_state->CopyTo(copyTo, state.m_flasherShaderState);
   m_lightShader->m_state->CopyTo(copyTo, state.m_lightShaderState);
   m_ballShader->m_state->CopyTo(copyTo, state.m_ballShaderState);
   if (m_stereoShader)
      m_stereoShader->m_state->CopyTo(copyTo, state.m_stereoShaderState);
}

void RenderDevice::SetClipPlane(const vec4 &plane)
{
#if defined(__OPENGLES__)
   // FIXME GLES implement (or use BGFX OpenGL ES implementation)
   return;
#elif defined(ENABLE_BGFX)
   //m_DMDShader->SetVector(ShaderUniform::clip_plane, &plane); // FIXME
   m_basicShader->SetVector(ShaderUniform::clip_plane, &plane);
   m_lightShader->SetVector(ShaderUniform::clip_plane, &plane);
   m_flasherShader->SetVector(ShaderUniform::clip_plane, &plane);
   m_ballShader->SetVector(ShaderUniform::clip_plane, &plane);
#elif defined(ENABLE_OPENGL)
   m_DMDShader->SetVector(ShaderUniform::clip_plane, &plane);
   m_basicShader->SetVector(ShaderUniform::clip_plane, &plane);
   m_lightShader->SetVector(ShaderUniform::clip_plane, &plane);
   m_flasherShader->SetVector(ShaderUniform::clip_plane, &plane);
   m_ballShader->SetVector(ShaderUniform::clip_plane, &plane);
#elif defined(ENABLE_DX9)
   // FIXME DX9 shouldn't we set the Model matrix to identity first ?
   Matrix3D mT = g_pplayer->m_renderer->GetMVP().GetModelViewProj(0); // = world * view * proj
   mT.Invert();
   mT.Transpose();
   const D3DXMATRIX m(mT);
   D3DXPLANE clipSpacePlane;
   const D3DXPLANE dxplane(-plane.x, -plane.y, -plane.z, -plane.w);
   D3DXPlaneTransform(&clipSpacePlane, &dxplane, &m);
   GetCoreDevice()->SetClipPlane(0, clipSpacePlane);
#endif
}

void RenderDevice::SubmitRenderFrame()
{
   #ifdef ENABLE_BGFX
   if (std::this_thread::get_id() != m_renderThread.get_id())
   {
      // post semaphore and wait for render thread to process frame
      assert(!m_framePending);
      m_framePending = true;
      m_frameNoPresent = true;
      m_frameMutex.unlock(); // release the lock and wait for render thread to process the frame
      m_frameReadySem.release();
      while (m_framePending || !m_frameMutex.try_lock())
      {
         g_pplayer->ProcessOSMessages();
         Sleep(0);
      }
      return;
   }
   #endif

   m_currentPass = nullptr;
   const bool rendered = m_renderFrame->Execute(m_logNextFrame);
   if (rendered)
      m_logNextFrame = false;
   m_lastPresentFrameTick = usec();
}

void RenderDevice::DiscardRenderFrame()
{
   m_currentPass = nullptr;
   m_renderFrame->Discard();
   #ifdef ENABLE_BGFX
   ResetActiveView();
   #endif
}

void RenderDevice::SetRenderTarget(const string& name, RenderTarget* rt, const bool useRTContent, const bool forceNewPass)
{
   if (rt == nullptr)
   {
      m_currentPass = nullptr;
   }
   else if (m_currentPass == nullptr || !useRTContent || rt != m_currentPass->m_rt || forceNewPass)
   {
      m_currentPass = m_renderFrame->AddPass(name, rt);
      m_currentPass->m_mergeable = !forceNewPass;
      if (useRTContent && rt->m_lastRenderPass != nullptr)
      {
         for (auto precursors : rt->m_lastRenderPass->m_dependencies)
            m_currentPass->AddPrecursor(precursors);
         m_currentPass->AddPrecursor(rt->m_lastRenderPass);
      }
      rt->m_lastRenderPass = m_currentPass;
   }
}

void RenderDevice::AddRenderTargetDependency(RenderTarget* rt, const bool needDepth)
{
   if (m_currentPass != nullptr && rt->m_lastRenderPass != nullptr)
   {
      rt->m_lastRenderPass->m_depthReadback |= needDepth;
      m_currentPass->AddPrecursor(rt->m_lastRenderPass);
   }
}

void RenderDevice::AddRenderTargetDependencyOnNextRenderCommand(RenderTarget* rt)
{
   assert(m_nextRenderCommandDependency == nullptr); // Only one dependency can be added on a render command
   m_nextRenderCommandDependency = rt->m_lastRenderPass;
}

void RenderDevice::Clear(const DWORD flags, const DWORD color)
{
   ApplyRenderStates();
   RenderCommand* cmd = m_renderFrame->NewCommand();
   cmd->SetClear(flags, color);
   cmd->m_dependency = m_nextRenderCommandDependency;
   m_nextRenderCommandDependency = nullptr;
   m_currentPass->Submit(cmd);
}

void RenderDevice::BlitRenderTarget(RenderTarget* source, RenderTarget* destination, bool copyColor, bool copyDepth, const int x1, const int y1, const int w1, const int h1, const int x2,
   const int y2, const int w2, const int h2, const int srcLayer, const int dstLayer)
{
   assert(m_currentPass->m_rt == destination); // We must be on a render pass targeted at the destination for correct render pass sorting
   AddRenderTargetDependency(source);
   RenderCommand* cmd = m_renderFrame->NewCommand();
   cmd->SetCopy(source, destination, copyColor, copyDepth, x1, y1, w1, h1, x2, y2, w2, h2, srcLayer, dstLayer);
   cmd->m_dependency = m_nextRenderCommandDependency;
   m_nextRenderCommandDependency = nullptr;
   m_currentPass->Submit(cmd);
}

void RenderDevice::DrawTexturedQuad(Shader* shader, const Vertex3D_TexelOnly* vertices, const bool isTransparent, const float depth)
{
   assert(shader == m_FBShader || shader == m_stereoShader); // FrameBuffer/Stereo shaders are the only ones using Position/Texture vertex format
   ApplyRenderStates();
   RenderCommand* cmd = m_renderFrame->NewCommand();
   const Vertex3D_TexelOnly* submittedVertices = vertices;
#if defined(ENABLE_BGFX) && defined(__RK3588__)
   // A directly scanned-out target has the opposite vertical origin to a normal GL framebuffer.
   // These quads are the only writers of the playfield output (tonemap, AA, sharpen, upscale,
   // stereo), so flipping their texture V here renders the whole frame right-way-up in the buffer
   // and no plane rotation is ever needed. Same mechanism as the 10.8.0 fork.
   Vertex3D_TexelOnly flippedVertices[4];
   if (IsCurrentPassScanout())
   {
      memcpy(flippedVertices, vertices, sizeof(flippedVertices));
      for (Vertex3D_TexelOnly& vertex : flippedVertices)
         vertex.tv = 1.0f - vertex.tv;
      submittedVertices = flippedVertices;
   }
#endif
   cmd->SetDrawTexturedQuad(shader, submittedVertices, isTransparent, depth);
   cmd->m_dependency = m_nextRenderCommandDependency;
   m_nextRenderCommandDependency = nullptr;
   m_currentPass->Submit(cmd);
}

void RenderDevice::DrawTexturedQuad(Shader* shader, const Vertex3D_NoTex2* vertices, const bool isTransparent, const float depth)
{
   assert(shader != m_FBShader && shader != m_stereoShader); // FrameBuffer/Stereo shaders are the only ones using Position/Texture vertex format
   ApplyRenderStates();
   RenderCommand* cmd = m_renderFrame->NewCommand();
   cmd->SetDrawTexturedQuad(shader, vertices, isTransparent, depth);
   cmd->m_dependency = m_nextRenderCommandDependency;
   m_nextRenderCommandDependency = nullptr;
   m_currentPass->Submit(cmd);
}

void RenderDevice::DrawFullscreenTexturedQuad(Shader* shader)
{
   assert(shader == m_FBShader || shader == m_stereoShader); // FrameBuffer/Stereo shaders are the only ones using Position/Texture vertex format
#if defined(ENABLE_BGFX) && defined(__RK3588__)
   // See DrawTexturedQuad: render right-way-up into a directly scanned-out target.
   if (IsCurrentPassScanout())
   {
      static constexpr Vertex3D_TexelOnly fullscreenFlippedVertices[4] = {
         { 1.0f, 1.0f, 0.0f, 1.0f, 0.0f },
         { -1.0f, 1.0f, 0.0f, 0.0f, 0.0f },
         { 1.0f, -1.0f, 0.0f, 1.0f, 1.0f },
         { -1.0f, -1.0f, 0.0f, 0.0f, 1.0f },
      };
      DrawTexturedQuad(shader, fullscreenFlippedVertices);
      return;
   }
#endif
   static constexpr Vertex3Ds pos { 0.f, 0.f, 0.f };
   DrawMesh(shader, false, pos, 0.f, m_quadMeshBuffer, TRIANGLESTRIP, 0, 4);
}

void RenderDevice::DrawMesh(Shader* shader, const bool isTranparentPass, const Vertex3Ds& center, const float depthBias, std::shared_ptr<MeshBuffer> mb, const PrimitiveTypes type, const uint32_t startIndex, const uint32_t indexCount)
{
   RenderCommand* cmd = m_renderFrame->NewCommand();
   float depth;
   if (g_pplayer->m_renderer == nullptr)
      // This happens during startup for offscreen rendering (somewhat hacky)
      depth = 0.f;
   else if (g_pplayer->m_renderer->GetShadeMode() != Renderer::ShadeMode::Default)
      // Used by the new wireframe renderer: sort along the left eye view vector
      //depth = isTranparentPass ? g_pplayer->m_renderer->GetMVP().GetModelView().MultiplyVectorNoPerspective(center).z : -g_pplayer->m_renderer->GetMVP().GetModelView().MultiplyVectorNoPerspective(center).z;
      // back to front
      depth = g_pplayer->m_renderer->GetMVP().GetModelView(0).MultiplyVectorNoPerspective(center).z;
   else
      // Legacy sorting order (only along negative z axis, which is reversed for reflections).
      // This is completely wrong, but needed to preserve backward compatibility. We should sort along the view axis (especially for reflection probes)
      depth = g_pplayer->m_renderer->IsRenderPass(Renderer::REFLECTION_PASS) ? depthBias + center.z : depthBias - center.z;
   // We can not use the real opacity from render states since some legacy code uses the alpha part that writes to the depth buffer (rendered during transparent pass) to mask out opaque parts
   cmd->SetDrawMesh(shader, mb, type, startIndex, indexCount, isTranparentPass /* && !GetRenderState().IsOpaque() */, depth);
   cmd->m_dependency = m_nextRenderCommandDependency;
   m_nextRenderCommandDependency = nullptr;
   m_currentPass->Submit(cmd);
}

void RenderDevice::DrawGaussianBlur(RenderTarget* source, RenderTarget* tmp, RenderTarget* dest, float kernel_size, int singleLayer)
{
   ShaderTechnique tech_h, tech_v;
   if (kernel_size < 8)
   {
      tech_h = ShaderTechnique::fb_blur_horiz7x7;
      tech_v = ShaderTechnique::fb_blur_vert7x7;
   }
   else if (kernel_size < 10)
   {
      tech_h = ShaderTechnique::fb_blur_horiz9x9;
      tech_v = ShaderTechnique::fb_blur_vert9x9;
   }
   else if (kernel_size < 12)
   {
      tech_h = ShaderTechnique::fb_blur_horiz11x11;
      tech_v = ShaderTechnique::fb_blur_vert11x11;
   }
   else if (kernel_size < 14)
   {
      tech_h = ShaderTechnique::fb_blur_horiz13x13;
      tech_v = ShaderTechnique::fb_blur_vert13x13;
   }
   else if (kernel_size < 17)
   {
      tech_h = ShaderTechnique::fb_blur_horiz15x15;
      tech_v = ShaderTechnique::fb_blur_vert15x15;
   }
   else if (kernel_size < 21)
   {
      tech_h = ShaderTechnique::fb_blur_horiz19x19;
      tech_v = ShaderTechnique::fb_blur_vert19x19;
   }
   else if (kernel_size < 25)
   {
      tech_h = ShaderTechnique::fb_blur_horiz23x23;
      tech_v = ShaderTechnique::fb_blur_vert23x23;
   }
   else if (kernel_size < 31)
   {
      tech_h = ShaderTechnique::fb_blur_horiz27x27;
      tech_v = ShaderTechnique::fb_blur_vert27x27;
   }
   else
   {
      tech_h = ShaderTechnique::fb_blur_horiz39x39;
      tech_v = ShaderTechnique::fb_blur_vert39x39;
   }

   RenderPass* const initial_rt = GetCurrentPass();
   RenderState initial_state;
   CopyRenderStates(true, initial_state);
   ResetRenderState();
   SetRenderState(RenderState::ALPHABLENDENABLE, RenderState::RS_FALSE);
   SetRenderState(RenderState::CULLMODE, RenderState::CULL_NONE);
   SetRenderState(RenderState::ZWRITEENABLE, RenderState::RS_FALSE);
   SetRenderState(RenderState::ZENABLE, RenderState::RS_FALSE);
   {
      m_FBShader->SetTextureNull(ShaderUniform::tex_fb_filtered);
      SetRenderTarget(initial_rt->m_name + " HBlur", tmp, false); // switch to temporary output buffer for horizontal phase of gaussian blur
      m_currentPass->m_singleLayerRendering = singleLayer; // We support blurring a single layer (for anaglyph defocusing)
      AddRenderTargetDependency(source);
      m_FBShader->SetTexture(ShaderUniform::tex_fb_filtered, source->GetColorSampler());
      m_FBShader->SetVector(ShaderUniform::w_h_height, (float)(1.0 / source->GetWidth()), (float)(1.0 / source->GetHeight()), 1.0f, 1.0f);
      m_FBShader->SetTechnique(tech_h);
      DrawFullscreenTexturedQuad(m_FBShader);
   }
   {
      m_FBShader->SetTextureNull(ShaderUniform::tex_fb_filtered);
      SetRenderTarget(initial_rt->m_name + " VBlur", dest, false); // switch to output buffer for vertical phase of gaussian blur
      m_currentPass->m_singleLayerRendering = singleLayer; // We support blurring a single layer (for anaglyph defocusing)
      AddRenderTargetDependency(tmp);
      m_FBShader->SetTexture(ShaderUniform::tex_fb_filtered, tmp->GetColorSampler());
      m_FBShader->SetVector(ShaderUniform::w_h_height, (float)(1.0 / tmp->GetWidth()), (float)(1.0 / tmp->GetHeight()), 1.0f, 1.0f);
      m_FBShader->SetTechnique(tech_v);
      DrawFullscreenTexturedQuad(m_FBShader);
   }
   CopyRenderStates(false, initial_state);
   SetRenderTarget(initial_rt->m_name, initial_rt->m_rt, true);
   initial_rt->m_name += '-';
}


////////////////////////////////////////////////////////////////////

void ReportFatalError(const HRESULT hr, const char* file, const int line)
{
#if defined(ENABLE_BGFX)
   const string msg = std::format("Fatal Error {:#010X} in {}:{}", (unsigned int)hr, file, line);
#elif defined(ENABLE_OPENGL)
   const string msg = std::format("Fatal Error {:#010X} {} in {}:{}", (unsigned int)hr, glErrorToString(hr), file, line);
#elif defined(ENABLE_DX9)
   const string msg = std::format("Fatal Error {} ({:#010X}: {}) at {}:{}", DXGetErrorString(hr), (unsigned int)hr, DXGetErrorDescription(hr), file, line);
#endif
   ShowError(msg);
   assert(false);
   exit(-1);
}

void ReportError(const string& errorText, const HRESULT hr, const char* file, const int line)
{
#if defined(ENABLE_BGFX)
   const string msg = std::format("Error {:#010X} in {}:{}\n{}", (unsigned int)hr, file, line, errorText);
#elif defined(ENABLE_OPENGL)
   const string msg = std::format("Error {:#010X} {} in {}:{}\n{}", (unsigned int)hr, glErrorToString(hr), file, line, errorText);
#elif defined(ENABLE_DX9)
   const string msg = std::format("{} {} ({:#010X}: {}) at {}:{}", errorText, DXGetErrorString(hr), (unsigned int)hr, DXGetErrorDescription(hr), file, line);
#endif
   ShowError(msg);
}
