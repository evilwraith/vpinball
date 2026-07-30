#include "core/stdafx.h"
#include "vpx_ready_signal.h"

#ifdef __RK3588__

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>

namespace
{
int g_readyFD = -1;
std::atomic<bool> g_fired { false };
}

void vpx_ready_signal_init()
{
   const char *const fdStr = getenv("VPX_READY_FD");
   if (!fdStr || !*fdStr)
   {
      g_readyFD = -1; // no launcher waiting on us
      return;
   }
   g_readyFD = atoi(fdStr);

   // Close this fd across any fork/exec of a helper process. If it leaked and we then
   // crashed before firing, the launcher's pipe would stay open via that helper and it
   // would sit at its full timeout instead of seeing EOF and failing fast.
   const int flags = fcntl(g_readyFD, F_GETFD);
   if (flags != -1)
      fcntl(g_readyFD, F_SETFD, flags | FD_CLOEXEC);

   PLOGI.printf("Ready signal: launcher fd %d", g_readyFD);
}

void vpx_ready_signal_fire()
{
   if (g_readyFD < 0)
      return;
   if (g_fired.exchange(true))
      return; // already fired; a second call is a no-op

   const char b = 1;
   ssize_t n;
   do
   {
      n = write(g_readyFD, &b, 1);
   } while (n < 0 && errno == EINTR);
   // 1-byte writes to a pipe are atomic (POSIX PIPE_BUF), so there is no partial write to
   // handle. A write error is not actionable (the launcher may simply be gone), so it is
   // reported and ignored.
   if (n < 0)
      PLOGW.printf("Ready signal: write to fd %d failed: %s", g_readyFD, strerror(errno));
   else
      PLOGI << "Ready signal: fired";

   close(g_readyFD);
   g_readyFD = -1;
}

#endif
