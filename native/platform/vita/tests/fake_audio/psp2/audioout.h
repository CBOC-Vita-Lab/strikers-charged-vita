// A stand-in for <psp2/audioout.h> so that np_vita_audio.cpp's console output thread can run on the host (test_audio_out.cpp).
#pragma once
#include <stdint.h>
#define SCE_AUDIO_OUT_PORT_TYPE_BGM 1
#define SCE_AUDIO_OUT_MODE_STEREO 1
#define SCE_AUDIO_VOLUME_FLAG_L_CH 1
#define SCE_AUDIO_VOLUME_FLAG_R_CH 2
typedef int SceAudioOutChannelFlag;
#ifdef __cplusplus
extern "C" {
#endif
int sceAudioOutOpenPort(int type, int len, int freq, int mode);
int sceAudioOutReleasePort(int port);
int sceAudioOutOutput(int port, const void* buf);
int sceAudioOutSetVolume(int port, SceAudioOutChannelFlag flags, int* vol);
#ifdef __cplusplus
}
#endif
