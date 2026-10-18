// np_wrap_swap.cpp: wires the harness's frame hook into the game without touching game source.
//
// The game presents a frame in glplatSendFrame() (src/NL/gl/glPlat.cpp): glxSwapPost (finish the
// previous frame, wait for the retrace), the views' GX work, then glxSwapPre, which issues the
// frame's GXCopyDisp. The recompiled build counted a presented frame at that GXCopyDisp; here the
// call of glxSwapPre is wrapped by the linker (glxSwapPre is defined in glxSwap.cpp and called from
// glPlat.cpp, two translation units, so the linker's --wrap reaches it):
//
//     link flags:  -Wl,--wrap=_Z10glxSwapPreb         (void glxSwapPre(bool))
//
// Frames in which the game shows its loading indicator instead of swapping
// (glx_bLoadingIndicator != 0: glxSwapPre does nothing) are not counted, as before.
// If a build prefers to call NpOnFramePresented() itself (from the GX layer's GXCopyDisp), it
// leaves this file and the flag out.
extern "C" {
void NpOnFramePresented(void);
// weak: an executable linked without -Wl,--wrap=_Z10glxSwapPreb never calls __wrap_, and the archive member is
// then harmless instead of an undefined reference
void __real__Z10glxSwapPreb(bool send) __attribute__((weak));
extern unsigned char glx_bLoadingIndicator __attribute__((weak));

void __wrap__Z10glxSwapPreb(bool send) {
    if (&__real__Z10glxSwapPreb) __real__Z10glxSwapPreb(send);
    if (&glx_bLoadingIndicator && glx_bLoadingIndicator) return;
    NpOnFramePresented();
}
}

// The frame-send job (src/NL/gl/glPlat.cpp glplatSendFrameForked) runs swap_Pre on a second core and does not pass through the wrapped
// glxSwapPre: the game thread calls the frame hook itself, at the fork, where the serial order has it (same loading-indicator rule).
extern "C" void NpFramePresentedFromGame(void) {
    if (&glx_bLoadingIndicator && glx_bLoadingIndicator) return;
    NpOnFramePresented();
}
