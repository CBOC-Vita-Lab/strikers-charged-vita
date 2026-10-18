// Workstream AUD (lever L6): members of AXWii for the fast mix path, the two-pass verification
// and the census. Included inside the class in ax_mix.cpp; the kernels are in ax_mix_fast.h.
//
// What the fast path does differently from the reference, and why the result is the same:
//   1. Stopped voices are skipped after reading two fields of the parameter block (running and
//      the link). The reference reads the whole block, finds running != 1, mixes nothing and
//      writes the same bytes back: no observable difference.
//   2. A running voice's block is converted with one byte-swapping copy each way instead of a
//      checked accessor per halfword. Same bytes.
//   3. A frame's input samples are decoded up front (ADPCM: AxFast::DecodeAdpcm; other formats:
//      the reference's own Accelerator, called in a row) into a history buffer; the converter
//      then runs over that buffer (ax_mix_fast.h). The number of input samples a frame takes
//      depends on position and ratio only, so the order of work is the only thing that changes.
//   4. Envelope and bus mixes use the Q15 kernels; unit envelope (0x8000, not moving) is
//      skipped (x * 0x8000 >> 15 == x); a bus at volume 0 that is not moving adds zero and is
//      skipped (its de-pop value still becomes 0, as in the reference).
//   5. A voice whose envelope is 0 and not moving, with no filter state and no Wii Remote part,
//      contributes zero to every bus: only its decoder state, position, last samples, de-pop
//      values and moving bus volumes are advanced; the converter and the mixes are skipped.
//   6. Aux return and output use a constant gain when the gain did not change since the last
//      frame (the reference builds a 96-entry table that is then constant).
// Anything unusual falls back to the reference code for that voice: a conversion ratio above 8,
// a coefficient bank that fails the 32-bit bound, a parameter block that does not resolve to
// host memory in one piece, an undetected block layout, the old command layout.

static constexpr uint32_t kFastMaxRatio = 0x00080000u;            // 8.0 (AX itself allows up to 4.0)
static constexpr uint32_t kFastMaxInput = (0xFFFFu + kAxSamplesPerFrame * kFastMaxRatio) >> 16;

// ---- the twelve 32 kHz buses of a voice, in the reference's mixing order ----------------------
struct FastBus {
    uint32_t bit;
    uint32_t rampBit;
    AxBus bus;
    VolumeData PBMixerWii::* volume;
    int16_t PBDpopWii::* dpop;
};
static constexpr FastBus kFastBuses[12] = {
    {MIX_MAIN_L, MIX_MAIN_L_RAMP, kBusMainL, &PBMixerWii::main_left, &PBDpopWii::main_left},
    {MIX_MAIN_R, MIX_MAIN_R_RAMP, kBusMainR, &PBMixerWii::main_right, &PBDpopWii::main_right},
    {MIX_MAIN_S, MIX_MAIN_S_RAMP, kBusMainS, &PBMixerWii::main_surround, &PBDpopWii::main_surround},
    {MIX_AUXA_L, MIX_AUXA_L_RAMP, kBusAuxAL, &PBMixerWii::auxA_left, &PBDpopWii::auxA_left},
    {MIX_AUXA_R, MIX_AUXA_R_RAMP, kBusAuxAR, &PBMixerWii::auxA_right, &PBDpopWii::auxA_right},
    {MIX_AUXA_S, MIX_AUXA_S_RAMP, kBusAuxAS, &PBMixerWii::auxA_surround, &PBDpopWii::auxA_surround},
    {MIX_AUXB_L, MIX_AUXB_L_RAMP, kBusAuxBL, &PBMixerWii::auxB_left, &PBDpopWii::auxB_left},
    {MIX_AUXB_R, MIX_AUXB_R_RAMP, kBusAuxBR, &PBMixerWii::auxB_right, &PBDpopWii::auxB_right},
    {MIX_AUXB_S, MIX_AUXB_S_RAMP, kBusAuxBS, &PBMixerWii::auxB_surround, &PBDpopWii::auxB_surround},
    {MIX_AUXC_L, MIX_AUXC_L_RAMP, kBusAuxCL, &PBMixerWii::auxC_left, &PBDpopWii::auxC_left},
    {MIX_AUXC_R, MIX_AUXC_R_RAMP, kBusAuxCR, &PBMixerWii::auxC_right, &PBDpopWii::auxC_right},
    {MIX_AUXC_S, MIX_AUXC_S_RAMP, kBusAuxCS, &PBMixerWii::auxC_surround, &PBDpopWii::auxC_surround},
};

