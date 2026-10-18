# AXSetVoiceAddr packs the ADPCM gain and predictor/scale halfwords of a PCM voice into ONE 32-bit store
# (0x08000000: gain 0x0800 in the first halfword, as the big-endian console lays it out). On a
# little-endian host the first halfword is the LOW half of the word: NP_BE_HI16 puts the halfword where
# the console's layout has it (shim/decomp.h).
s/\*dst++ = 0x08000000;/*dst++ = NP_BE_HI16(0x0800);/
s/\*dst++ = 0x01000000;/*dst++ = NP_BE_HI16(0x0100);/
