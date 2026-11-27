#ifndef NL_GL_GLFONT_H
#define NL_GL_GLFONT_H

#include "NL/gl/gl.h"
#include "NL/nlColour.h"

class GLView;

void gl_FontStartup();
void glFontBegin(bool drop);
void glFontEnd();
void glFontVirtualPosToScreenCoordPos(float x, float y, float& outX, float& outY);
bool glFontVirtualCoordinates(bool virtualCoordinates);
int glFontSetFont(int font);
void glFontBlitCharacter(int x, int y, char character, unsigned short* image,
    int imageWidth, int font);
int glFontGetScreenHeight(GLView* renderView);

int glFontPrint(void* renderView, eGLView view, int x, int y, const nlColour& colour, const char* str);
int glFontPrint(void* renderView, eGLView view, int x, int y, const char* str);
int glFontPrintf(void* renderView, int x, int y, const char* format, ...) __attribute__((format(printf, 4, 5)));
int glFontPrintf(void* renderView, int x, int y, const nlColour& colour, const char* format, ...) __attribute__((format(printf, 5, 6)));
int glFontPrintf(void* renderView, eGLView view, int x, int y, const nlColour& colour, const char* format, ...) __attribute__((format(printf, 6, 7)));

#endif // NL_GL_GLFONT_H
