#pragma once

// One-shot "ready" signal to the launcher that spawned us (go-vpx-launcher), over
// an fd it passed in via the VPX_READY_FD environment variable. If that variable is
// unset we were not launched by it, and both calls are no-ops.
//
// vpx_ready_signal_init() must run once, as early as possible at startup, before this
// process forks/execs any helper (B2S server, VPinMAME, PUP, ...), so the fd is not
// inherited by them: a leaked fd holds the launcher's pipe open and turns instant
// crash detection into a full timeout.
//
// vpx_ready_signal_fire() must run exactly once, immediately after the loading screen's
// frame has actually been presented (i.e. after Flip()/swap returns), and before the
// renderer is torn down or re-initialized for the real game session. It is safe to call
// on every loading-screen frame; only the first call does anything.

#ifdef __RK3588__

void vpx_ready_signal_init();
void vpx_ready_signal_fire();

#else

inline void vpx_ready_signal_init() { }
inline void vpx_ready_signal_fire() { }

#endif
