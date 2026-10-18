#ifndef NL_GLX_GLXSEND_H
#define NL_GLX_GLXSEND_H

class GLView;
struct glModelPacket;

void glx_SendFrame_cb(
    GLView* view, unsigned long flags, const glModelPacket* p);
void glx_SendEnd();
void glx_Fog(bool enable);
void glx_SetFogClipPlanes(float nearPlane, float farPlane);
float glx_GetFogStart();
void glx_SetFogStart(float value);
float glx_GetFogEnd();
void glx_SetFogEnd(float value);
#ifdef NP_NATIVE
void glx_FogAtFork();   // NP (two-stage pipeline): the fog the frame send uses is the fog as of the fork (NisPlayer, the layers move it while the send runs)
#endif

#endif // NL_GLX_GLXSEND_H
