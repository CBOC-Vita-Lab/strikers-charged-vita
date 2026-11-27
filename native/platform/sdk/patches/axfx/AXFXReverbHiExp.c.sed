# 1.3 (MSC_AUDIO_NEON): the stereo reverb's NEON form (src/axfx_reverb_neon.c), after the reference's "active" test. It returns 0 when it did nothing, and the reference's loop runs.
/^void AXFXReverbHiExpCallback(AXFX_BUFFERUPDATE\* bufferUpdate,/i\
int NpAxfxReverbNeon(AXFX_BUFFERUPDATE* update, AXFX_REVERBHI_EXP* fx);
/^    input\[0\] = bufferUpdate->left;/i\
    if (NpAxfxReverbNeon(bufferUpdate, fx)) {\
        return;\
    }
