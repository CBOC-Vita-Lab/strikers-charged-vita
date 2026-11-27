#ifndef GAME_RENDER_STILLBACKDROP_H
#define GAME_RENDER_STILLBACKDROP_H

// MSC_MENU_STILL, MSC_PAUSE_STILL (np/stillback.h). Called around the frame's send: the first may hide views and change one, the second puts them back.
void StillBackdropBeforeSend();
void StillBackdropAfterSend();

#endif // GAME_RENDER_STILLBACKDROP_H
