// Vita ad hoc radio for the LAN link (vlink_vita.cpp, vlink.h): brings the PSP-compatible ad hoc network up through
// the system NetCheck dialog and moves whole link frames as PDP datagrams between MAC addresses. The same code runs in
// the recompiled port's Vita platform layer (vita repo, branch mp-one, src/vita_multiplayer.cpp; the radio state
// machine of the multiplayer import reduced to the raw link mode). The dialog is drawn by the renderer's swap: the
// Vita renderer calls np_vita_radio_dialog_render() before every display swap and passes np_vita_radio_dialog_active()
// to vglSwapBuffers (has_commondialog), and calls np_vita_radio_dialog_prepare(true) once its back end is ready.
// Owner thread = the thread that calls np_vita_radio_pump() (once per presented frame).
#ifndef NP_VLINK_VITA_RADIO_H
#define NP_VLINK_VITA_RADIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum { NP_RADIO_OK, NP_RADIO_AGAIN, NP_RADIO_CLOSED, NP_RADIO_LIMIT, NP_RADIO_INVALID, NP_RADIO_WRONG_PHASE } NpRadioStatus;
typedef struct { uint8_t bytes[6]; } NpRadioAddress;
typedef enum { NP_RADIO_DOWN, NP_RADIO_STARTING, NP_RADIO_DIALOG, NP_RADIO_OPEN, NP_RADIO_CLOSING } NpRadioPhase;
typedef struct {
    NpRadioPhase phase;
    NpRadioStatus status;
    const char *stage;
    uint32_t sce_error, rx_dropped;
    bool cleanup_stalled;
    uint32_t oversize;   // sends refused because the datagram was longer than SCE_NET_ADHOC_PDP_MFS (the link fragments, so this stays 0)
} NpRadioSnapshot;

#ifdef __cplusplus
extern "C" {
#endif
// Owner-thread operations (the game thread, from the presented-frame hook). Close is asynchronous: pump until DOWN.
// Both consoles use the same NetCheck mode (MSC_LAN_DIALOG=conn|create|join, default conn). Pump until OPEN.
NpRadioStatus np_vita_radio_begin_link(void);
// OPEN only. mac == NULL broadcasts (FF:FF:FF:FF:FF:FF plus every known peer).
int np_vita_radio_link_send(const uint8_t *mac, const void *bytes, size_t length);
// Non-blocking: frame length, 0 when nothing is waiting, <0 when the radio is not open.
int np_vita_radio_link_recv(uint8_t mac[6], void *bytes, size_t capacity);
bool np_vita_radio_link_mac(uint8_t mac[6]);
void np_vita_radio_pump(void);
void np_vita_radio_close(void);
void np_vita_radio_suspend(void);
void np_vita_radio_snapshot(NpRadioSnapshot *out);
void np_vita_radio_set_dialog_ready(bool ready);
// Renderer startup, only after its surfaces/pools and graphics checks succeeded. Returns 0 (an int, not NpRadioStatus:
// the renderer declares it weakly and has no enum) or a nonzero NpRadioStatus value.
int np_vita_radio_dialog_prepare(bool surface_ready);
// Renderer only: process one operation before its CommonDialog swap.
void np_vita_radio_dialog_render(void);
bool np_vita_radio_dialog_active(void);
#ifdef __cplusplus
}
#endif
#endif
