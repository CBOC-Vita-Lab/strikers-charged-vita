/* np/latency.h (W5 RT): the time from the pad read a game frame used to the display refresh that first shows that frame.
 *
 * A frame is followed through the stages of the build it runs in; every stage stamps it with np_time_ns() (virtual in the harness's
 * MSC_VIRTUAL_TIME runs, real everywhere else):
 *   pad        the raw pad read behind the sample the frame's first pad request got (SDK input layer, channel 0; the layer keeps one
 *              sample per retrace, see input_native.cpp sample_epoch)
 *   handover   the game thread hands the frame to the frame send (glxSwap.cpp: glxSwapPreFork in the forked send, swap_Pre in the serial one)
 *   decoded    the renderer's player has decoded the frame (vgxn_end_frame returned: rec::Present waits for that)
 *   presented  the GL back end reached the frame's display copy and swaps (Vita host PresentDisplayCopy)
 *   handed     vitaGL's display-queue thread calls back with the finished buffer (the GPU is done with it; Vita host DisplayCallback)
 *   shown      the vblank at which sceDisplaySetFrameBuf(NEXTFRAME) puts it on the screen (the display gate's vcount, timed from the
 *              last vblank the VI thread saw)
 * The record of a frame travels as a tag: the game thread latches it at the hand-over, the thread that sends the frame takes it at
 * vgxn_end_frame (NpLatTakeSendTag) and the Vita host carries it, keyed by the renderer's frame number, to the present and the display.
 * Statistics: an [lat] line every MSC_LAT_LOG (default 300) shown frames with p50/p95 of the whole delay and of each stage; the
 * [frame-timing] lines carry lat_n / lat_p50_us / lat_p95_us of the frames shown in their window (NpLatTakeWindow). MSC_LAT=0 turns it off.
 * The host builds have no display: there the end is the decode (the [lat] line says so).
 */
#ifndef NP_LATENCY_H
#define NP_LATENCY_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void NpLatPadUse(uint64_t rawReadNs, int chan);   /* SDK input layer: a pad request was answered from the raw read made at rawReadNs */
void NpLatLatch(void);                            /* game thread, at the hand-over of a frame */
uint64_t NpLatTakeSendTag(void);                  /* the thread that sends the frame, at its display copy: the frame's tag (0: none) */
void NpLatDecoded(uint64_t tag);                  /* the renderer has decoded the frame */
void NpLatPresented(uint64_t tag);                /* the back end reached its display copy */
void NpLatShown(uint64_t tag, uint64_t shownNs, int handedToo);   /* the frame appears at shownNs (handedToo: stamp "handed" now) */
void NpLatTakeWindow(uint32_t* n, uint32_t* p50Us, uint32_t* p95Us);   /* frames shown since the last call: count and percentiles of the delay */
#ifdef __cplusplus
}
#endif
#endif