// ---- census (MSC_AX_STATS=1) ------------------------------------------------------------------
void CensusPB(const AXPBWii& pb, AXMixControl control, bool newFilter) {
    AxStats& st = g_axStats;
    ++st.pbs;
    if (pb.running != 1) {
        return;
    }
    ++st.running;
    const uint16_t format = pb.audio_addr.sample_format;
    if ((format & 0xF) == 0) ++st.fmtAdpcm;
    else if (format == 0x0a) ++st.fmtPcm16;
    else if (format == 0x19) ++st.fmtPcm8;
    else ++st.fmtOther;
    const uint32_t ratio = Hilo(pb.src.ratio_hi, pb.src.ratio_lo);
    const bool table = VoiceCoefficients(BaseCoefficients(), pb.coef_select) != nullptr;
    if (pb.src_type == 0 && table) {
        ++st.src[0];
        ++st.coefSel[pb.coef_select & 3];
    } else if (pb.src_type == 0 || pb.src_type == 1) {
        ++st.src[1];
        if (pb.src_type == 0) ++st.srcNoTable;
    } else {
        ++st.src[2];
    }
    if (ratio <= 0x10000u) ++st.ratioLe1;
    else if (ratio <= 0x20000u) ++st.ratioLe2;
    else if (ratio <= 0x40000u) ++st.ratioLe4;
    else ++st.ratioMore;
    if (pb.src_type == 0 || pb.src_type == 1) {
        if (ratio <= kFastMaxRatio) st.inputSamples += AxFast::SrcInputCount(pb.src.cur_addr_frac, ratio);
    } else {
        st.inputSamples += kAxSamplesPerFrame;
    }
    const uint16_t envVol = static_cast<uint16_t>(pb.vol_env.cur_volume);
    const uint16_t envDelta = static_cast<uint16_t>(pb.vol_env.cur_volume_delta);
    if (envDelta != 0) ++st.envRamp;
    else if (envVol == 0x8000u) ++st.envUnity;
    else if (envVol == 0) ++st.envZero;
    else ++st.envConst;
    if (pb.lpf.on) ++st.lpf;
    if (newFilter && pb.biquad.on) ++st.biquad;
    if (pb.remote) ++st.remote;
    if ((pb.updates.num_updates[0] | pb.updates.num_updates[1] | pb.updates.num_updates[2]) != 0) ++st.updates;
    uint32_t buses = 0;
    for (const FastBus& bus : kFastBuses) {
        if ((control & bus.bit) == 0) {
            continue;
        }
        ++buses;
        ++st.busMixes;
        const VolumeData& vd = pb.mixer.*(bus.volume);
        const bool rampBit = (control & bus.rampBit) != 0;
        if (rampBit) ++st.busRampBit;
        if (rampBit && vd.volume_delta != 0) ++st.busRampMoving;
        else if (vd.volume == 0) ++st.busZeroVolume;
        else if (vd.volume == 0x8000u) ++st.busUnity;
    }
    ++st.busesPerVoice[buses > 12 ? 12 : buses];
}

// ---- parameter block <-> guest memory, whole segments at a time -------------------------------
static void LoadPBFast(const uint8_t* guest, AXPBWii& pb, const std::array<PBSegment, 3>& segments,
                       size_t segmentCount) {
    uint8_t* dst = reinterpret_cast<uint8_t*>(&pb);
    size_t structPos = 0;
    for (size_t i = 0; i < segmentCount; ++i) {
        const PBSegment& seg = segments[i];
        if (seg.structOffset > structPos) {
            // A field the guest layout does not have: zero, as the reference's memset leaves it.
            std::memset(dst + structPos, 0, seg.structOffset - structPos);
        }
        AxFast::SwapCopy16(dst + seg.structOffset, guest + seg.guestOffset, seg.byteCount);
        structPos = seg.structOffset + seg.byteCount;
    }
    if (structPos < sizeof(AXPBWii)) {
        std::memset(dst + structPos, 0, sizeof(AXPBWii) - structPos);
    }
}

static void StorePBFast(uint8_t* guest, const AXPBWii& pb, const std::array<PBSegment, 3>& segments,
                        size_t segmentCount) {
    const uint8_t* src = reinterpret_cast<const uint8_t*>(&pb);
    for (size_t i = 0; i < segmentCount; ++i) {
        const PBSegment& seg = segments[i];
        AxFast::SwapCopy16(guest + seg.guestOffset, src + seg.structOffset, seg.byteCount);
    }
}

