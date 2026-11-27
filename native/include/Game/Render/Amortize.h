#ifndef GAME_RENDER_AMORTIZE_H
#define GAME_RENDER_AMORTIZE_H

class ParticleSystem;

// np/amortize.h has the switches.
void AmortizeCreateTargets();                        // once, when the views are made
void AmortizeHazeBeginFrame();                       // before the frame's particles are attached
class GLView;
bool AmortizeHazeRoute(const ParticleSystem* system, GLView** view);   // false: not drawn this frame
int AmortizeSixty();                                 // 1: MSC_AMORT_60 acts on this frame
int AmortizeCrowdSixty();                            // 1: the crowd sprites keep their rate in time at 60 (MSC_AMORT_60 or MSC_CROWD_LIVE)
void AmortizeBeforeSend();
void AmortizeAfterSend();

#endif // GAME_RENDER_AMORTIZE_H