// ---- the chain of parameter blocks ------------------------------------------------------------
AX_NOINLINE void ProcessPBListFast(uint32_t pbAddr, bool newFilter) {
    const AXBuffers buffers = MixBuffers();
    const int16_t* const coeffs = BaseCoefficients();
    const bool stats = g_axMode.stats && !m_statsMuted;
    std::array<PBSegment, 3> segments{};
    size_t segmentCount = 0;
    size_t guestBytes = 0;
    PBLayout layout = PBLayout::Unknown;
    uint32_t guard = 0;
    while (pbAddr && guard++ < 256) {
        if (layout != m_pbLayout) {
            layout = m_pbLayout;
            segmentCount = PBSegmentsForLayout(layout, segments);
            guestBytes = segments[segmentCount - 1].guestOffset + segments[segmentCount - 1].byteCount;
        }
        uint8_t* const guest = layout != PBLayout::Unknown ? MixResolveRange(pbAddr, guestBytes) : nullptr;
        if (guest == nullptr) {
            if (g_axMode.stats) {
                ++g_axStats.fallbackResolve;
            }
            pbAddr = ProcessOnePB(pbAddr, newFilter, false);
            continue;
        }
        // running and the link are in the first segment of every layout, at their struct offsets.
        if (BigEndian::Read16(guest + offsetof(AXPBWii, running)) != 1) {
            if (stats) {
                ++g_axStats.pbs;
            }
            if (g_axMode.stats) {
                ++g_axStats.stoppedSkipped;
            }
            pbAddr = Hilo(BigEndian::Read16(guest + offsetof(AXPBWii, next_pb_hi)),
                          BigEndian::Read16(guest + offsetof(AXPBWii, next_pb_lo)));
            continue;
        }
        AXPBWii pb;
        LoadPBFast(guest, pb, segments, segmentCount);
        const AXMixControl control = ConvertMixerControl(Hilo(pb.mixer_control_hi, pb.mixer_control_lo));
        if (stats) {
            CensusPB(pb, control, newFilter);
        }
        ProcessVoiceFast(pb, buffers, control, coeffs, newFilter);
        MixNoteWrite(guest, guestBytes);
        StorePBFast(guest, pb, segments, segmentCount);
        pbAddr = Hilo(pb.next_pb_hi, pb.next_pb_lo);
    }
}

// One bus of a voice. `silent`: the voice's frame is all zero and `samples` was not produced.
static void MixBusFast(int32_t* out, const int16_t* samples, bool silent, VolumeData& vd, int16_t& dpop,
                       bool ramp) {
    const uint16_t delta = ramp ? vd.volume_delta : 0;
    const uint16_t volume = vd.volume;
    if (delta == 0) {
        if (silent || volume == 0) {
            dpop = 0; // every scaled sample is 0: the bus is unchanged, the last sample is 0
            return;
        }
        AxFast::MixAddConst96(out, samples, volume);
        dpop = AxFast::ScaleQ15C(samples[kAxSamplesPerFrame - 1], volume);
        return;
    }
    if (silent) {
        dpop = 0;
        vd.volume = static_cast<uint16_t>(volume + kAxSamplesPerFrame * delta);
        return;
    }
    vd.volume = AxFast::MixAddRamp96(out, samples, volume, delta);
    dpop = AxFast::ScaleQ15C(samples[kAxSamplesPerFrame - 1],
                             static_cast<uint16_t>(volume + (kAxSamplesPerFrame - 1) * delta));
}

// ---- one running voice, one 96-sample frame ---------------------------------------------------
AX_NOINLINE void ProcessVoiceFast(AXPBWii& pb, const AXBuffers& buffers, AXMixControl control,
                                  const int16_t* coeffsBase, bool newFilter) {
    const uint32_t ratio = Hilo(pb.src.ratio_hi, pb.src.ratio_lo);
    const uint16_t srcType = pb.src_type;
    const int16_t* const coeffs = VoiceCoefficients(coeffsBase, pb.coef_select);
    enum class Src { Poly, Linear, Nearest };
    const Src src = (coeffs != nullptr && srcType == 0) ? Src::Poly
                    : ((srcType != 0 && srcType != 1) ? Src::Nearest : Src::Linear);
    const bool tableOk = src != Src::Poly || m_coefBankFits32[pb.coef_select & 3];
    if (ratio > kFastMaxRatio || !tableOk) {
        if (g_axMode.stats) {
            ++(ratio > kFastMaxRatio ? g_axStats.fallbackRatio : g_axStats.fallbackTable);
            ++g_axStats.referenceVoices;
        }
        if (g_axMode.logFallback) {
            static int logged = 0;
            if (logged < 40) {
                ++logged;
                RT_LOGF(RT_TAG_AUDIO,
                        "reference voice %d: ratio=%08x (%.3f) src_type=%u coef=%u fmt=%04x looping=%u cur=%08x loop=%08x end=%08x env=%04x/%04x pb=%08x\n",
                        logged, ratio, ratio / 65536.0, srcType, pb.coef_select, pb.audio_addr.sample_format, pb.audio_addr.looping,
                        Hilo(pb.audio_addr.cur_addr_hi, pb.audio_addr.cur_addr_lo),
                        Hilo(pb.audio_addr.loop_addr_hi, pb.audio_addr.loop_addr_lo),
                        Hilo(pb.audio_addr.end_addr_hi, pb.audio_addr.end_addr_lo), static_cast<uint16_t>(pb.vol_env.cur_volume),
                        static_cast<uint16_t>(pb.vol_env.cur_volume_delta), Hilo(pb.this_pb_hi, pb.this_pb_lo));
                std::fflush(stderr);
            }
        }
        ProcessVoice(pb, buffers, kAxSamplesPerFrame, control, coeffsBase, newFilter);
        return;
    }
    if (g_axMode.stats) {
        ++g_axStats.fastVoices;
    }

    // 1. This frame's input, after the four samples carried over from the last frame.
    const uint32_t pos0 = pb.src.cur_addr_frac;
    const uint32_t inputCount = src == Src::Nearest ? kAxSamplesPerFrame : AxFast::SrcInputCount(pos0, ratio);
    int16_t hist[4 + kFastMaxInput + 4];
    std::memcpy(hist, pb.src.last_samples, 4 * sizeof(int16_t));
    if ((pb.audio_addr.sample_format & 0xF) == 0) {
        AxFast::DecodeAdpcm(pb, hist + 4, inputCount);
    } else {
        Accelerator accel;
        accel.Setup(&pb);
        for (uint32_t i = 0; i < inputCount; ++i) {
            hist[4 + i] = accel.ReadSample();
        }
    }

    // 2. Is the whole frame zero whatever the input? (Envelope 0 and not moving, filters at rest;
    //    a voice with a Wii Remote part always takes the full path.)
    const uint16_t envVolume = static_cast<uint16_t>(pb.vol_env.cur_volume);
    const uint16_t envDelta = static_cast<uint16_t>(pb.vol_env.cur_volume_delta);
    const bool lpfActive = pb.lpf.on != 0;
    const bool biquadActive = newFilter && pb.biquad.on != 0;
    const bool silent = envDelta == 0 && envVolume == 0 && pb.remote == 0 &&
                        !(lpfActive && pb.lpf.yn1 != 0) &&
                        !(biquadActive && (pb.biquad.xn1 | pb.biquad.xn2 | pb.biquad.yn1 | pb.biquad.yn2) != 0);
    if (silent && g_axMode.stats) {
        ++g_axStats.silentVoices;
    }

    // 3. Sample-rate conversion to 96 samples; position and carried-over samples for the next frame.
    alignas(16) int16_t samples[kAxSamplesPerFrame];
    if (src == Src::Nearest) {
        if (!silent) {
            std::memcpy(samples, hist + 4, sizeof(samples));
        }
        std::memcpy(pb.src.last_samples, hist + 4 + kAxSamplesPerFrame - 4, 4 * sizeof(int16_t));
        // (The reference returns the position unchanged in this mode.)
    } else {
        uint32_t pos;
        if (silent) {
            pos = (pos0 + kAxSamplesPerFrame * ratio) & 0xffffu;
        } else if (src == Src::Poly) {
            pos = AxFast::ResamplePoly96(samples, hist, pos0, ratio, coeffs);
        } else {
            pos = AxFast::ResampleLinear96(samples, hist, pos0, ratio);
        }
        std::memcpy(pb.src.last_samples, hist + inputCount, 4 * sizeof(int16_t));
        pb.src.cur_addr_frac = static_cast<uint16_t>(pos);
    }

    // 4. Envelope, filters.
    if (!silent) {
        if (!(envDelta == 0 && envVolume == 0x8000u)) {
            pb.vol_env.cur_volume = static_cast<int16_t>(AxFast::ScaleEnv96(samples, envVolume, envDelta));
        }
        if (lpfActive) {
            LowPassFilter(samples, kAxSamplesPerFrame, pb.lpf);
        }
        if (biquadActive) {
            BiquadFilter(samples, kAxSamplesPerFrame, pb.biquad);
        }
    }

    // 5. Buses.
    for (const FastBus& bus : kFastBuses) {
        if ((control & bus.bit) != 0) {
            MixBusFast(buffers[bus.bus], samples, silent, pb.mixer.*(bus.volume), pb.dpop.*(bus.dpop),
                       (control & bus.rampBit) != 0);
        }
    }
    if (pb.remote) {
        ProcessVoiceRemote(pb, samples, kAxSamplesPerFrame, buffers, coeffsBase, newFilter);
    }
}

// ---- the per-frame commands around the voices -------------------------------------------------
// SETUP: the 20 buses' start values, the 120-byte block resolved once instead of per halfword.
AX_NOINLINE void SetupProcessingFast(uint32_t initAddr) {
    const AXBuffers buffers = MixBuffers();
    const uint8_t* const init = MixResolveRange(initAddr, buffers.size() * 6);
    if (init == nullptr) {
        SetupProcessing(initAddr);
        return;
    }
    for (size_t i = 0; i < buffers.size(); ++i) {
        const uint32_t value = BigEndian::Read32(init + i * 6);
        const int16_t delta = static_cast<int16_t>(BigEndian::Read16(init + i * 6 + 4));
        const int count = (i < kAxRegularBusCount ? 32 : 6) * 3;
        if (value == 0) {
            std::memset(buffers[i], 0, static_cast<size_t>(count) * sizeof(int));
        } else {
            const int32_t start = static_cast<int32_t>(value);
            for (int j = 0; j < count; ++j) {
                buffers[i][j] = start + j * delta;
            }
        }
    }
}

// ADD_TO_LR / SUB_TO_LR: the guest frame converted in one go.
AX_NOINLINE void AddToLRFast(uint32_t addr, bool neg) {
    const uint8_t* const host = MixResolveRange(addr, kAxSamplesPerFrame * sizeof(int32_t));
    if (host == nullptr) {
        AddToLR(addr, neg);
        return;
    }
    alignas(16) int32_t frame[kAxSamplesPerFrame];
    AxFast::LoadBigEndian32(frame, host, kAxSamplesPerFrame);
    AxFast::AddToTwo96(m_bus[kBusMainL].data(), m_bus[kBusMainR].data(), frame, neg);
}

// WM_OUTPUT: four 18-sample buses, each 36-byte guest buffer resolved once.
AX_NOINLINE void OutputWMSamplesFast(uint32_t* addresses) {
    const AXBuffers all = MixBuffers();
    const std::array<int*, 4> buffers = {all[kBusWm0Main], all[kBusWm1Main], all[kBusWm2Main], all[kBusWm3Main]};
    for (uint32_t i = 0; i < buffers.size(); ++i) {
        uint8_t* const host = MixResolveRange(addresses[i], kAxWiimoteSamplesPerFrame * sizeof(int16_t));
        if (host == nullptr) {
            for (uint32_t j = 0; j < kAxWiimoteSamplesPerFrame; ++j) {
                MixWrite16(addresses[i] + j * 2, static_cast<uint16_t>(ClampS16(buffers[i][j])));
            }
            continue;
        }
        MixNoteWrite(host, kAxWiimoteSamplesPerFrame * sizeof(int16_t));
        for (uint32_t j = 0; j < kAxWiimoteSamplesPerFrame; ++j) {
            BigEndian::Write16(host + j * 2, static_cast<uint16_t>(AxMixKernels::ClampToS16(buffers[i][j])));
        }
    }
}

// ---- aux return and output with a constant gain where the gain did not move -------------------
AX_NOINLINE void MixAUXSamplesFast(uint32_t auxId, uint32_t writeAddr, uint32_t readAddr, uint16_t volume) {
    const uint16_t from = m_lastAuxVolumes[auxId];
    const bool constant = from == volume; // GenerateVolumeRamp(from, from) is `from` 96 times
    std::array<uint16_t, kAxSamplesPerFrame> ramp;
    if (!constant) {
        GenerateVolumeRamp(ramp.data(), from, volume, ramp.size());
    }
    if (g_axMode.stats) {
        ++(constant ? (volume == 0x8000u ? g_axStats.auxUnity : g_axStats.auxConst) : g_axStats.auxRamp);
    }
    m_lastAuxVolumes[auxId] = volume;
    std::array<int*, 3> src{};
    if (auxId == 0) src = {m_bus[kBusAuxAL].data(), m_bus[kBusAuxAR].data(), m_bus[kBusAuxAS].data()};
    if (auxId == 1) src = {m_bus[kBusAuxBL].data(), m_bus[kBusAuxBR].data(), m_bus[kBusAuxBS].data()};
    if (auxId == 2) src = {m_bus[kBusAuxCL].data(), m_bus[kBusAuxCR].data(), m_bus[kBusAuxCS].data()};
    if (writeAddr) {
        for (int* buffer : src) {
            WriteAuxOutBuffer(writeAddr, buffer, kAxSamplesPerFrame);
            writeAddr += kAxSamplesPerFrame * sizeof(int32_t);
        }
    }
    std::array<int*, 3> dst = {m_bus[kBusMainL].data(), m_bus[kBusMainR].data(), m_bus[kBusMainS].data()};
    alignas(16) std::array<int, kAxSamplesPerFrame> aux;
    for (int* buffer : dst) {
        ReadAuxInBuffer(readAddr, aux.data(), kAxSamplesPerFrame);
        readAddr += kAxSamplesPerFrame * sizeof(int32_t);
        if (constant) {
            AxFast::AccumGain96(buffer, aux.data(), volume);
        } else {
            AxFast::AccumRamp96(buffer, aux.data(), ramp.data());
        }
    }
}

AX_NOINLINE void UploadAUXMixLRSCFast(int auxId, uint32_t* addresses, uint16_t volume) {
    int* auxLeft = auxId ? m_bus[kBusAuxBL].data() : m_bus[kBusAuxAL].data();
    int* auxRight = auxId ? m_bus[kBusAuxBR].data() : m_bus[kBusAuxAR].data();
    int* auxSurround = auxId ? m_bus[kBusAuxBS].data() : m_bus[kBusAuxAS].data();
    int* auxCBuffer = auxId ? m_bus[kBusAuxCS].data() : m_bus[kBusAuxCR].data();
    WriteAuxOutBuffer(addresses[0], auxLeft, kAxSamplesPerFrame);
    WriteAuxOutBuffer(addresses[0] + kAxSamplesPerFrame * sizeof(int32_t), auxRight, kAxSamplesPerFrame);
    WriteAuxOutBuffer(addresses[0] + 2 * kAxSamplesPerFrame * sizeof(int32_t), auxSurround, kAxSamplesPerFrame);
    WriteAuxOutBuffer(addresses[1], auxCBuffer, kAxSamplesPerFrame);
    const uint16_t from = m_lastAuxVolumes[auxId];
    const bool constant = from == volume;
    std::array<uint16_t, kAxSamplesPerFrame> ramp;
    if (!constant) {
        GenerateVolumeRamp(ramp.data(), from, volume, ramp.size());
    }
    if (g_axMode.stats) {
        ++(constant ? (volume == 0x8000u ? g_axStats.auxUnity : g_axStats.auxConst) : g_axStats.auxRamp);
    }
    m_lastAuxVolumes[auxId] = volume;
    std::array<int*, 4> mixDest = {m_bus[kBusMainL].data(), m_bus[kBusMainR].data(),
                                   m_bus[kBusMainS].data(), m_bus[kBusAuxCL].data()};
    alignas(16) std::array<int, kAxSamplesPerFrame> temp;
    for (uint32_t i = 0; i < mixDest.size(); ++i) {
        ReadAuxInBuffer(addresses[2 + i], temp.data(), kAxSamplesPerFrame);
        if (constant) {
            AxFast::AccumGain96(mixDest[i], temp.data(), volume);
        } else {
            AxFast::AccumRamp96(mixDest[i], temp.data(), ramp.data());
        }
    }
}

AX_NOINLINE void OutputSamplesFast(uint32_t lrAddr, uint32_t surroundAddr, uint16_t volume, bool uploadAuxC) {
    const uint16_t from = m_lastMainVolume;
    const bool constant = from == volume;
    std::array<uint16_t, kAxSamplesPerFrame> ramp;
    if (!constant) {
        GenerateVolumeRamp(ramp.data(), from, volume, ramp.size());
    }
    if (g_axMode.stats) {
        ++(constant ? (volume == 0x8000u ? g_axStats.outUnity : g_axStats.outConst) : g_axStats.outRamp);
    }
    m_lastMainVolume = volume;
    WriteGuestS32Buffer(surroundAddr, m_bus[kBusMainS].data(), kAxSamplesPerFrame);
    if (uploadAuxC) {
        WriteGuestS32Buffer(surroundAddr + kAxSamplesPerFrame * sizeof(int32_t),
                            m_bus[kBusAuxCL].data(), kAxSamplesPerFrame);
    }
    constexpr size_t kPcmBytes = kAxSamplesPerFrame * 2 * sizeof(int16_t);
    const uint16_t* const table = constant ? nullptr : ramp.data();
    if (uint8_t* host = MixResolveRange(lrAddr, kPcmBytes)) {
        MixNoteWrite(host, kPcmBytes);
        AxFast::Output96(m_bus[kBusMainL].data(), m_bus[kBusMainR].data(), table, volume, host);
        return;
    }
    // Not host memory in one piece: the reference's word-by-word route for the same values.
    uint8_t pcm[kPcmBytes];
    AxFast::Output96(m_bus[kBusMainL].data(), m_bus[kBusMainR].data(), table, volume, pcm);
    for (uint32_t i = 0; i < kAxSamplesPerFrame * 2; ++i) {
        MixWrite16(lrAddr + i * 2, BigEndian::Read16(pcm + i * 2));
    }
}

// ---- MSC_AX_VERIFY: both paths on the same command list, everything compared -----------------
struct MixState {
    std::array<std::array<int, kAxSamplesPerFrame>, kAxRegularBusCount> bus;
    std::array<std::array<int, kAxWiimoteSamplesPerFrame>, kAxWiimoteBusCount> wmBus;
    std::array<AuxBlock, 24> auxOut;
    std::array<uint16_t, 3> lastAuxVolumes;
    uint16_t lastMainVolume;
    uint32_t compressorPos;
    uint32_t auxOutCount;
    PBLayout pbLayout;
};
struct VerifyScratch {
    MixState start, refEnd, fastEnd;
    MixJournal ref, fast;
    std::vector<uint8_t> expected;
    uint32_t logged = 0;
};
std::unique_ptr<VerifyScratch> m_verify;

void SaveMixState(MixState& s) const {
    s.bus = m_bus;
    s.wmBus = m_wmBus;
    s.auxOut = m_auxOut;
    s.lastAuxVolumes = m_lastAuxVolumes;
    s.lastMainVolume = m_lastMainVolume;
    s.compressorPos = m_compressorPos;
    s.auxOutCount = m_auxOutCount;
    s.pbLayout = m_pbLayout;
}

void LoadMixState(const MixState& s) {
    m_bus = s.bus;
    m_wmBus = s.wmBus;
    m_auxOut = s.auxOut;
    m_lastAuxVolumes = s.lastAuxVolumes;
    m_lastMainVolume = s.lastMainVolume;
    m_compressorPos = s.compressorPos;
    m_auxOutCount = s.auxOutCount;
    m_pbLayout = s.pbLayout;
}

// A host pointer of the mix for the log.
static void DescribeHost(const uint8_t* host, char* text, size_t size) {
    std::snprintf(text, size, "host %p", static_cast<const void*>(host));
}

bool VerifyLog(VerifyScratch& v) {
    if (v.logged >= 40) {
        return false;
    }
    ++v.logged;
    return true;
}

AX_NOINLINE void ExecuteCommandListVerified(AXCommandLayout layout, uint32_t cmdListSize) {
    if (!m_verify) {
        m_verify = std::make_unique<VerifyScratch>();
    }
    VerifyScratch& v = *m_verify;
    const unsigned long long listNo = g_axStats.verifyLists++;
    const bool neon = AxFast::g_useNeon;

    // Pass 1: the reference, with every guest write journalled.
    SaveMixState(v.start);
    v.ref.Clear();
    v.fast.Clear();
    g_mixJournal = &v.ref;
    g_mixJournalOn.store(true, std::memory_order_relaxed);
    AxFast::g_useNeon = false;
    ExecuteCommandListPass(layout, cmdListSize, false);
    for (const MixJournal::Entry& e : v.ref.entries) {
        v.ref.after.insert(v.ref.after.end(), e.host, e.host + e.bytes);
    }
    SaveMixState(v.refEnd);
    // Undo it (newest write first) and go back to the starting state.
    for (size_t i = v.ref.entries.size(); i-- > 0;) {
        const MixJournal::Entry& e = v.ref.entries[i];
        std::memcpy(e.host, v.ref.before.data() + e.offset, e.bytes);
    }
    LoadMixState(v.start);

    // Pass 2: the fast path; its result stays.
    g_mixJournal = &v.fast;
    AxFast::g_useNeon = neon;
    m_statsMuted = true;
    ExecuteCommandListPass(layout, cmdListSize, true);
    m_statsMuted = false;
    g_mixJournalOn.store(false, std::memory_order_relaxed);
    g_mixJournal = nullptr;
    SaveMixState(v.fastEnd);

    bool bad = false;
    // Mixer state.
    for (size_t b = 0; b < kAxRegularBusCount; ++b) {
        for (size_t i = 0; i < kAxSamplesPerFrame; ++i) {
            if (v.refEnd.bus[b][i] != v.fastEnd.bus[b][i]) {
                bad = true;
                if (VerifyLog(v)) {
                    RT_LOGF(RT_TAG_AUDIO, "VERIFY list %llu: bus %u sample %u: reference %d, fast %d\n", listNo,
                            static_cast<unsigned>(b), static_cast<unsigned>(i), v.refEnd.bus[b][i], v.fastEnd.bus[b][i]);
                }
                break;
            }
        }
    }
    if (v.refEnd.wmBus != v.fastEnd.wmBus) {
        bad = true;
        if (VerifyLog(v)) RT_LOGF(RT_TAG_AUDIO, "VERIFY list %llu: Wii Remote buses differ\n", listNo);
    }
    if (v.refEnd.lastAuxVolumes != v.fastEnd.lastAuxVolumes || v.refEnd.lastMainVolume != v.fastEnd.lastMainVolume ||
        v.refEnd.compressorPos != v.fastEnd.compressorPos || v.refEnd.pbLayout != v.fastEnd.pbLayout) {
        bad = true;
        if (VerifyLog(v)) RT_LOGF(RT_TAG_AUDIO, "VERIFY list %llu: output/aux volume, compressor or layout state differs\n", listNo);
    }
    if (v.refEnd.auxOutCount != v.fastEnd.auxOutCount) {
        bad = true;
        if (VerifyLog(v)) {
            RT_LOGF(RT_TAG_AUDIO, "VERIFY list %llu: aux shadow count: reference %u, fast %u\n", listNo,
                    v.refEnd.auxOutCount, v.fastEnd.auxOutCount);
        }
    } else {
        for (uint32_t i = 0; i < v.refEnd.auxOutCount; ++i) {
            if (v.refEnd.auxOut[i].addr != v.fastEnd.auxOut[i].addr || v.refEnd.auxOut[i].data != v.fastEnd.auxOut[i].data) {
                bad = true;
                if (VerifyLog(v)) RT_LOGF(RT_TAG_AUDIO, "VERIFY list %llu: aux shadow block %u differs\n", listNo, i);
            }
        }
    }
    // Guest memory, part 1: wherever the reference wrote, memory must now hold what it held at
    // the end of the reference pass.
    for (const MixJournal::Entry& e : v.ref.entries) {
        const uint8_t* want = v.ref.after.data() + e.offset;
        if (std::memcmp(e.host, want, e.bytes) != 0) {
            bad = true;
            if (VerifyLog(v)) {
                uint32_t at = 0;
                while (at < e.bytes && e.host[at] == want[at]) ++at;
                char where[32];
                DescribeHost(e.host, where, sizeof(where));
                RT_LOGF(RT_TAG_AUDIO, "VERIFY list %llu: guest %s (%u bytes written by the reference) differs at +%u: reference %02X %02X, fast %02X %02X\n",
                        listNo, where, e.bytes, at & ~1u, want[at & ~1u], want[(at & ~1u) + 1], e.host[at & ~1u], e.host[(at & ~1u) + 1]);
            }
        }
    }
    // Part 2: wherever the fast path wrote, memory must hold what the reference pass left there
    // (its own writes where it wrote, the content from before the mix elsewhere).
    for (const MixJournal::Entry& f : v.fast.entries) {
        v.expected.assign(v.fast.before.begin() + f.offset, v.fast.before.begin() + f.offset + f.bytes);
        for (const MixJournal::Entry& e : v.ref.entries) {
            const uint8_t* lo = std::max(e.host, f.host);
            const uint8_t* hi = std::min(e.host + e.bytes, f.host + f.bytes);
            if (lo < hi) {
                std::memcpy(v.expected.data() + (lo - f.host), v.ref.after.data() + e.offset + (lo - e.host),
                            static_cast<size_t>(hi - lo));
            }
        }
        if (std::memcmp(f.host, v.expected.data(), f.bytes) != 0) {
            bad = true;
            if (VerifyLog(v)) {
                uint32_t at = 0;
                while (at < f.bytes && f.host[at] == v.expected[at]) ++at;
                char where[32];
                DescribeHost(f.host, where, sizeof(where));
                RT_LOGF(RT_TAG_AUDIO, "VERIFY list %llu: guest %s (%u bytes written by the fast path) differs at +%u: reference %02X %02X, fast %02X %02X\n",
                        listNo, where, f.bytes, at & ~1u, v.expected[at & ~1u], v.expected[(at & ~1u) + 1], f.host[at & ~1u], f.host[(at & ~1u) + 1]);
            }
        }
    }
    if (bad) {
        ++g_axStats.verifyBad;
        std::fflush(stderr);
    }
}
