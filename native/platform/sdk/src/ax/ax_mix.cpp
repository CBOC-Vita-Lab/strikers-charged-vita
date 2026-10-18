// Ported from the recompiled build's AX/DSP HLE (one/port/runtime/src/hle/audio/ax_mix.cpp, itself a
// port of Dolphin's AXWii ucode HLE): command-list mixer of the Wii AX microcode. Changes for the
// native port: host-endian memory and plain pointers (ax_internal.h, ax_native_endian.h), no worker
// thread (the audio thread runs the mix after the AI callback: RunPendingMix), the DSP task and
// callback glue lives in dsp_hle.cpp, the resampling coefficient ROM is compiled in.
#include "ax_dsp.h"

#include "ax_internal.h"

#include "ax_mix_kernels.h"
#include "ax_mix_fast.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// Workstream AUD: the kernels of the reference path stay out of line so that a per-function
// instruction profile (qemu block profile by symbol) can tell them apart. Each is entered once
// per voice or per bus per 96-sample frame, so the calls cost nothing measurable.
#if defined(__GNUC__)
#define AX_NOINLINE __attribute__((noinline))
#else
#define AX_NOINLINE
#endif

namespace AxFast {
bool g_useNeon = true;
} // namespace AxFast

namespace AxDspHle {

// Write journal of the mix (MSC_AX_VERIFY, see AXWii::ExecuteCommandList): every write of the
// mix into guest memory reports its range first (MixNoteWrite in ax_internal.h).
std::atomic<bool> g_mixJournalOn{false};

namespace {

// How the mix runs (read once from the environment when the AX HLE initialises):
//   MSC_AX_FAST=0    the reference path only (the mixer as it was: scalar, per-sample reads)
//   MSC_AX_FAST=1    the fast path (ax_mix_fast.h), NEON kernels on ARM              [default]
//   MSC_AX_FAST=2    the fast path with the plain C++ kernels (what a non-ARM build runs anyway)
//   MSC_AX_VERIFY=1  every command list is mixed twice, reference first, then the fast path
//                    from the same starting state; buses, mixer state, aux shadows and every
//                    guest byte either pass wrote are compared, differences are counted and the
//                    first ones logged. The fast pass's result stays in effect.
//   MSC_AX_STATS=1   voice census and path counters, printed by MscAxPrintStats at exit
//   NP_AX_MIX=0      (native port) NOT SAFE: command lists are accepted and acknowledged but never mixed (no PCM, voices never end, no
//                    parameter block write-back); the front end then crashes at f=58 in SoundPlaybackEvent::AccumulateRpcModifiers
//                    (an RPC node whose definition was cleared by a voice-end path that never runs). Kept for experiments only;
//                    the AX cost of the front end was the unswapped audio stream header (NPP), not the mixer.
//   NP_AX_LOG=1      log the first voices that take the reference path (ratio above 8.0 or an unusable coefficient bank)
//   MSC_AX_INLINE_BURST=0  every command list goes to the mix worker. Default 1: a list whose
//                          result the tick waits for at once is mixed on the guest thread
//                          (see SetMixInlineOnce; same PCM and guest state, aud/NOTES.md)
struct AxMode {
    int fast = 1;
    bool verify = false;
    bool stats = false;
    bool inlineBurst = true;
    bool mix = true;
    bool logFallback = false;
};
AxMode g_axMode;

void ReadAxMode() {
    AxMode mode;
    if (const char* e = std::getenv("MSC_AX_FAST")) {
        mode.fast = std::atoi(e);
    }
    if (const char* e = std::getenv("MSC_AX_VERIFY")) {
        mode.verify = *e != 0 && *e != '0';
    }
    if (const char* e = std::getenv("MSC_AX_STATS")) {
        mode.stats = *e != 0 && *e != '0';
    }
    if (const char* e = std::getenv("NP_AX_MIX")) {
        mode.mix = *e != 0 && *e != '0';
    }
    if (const char* e = std::getenv("NP_AX_LOG")) {
        mode.logFallback = *e != 0 && *e != '0';
    }
    if (const char* e = std::getenv("MSC_AX_INLINE_BURST")) {
        mode.inlineBurst = *e != 0 && *e != '0';
    }
    g_axMode = mode;
    AxFast::g_useNeon = mode.fast == 1;
}

struct MixJournal {
    struct Entry {
        uint8_t* host;
        uint32_t bytes;
        uint32_t offset; // of the range's content before the write, in `before`
    };
    std::vector<Entry> entries;
    std::vector<uint8_t> before;
    std::vector<uint8_t> after; // content at the end of the pass, same offsets
    void Clear() {
        entries.clear();
        before.clear();
        after.clear();
    }
};
MixJournal* g_mixJournal = nullptr;

// Census and path counters (MSC_AX_STATS=1). Written by whichever thread runs the mix.
struct AxStats {
    uint64_t lists = 0, pbs = 0, running = 0, stoppedSkipped = 0, referenceVoices = 0, fastVoices = 0;
    uint64_t fmtAdpcm = 0, fmtPcm16 = 0, fmtPcm8 = 0, fmtOther = 0;
    uint64_t src[3] = {0, 0, 0}, srcNoTable = 0, coefSel[4] = {0, 0, 0, 0};
    uint64_t ratioLe1 = 0, ratioLe2 = 0, ratioLe4 = 0, ratioMore = 0, inputSamples = 0;
    uint64_t envUnity = 0, envZero = 0, envConst = 0, envRamp = 0;
    uint64_t lpf = 0, biquad = 0, remote = 0, updates = 0, voiceEnded = 0;
    uint64_t busMixes = 0, busRampBit = 0, busRampMoving = 0, busZeroVolume = 0, busUnity = 0;
    uint64_t busesPerVoice[13] = {};
    uint64_t silentVoices = 0, fallbackRatio = 0, fallbackTable = 0, fallbackResolve = 0;
    uint64_t cmd[16] = {};
    uint64_t auxConst = 0, auxUnity = 0, auxRamp = 0, outConst = 0, outUnity = 0, outRamp = 0;
    uint64_t compressorScans = 0, compressorApplied = 0;
    uint64_t verifyLists = 0, verifyBad = 0, verifyUnjournaled = 0;
    uint64_t listsSkipped = 0; // NP_AX_MIX=0
    uint64_t listsDeferred = 0, listsInline = 0; // command lists handed to the worker / mixed on the guest thread
    uint64_t workerMixes = 0, workerMixNs = 0, workerMixMaxNs = 0; // real time of the worker's mixes
    uint64_t joins = 0, joinsWaited = 0; // guest thread: JoinMix calls, and those that found a mix pending or running
};
AxStats g_axStats;

} // namespace
extern "C" const unsigned char np_ax_dsp_coef_rom[4096];
namespace {

void (*g_resumeCallback)() = nullptr;

class AXWii {
public:
    AXWii() {
        m_lastMainVolume = 0x8000;
        m_lastAuxVolumes.fill(0x8000);
    }

    ~AXWii() { ShutdownMixWorker(); }

    void Initialize() {
        // Every entry point that resets or reconfigures mix state joins the
        // worker first: the mix owns those members outright while it runs.
        JoinMix();
        std::lock_guard<std::mutex> lock(m_mutex);
        ReadAxMode();
        m_mixWorkerEnabled = true;   // "deferred": the mix runs in RunPendingMix
        m_mailState = MailState::WaitingForCmdListSize;
        m_cmdListSize = 0;
        m_toDspBusy = false;
        m_cpuMails.clear();
        m_compressorPos = 0;
        m_deferredResumeCallbacks = 0;
        // Both layouts are re-derived from the ucode CRC by ConfigureFromTask;
        // until one arrives, the PB layout is detected per parameter block and
        // the command layout is probed per command list.
        m_pbLayout = PBLayout::Unknown;
        m_commandLayout = AXCommandLayout::Auto;
        m_ucodeCrc = 0;
        m_auxOutCount = 0;
        m_auxInCount = 0;
        m_deferredAux = false;
        LoadResamplingCoefficients();
        PushMail(kDspInit);
    }

    void Stop() {
        JoinMix();
        std::lock_guard<std::mutex> lock(m_mutex);
        m_mailState = MailState::WaitingForCmdListSize;
        m_cmdListSize = 0;
        m_toDspBusy = false;
        m_cpuMails.clear();
        m_lastCpuMail = 0;
        m_compressorPos = 0;
        m_deferredResumeCallbacks = 0;
        m_ucodeCrc = 0;
    }

    void ConfigureFromIram(const uint8_t* iram, uint32_t length) {
        JoinMix();
        std::lock_guard<std::mutex> lock(m_mutex);
        ConfigureFromIramLocked(iram, length);
    }

    // Runs the command list the last mail pair delivered, if it is still pending, and publishes the
    // aux outputs it produced (one frame later than a synchronous mix, as the real DSP's).
    void JoinMix() {
        if (m_mixPending) {
            m_mixPending = false;
            const bool saved = t_onMixWorker;
            t_onMixWorker = true;
            RunMix();
            t_onMixWorker = saved;
        }
        PublishAuxOutputs();
    }

    void ShutdownMixWorker() {}

    void SetMixWorkerEnabled(bool enabled) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_mixWorkerEnabled = enabled;
    }

    void SetMixInlineOnce(bool inlineNext) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_inlineNext = inlineNext;
    }

    void SendMail(uint32_t mail) {
        std::lock_guard<std::mutex> lock(m_mutex);
        HandleMail(mail);
        m_toDspBusy = false;
    }

    void QueueResumeCallback() {
        std::lock_guard<std::mutex> lock(m_mutex);
        ++m_deferredResumeCallbacks;
    }

    void ServiceDeferredCallbacks() {
        while (true) {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_deferredResumeCallbacks == 0) {
                    return;
                }
                --m_deferredResumeCallbacks;
            }
            if (g_resumeCallback) g_resumeCallback();
        }
    }

    uint32_t CheckMailToDSP() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_toDspBusy ? 1u : 0u;
    }

    uint32_t CheckMailFromDSP() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_cpuMails.empty() ? 0u : 1u;
    }

    uint32_t ReadMailFromDSP() {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_cpuMails.empty()) {
            return m_lastCpuMail & 0x7fffffffu;
        }
        m_lastCpuMail = m_cpuMails.front();
        m_cpuMails.pop_front();
        return m_lastCpuMail;
    }

private:
    // What DispatchCommandList has to know about a command list before it is
    // allowed to leave the guest thread.
    struct CommandListScan {
        uint32_t outputCommands = 0;
        uint32_t auxInputCount = 0;
        // Three blocks per MIX_AUX* (x3) plus four per UPL_AUX* (x2) = 17.
        std::array<uint32_t, 24> auxInputAddrs{};
    };

    struct AuxBlock {
        uint32_t addr = 0;
        std::array<int32_t, kAxSamplesPerFrame> data{};
    };

    enum Cmd : uint16_t {
        CMD_SETUP = 0x00,
        CMD_ADD_TO_LR = 0x01,
        CMD_SUB_TO_LR = 0x02,
        CMD_ADD_SUB_TO_LR = 0x03,
        CMD_PROCESS = 0x04,
        CMD_MIX_AUXA = 0x05,
        CMD_MIX_AUXB = 0x06,
        CMD_MIX_AUXC = 0x07,
        CMD_UPL_AUXA_MIX_LRSC = 0x08,
        CMD_UPL_AUXB_MIX_LRSC = 0x09,
        CMD_COMPRESSOR = 0x0a,
        CMD_OUTPUT = 0x0b,
        CMD_OUTPUT_DPL2 = 0x0c,
        CMD_WM_OUTPUT = 0x0d,
        CMD_END = 0x0e,
    };


    static constexpr uint16_t kCmdSetPbAddressOld = 0x04;

    struct DecodedCommand {
        Cmd cmd = CMD_SETUP;
        bool setsPbAddress = false;
    };

    static DecodedCommand DecodeCommand(uint16_t raw, AXCommandLayout layout) {
        if (layout != AXCommandLayout::Old) {
            return {static_cast<Cmd>(raw), false};
        }
        if (raw == kCmdSetPbAddressOld) {
            return {CMD_SETUP, true};
        }
        return {static_cast<Cmd>(raw > kCmdSetPbAddressOld ? raw - 1u : raw), false};
    }

    // The old layout runs the PB chain the previous SET_PB_ADDRESS stashed, and
    // ProcessPBList applies the old per-millisecond PB update walk for it.
    static bool UsesStashedPbAddress(AXCommandLayout layout) {
        return layout == AXCommandLayout::Old;
    }

    // Only the plain New encoding puts a volume halfword in front of OUTPUT's
    // two addresses; Old and NewNoOutputVolume both imply 0x8000.
    static bool OutputCarriesVolume(AXCommandLayout layout) {
        return layout != AXCommandLayout::Old && layout != AXCommandLayout::NewNoOutputVolume;
    }

    // Mail from the (emulated) DSP to the game. The game only takes these out in its DSP
    // interrupt path (DSPReadMailFromDSP), which this HLE does not run during normal mixing:
    // it calls the AX callback directly. Without a bound the queue grew by about 1,100 words
    // (4.4 KiB) per second of running for ever: on the Vita, with its 40 MiB C heap, that is
    // the whole heap after about two hours (found from the Vita build's heap line in Vita3K
    // and traced on x86-64, 5 Oct). Keep the newest entries only; a reader that does come
    // (start-up handshake) is never more than a few words behind.
    static constexpr size_t kMaxCpuMails = 256;
    void PushMail(uint32_t mail) {
        if (m_cpuMails.size() >= kMaxCpuMails) {
            m_cpuMails.pop_front();
        }
        m_cpuMails.push_back(mail);
    }

    void HandleMail(uint32_t mail) {
        switch (m_mailState) {
        case MailState::WaitingForCmdListSize:
            if ((mail & kMailCmdListMask) == kMailCmdList) {
                m_cmdListSize = mail & 0xffffu;
                m_mailState = MailState::WaitingForCmdListAddress;
            } else if ((mail & 0xffff0000u) == kTaskMailToDsp) {
                HandleTaskMail(mail);
            }
            break;
        case MailState::WaitingForCmdListAddress:
            // The previous mix owns m_cmdList, the mix buses and the PB chain
            // until it finishes. In practice this join is free
            JoinMix();
            CopyCmdList(mail, m_cmdListSize);
            DispatchCommandList();
            m_cmdListSize = 0;
            //  yield mail and the deferred resume callback are still raised synchronously at mail time,
            PushMail(kDspYield);
            ++m_deferredResumeCallbacks;
            m_mailState = MailState::WaitingForNextTask;
            break;
        case MailState::WaitingForNextTask:
            if ((mail & kMailCmdListMask) == kMailCmdList) {
                m_cmdListSize = mail & 0xffffu;
                m_mailState = MailState::WaitingForCmdListAddress;
            } else {
                HandleTaskMail(mail);
            }
            break;
        }
    }

    void HandleTaskMail(uint32_t mail) {
        switch (mail) {
        case kMailResume:
            PushMail(kDspResume);
            m_mailState = MailState::WaitingForCmdListSize;
            break;
        case kMailNewUCode:
        case kMailContinue:
            m_mailState = MailState::WaitingForCmdListSize;
            break;
        case kMailReset:
            PushMail(kDspDone);
            m_mailState = MailState::WaitingForCmdListSize;
            break;
        default:
            m_mailState = MailState::WaitingForCmdListSize;
            break;
        }
    }

    void CopyCmdList(uint32_t addr, uint32_t sizeWords) {
        if (sizeWords > m_cmdList.size()) {
            // Truncating a command list drops whole commands
            if (!m_loggedCmdListTruncated.exchange(true, std::memory_order_relaxed)) {
                RT_LOGF(RT_TAG_AUDIO,
                             "command list of %u words exceeds the %u-word buffer; truncating\n",
                             sizeWords, static_cast<unsigned>(m_cmdList.size()));
                std::fflush(stderr);
            }
            sizeWords = static_cast<uint32_t>(m_cmdList.size());
        }
        for (uint32_t i = 0; i < sizeWords; ++i) {
            m_cmdList[i] = MixRead16(addr + i * 2);
        }
    }

    // Everything that must be observed at mail time happens here, before the mix leaves the
    // guest thread: the DSP sync mails OUTPUT commands raise (so the mail sequence doesn't
    // depend on where the mix runs)
    void DispatchCommandList() {
        AXCommandLayout layout = m_commandLayout;
        if (layout == AXCommandLayout::Auto) {
            const bool validNew = ScanCommandList(AXCommandLayout::New, nullptr);
            const bool validNewNoOutputVolume = ScanCommandList(AXCommandLayout::NewNoOutputVolume, nullptr);
            const bool validOld = ScanCommandList(AXCommandLayout::Old, nullptr);
            layout = validNew ? AXCommandLayout::New :
                     (validNewNoOutputVolume ? AXCommandLayout::NewNoOutputVolume :
                      (validOld ? AXCommandLayout::Old : AXCommandLayout::New));
        }

        CommandListScan scan{};
        ScanCommandList(layout, &scan);
        for (uint32_t i = 0; i < scan.outputCommands; ++i) {
            PushMail(kDspSync);
        }

        // (Workstream AUD) A list whose result the tick will wait for at once is mixed here. Only
        // lists without aux inputs: with them the two routes differ by design (the deferred one
        // publishes aux output one frame later), and this must not change what the guest sees.
        const bool inlineBurst =
            m_mixWorkerEnabled && m_inlineNext && g_axMode.inlineBurst && scan.auxInputCount == 0;
        const bool deferred = m_mixWorkerEnabled && !inlineBurst && EnsureMixWorker();
        ++(deferred ? g_axStats.listsDeferred : g_axStats.listsInline);
        m_deferredAux = deferred;
        m_auxInCount = 0;
        if (deferred) {
            for (uint32_t i = 0; i < scan.auxInputCount && m_auxInCount < m_auxIn.size(); ++i) {
                AuxBlock& block = m_auxIn[m_auxInCount++];
                block.addr = scan.auxInputAddrs[i];
                ReadGuestS32Buffer(block.addr, block.data.data(), kAxSamplesPerFrame);
            }
        }

        m_mixLayout = layout;
        m_mixCmdListSize = m_cmdListSize;
        if (deferred) {
            m_mixPending = true;
            return;
        }

        if (inlineBurst) {
            // The worker's mix on this thread: same fail-safe guest accessors, same handling of
            // an exception, so that a list behaves alike wherever it is mixed.
            struct WorkerAccessors {
                bool saved = t_onMixWorker;
                WorkerAccessors() { t_onMixWorker = true; }
                ~WorkerAccessors() { t_onMixWorker = saved; }
            } accessors;
            RunMix();
            return;
        }
        RunMix();
    }

    // NP_AX_MIX=0: the list is acknowledged (mails, callbacks) but not mixed.
    void RunMix() {
        if (!g_axMode.mix) {
            ++g_axStats.listsSkipped;
            return;
        }
        ExecuteCommandList(m_mixLayout, m_mixCmdListSize);
    }

    bool CanReadWords(uint32_t idx, uint32_t count) const {
        return idx <= m_cmdListSize && count <= m_cmdListSize - idx;
    }

    // Walks the command list for a candidate layout
    bool ScanCommandList(AXCommandLayout layout, CommandListScan* scan) const {
        uint32_t idx = 0;
        bool end = false;
        auto addAuxInput = [&](uint32_t addr, uint32_t blocks) {
            if (!scan) {
                return;
            }
            for (uint32_t block = 0; block < blocks; ++block) {
                if (scan->auxInputCount >= scan->auxInputAddrs.size()) {
                    return;
                }
                scan->auxInputAddrs[scan->auxInputCount++] =
                    addr + block * kAxSamplesPerFrame * static_cast<uint32_t>(sizeof(int32_t));
            }
        };
        auto wordAddress = [&](uint32_t base) {
            return Hilo(m_cmdList[base], m_cmdList[base + 1]);
        };

        while (!end) {
            if (!CanReadWords(idx, 1)) {
                return false;
            }
            const uint16_t cmd = m_cmdList[idx++];
            const uint32_t operandBase = idx;
            auto skip = [&](uint32_t words) {
                if (!CanReadWords(idx, words)) {
                    return false;
                }
                idx += words;
                return true;
            };

            const DecodedCommand decoded = DecodeCommand(cmd, layout);
            if (decoded.setsPbAddress) {
                if (!skip(2)) return false;
                continue;
            }

            switch (decoded.cmd) {
            case CMD_SETUP:
            case CMD_ADD_TO_LR:
            case CMD_SUB_TO_LR:
            case CMD_ADD_SUB_TO_LR:
                if (!skip(2)) return false;
                break;
            case CMD_PROCESS:
                if (!UsesStashedPbAddress(layout) && !skip(2)) return false;
                break;
            case CMD_MIX_AUXA:
            case CMD_MIX_AUXB:
            case CMD_MIX_AUXC:
                if (!skip(5)) return false;
                // volume, write address, read address; the mix reads three
                // consecutive 96-word blocks from the read address.
                addAuxInput(wordAddress(operandBase + 3), 3);
                break;
            case CMD_UPL_AUXA_MIX_LRSC:
            case CMD_UPL_AUXB_MIX_LRSC:
                if (!skip(13)) return false;
                // volume then six addresses; [2..5] are read back.
                for (uint32_t slot = 2; slot < 6; ++slot) {
                    addAuxInput(wordAddress(operandBase + 1 + slot * 2), 1);
                }
                break;
            case CMD_COMPRESSOR:
                if (!skip(4)) return false;
                break;
            case CMD_OUTPUT:
            case CMD_OUTPUT_DPL2:
                if (!skip(OutputCarriesVolume(layout) ? 5 : 4)) return false;
                if (scan) ++scan->outputCommands;
                break;
            case CMD_WM_OUTPUT:
                if (!skip(8)) return false;
                break;
            case CMD_END:
                end = true;
                break;
            default:
                return false;
            }
        }
        return idx <= m_cmdListSize;
    }

    void ExecuteCommandList(AXCommandLayout layout, uint32_t cmdListSize) {
        if (g_axMode.verify) {
            ExecuteCommandListVerified(layout, cmdListSize);
            return;
        }
        ExecuteCommandListPass(layout, cmdListSize, g_axMode.fast != 0);
    }

    // One pass over the command list. `fast` selects the fast path (ax_mix_fast.h and
    // ax_mix_fast_members.inc) for everything that has one; false is the mixer as it was.
    void ExecuteCommandListPass(AXCommandLayout layout, uint32_t cmdListSize, bool fast) {
        m_fastPass = fast;
        if (g_axMode.stats && !m_statsMuted) {
            ++g_axStats.lists;
        }
        uint32_t idx = 0;
        uint32_t oldPbAddr = 0;
        const bool newFilter = m_ucodeCrc != 0 ? UCodeUsesNewFilter(m_ucodeCrc) : layout != AXCommandLayout::Old;
        bool end = false;
        m_auxOutCount = 0;
        while (!end && idx < cmdListSize) {
            const uint16_t cmd = m_cmdList[idx++];
            uint16_t volume = 0;
            uint16_t frames = 0;
            uint32_t addr = 0;
            uint32_t addr2 = 0;
            const DecodedCommand decoded = DecodeCommand(cmd, layout);
            if (decoded.setsPbAddress) {
                oldPbAddr = ReadAddress(idx);
                continue;
            }
            if (g_axMode.stats && !m_statsMuted) {
                ++g_axStats.cmd[static_cast<uint32_t>(decoded.cmd) & 15u];
            }

            switch (decoded.cmd) {
            case CMD_SETUP:
                addr = ReadAddress(idx);
                if (fast) {
                    SetupProcessingFast(addr);
                } else {
                    SetupProcessing(addr);
                }
                break;
            case CMD_ADD_TO_LR:
            case CMD_SUB_TO_LR:
                addr = ReadAddress(idx);
                if (fast) {
                    AddToLRFast(addr, decoded.cmd == CMD_SUB_TO_LR);
                } else {
                    AddToLR(addr, decoded.cmd == CMD_SUB_TO_LR);
                }
                break;
            case CMD_ADD_SUB_TO_LR:
                addr = ReadAddress(idx);
                AddSubToLR(addr);
                break;
            case CMD_PROCESS:
                if (UsesStashedPbAddress(layout)) {
                    ProcessPBList(oldPbAddr, newFilter, true);
                } else {
                    addr = ReadAddress(idx);
                    ProcessPBList(addr, newFilter, false);
                }
                break;
            case CMD_MIX_AUXA:
            case CMD_MIX_AUXB:
            case CMD_MIX_AUXC:
                volume = m_cmdList[idx++];
                addr = ReadAddress(idx);
                addr2 = ReadAddress(idx);
                if (fast) {
                    MixAUXSamplesFast(decoded.cmd - CMD_MIX_AUXA, addr, addr2, volume);
                } else {
                    MixAUXSamples(decoded.cmd - CMD_MIX_AUXA, addr, addr2, volume);
                }
                break;
            case CMD_UPL_AUXA_MIX_LRSC:
            case CMD_UPL_AUXB_MIX_LRSC: {
                volume = m_cmdList[idx++];
                std::array<uint32_t, 6> addresses{};
                for (uint32_t& out : addresses) {
                    out = ReadAddress(idx);
                }
                if (fast) {
                    UploadAUXMixLRSCFast(decoded.cmd == CMD_UPL_AUXB_MIX_LRSC ? 1 : 0, addresses.data(), volume);
                } else {
                    UploadAUXMixLRSC(decoded.cmd == CMD_UPL_AUXB_MIX_LRSC ? 1 : 0, addresses.data(), volume);
                }
                break;
            }
            case CMD_COMPRESSOR:
                volume = m_cmdList[idx++];
                frames = m_cmdList[idx++];
                addr = ReadAddress(idx);
                if (fast) {
                    ApplyCompressor(AxFast::AnyAbove96(m_bus[kBusMainL].data(), m_bus[kBusMainR].data(), volume), frames, addr);
                } else {
                    RunCompressor(volume, frames, addr);
                }
                break;
            case CMD_OUTPUT:
            case CMD_OUTPUT_DPL2:
                volume = OutputCarriesVolume(layout) ? m_cmdList[idx++] : 0x8000u;
                addr = ReadAddress(idx);
                addr2 = ReadAddress(idx);
                if (fast) {
                    OutputSamplesFast(addr2, addr, volume, decoded.cmd == CMD_OUTPUT_DPL2);
                } else {
                    OutputSamples(addr2, addr, volume, decoded.cmd == CMD_OUTPUT_DPL2);
                }
                break;
            case CMD_WM_OUTPUT: {
                std::array<uint32_t, 4> addresses{};
                for (uint32_t& out : addresses) {
                    out = ReadAddress(idx);
                }
                if (fast) {
                    OutputWMSamplesFast(addresses.data());
                } else {
                    OutputWMSamples(addresses.data());
                }
                break;
            }
            case CMD_END:
                end = true;
                break;
            default:
                ReportUnknownCommand(cmd, idx - 1);
                end = true;
                break;
            }
        }
    }

    void ReportUnknownCommand(uint16_t cmd, uint32_t word) {
        if (m_loggedUnknownCommand.exchange(true, std::memory_order_relaxed)) {
            return;
        }
        RT_LOGF(RT_TAG_AUDIO,
                     "unknown AX command 0x%04X at word %u; abandoning the command list\n",
                     cmd, word);
        std::fflush(stderr);
    }

    uint32_t ReadAddress(uint32_t& idx) const {
        const uint16_t hi = m_cmdList[idx++];
        const uint16_t lo = m_cmdList[idx++];
        return Hilo(hi, lo);
    }

    // Head-of-frame pointer for every bus, in AxBus order. This is the only
    // place the 20 buses are enumerated.
    AXBuffers MixBuffers() {
        AXBuffers buffers{};
        for (size_t i = 0; i < kAxRegularBusCount; ++i) {
            buffers[i] = m_bus[i].data();
        }
        for (size_t i = 0; i < kAxWiimoteBusCount; ++i) {
            buffers[kAxRegularBusCount + i] = m_wmBus[i].data();
        }
        return buffers;
    }

    AX_NOINLINE void SetupProcessing(uint32_t initAddr) {
        const AXBuffers buffers = MixBuffers();
        for (size_t i = 0; i < buffers.size(); ++i) {
            const uint32_t value = (MixRead16(initAddr + static_cast<uint32_t>(i * 6)) << 16) |
                                   MixRead16(initAddr + static_cast<uint32_t>(i * 6 + 2));
            const int16_t delta = static_cast<int16_t>(MixRead16(initAddr + static_cast<uint32_t>(i * 6 + 4)));
            const int count = (i < kAxRegularBusCount ? 32 : 6) * 3;
            if (value == 0) {
                std::fill(buffers[i], buffers[i] + count, 0);
            } else {
                const int32_t start = static_cast<int32_t>(value);
                for (int j = 0; j < count; ++j) {
                    buffers[i][j] = start + j * delta;
                }
            }
        }
    }

    AX_NOINLINE void AddToLR(uint32_t addr, bool neg) {
        for (uint32_t i = 0; i < kAxSamplesPerFrame; ++i) {
            int32_t value = static_cast<int32_t>(MixRead32(addr + i * 4));
            if (neg) {
                value = -value;
            }
            m_bus[kBusMainL][i] += value;
            m_bus[kBusMainR][i] += value;
        }
    }

    AX_NOINLINE void AddSubToLR(uint32_t addr) {
        for (uint32_t i = 0; i < kAxSamplesPerFrame; ++i) {
            m_bus[kBusMainL][i] += static_cast<int32_t>(MixRead32(addr + i * 4));
        }
        for (uint32_t i = 0; i < kAxSamplesPerFrame; ++i) {
            m_bus[kBusMainR][i] -= static_cast<int32_t>(MixRead32(addr + (kAxSamplesPerFrame + i) * 4));
        }
    }

    static AXMixControl ConvertMixerControl(uint32_t mixerControl) {
        uint32_t ret = 0;
        if (mixerControl & 0x00000001) ret |= MIX_MAIN_L;
        if (mixerControl & 0x00000002) ret |= MIX_MAIN_R;
        if (mixerControl & 0x00000004) ret |= MIX_MAIN_L | MIX_MAIN_R | MIX_MAIN_L_RAMP | MIX_MAIN_R_RAMP;
        if (mixerControl & 0x00000008) ret |= MIX_MAIN_S;
        if (mixerControl & 0x00000010) ret |= MIX_MAIN_S | MIX_MAIN_S_RAMP;
        if (mixerControl & 0x00010000) ret |= MIX_AUXA_L;
        if (mixerControl & 0x00020000) ret |= MIX_AUXA_R;
        if (mixerControl & 0x00040000) ret |= MIX_AUXA_L | MIX_AUXA_R | MIX_AUXA_L_RAMP | MIX_AUXA_R_RAMP;
        if (mixerControl & 0x00080000) ret |= MIX_AUXA_S;
        if (mixerControl & 0x00100000) ret |= MIX_AUXA_S | MIX_AUXA_S_RAMP;
        if (mixerControl & 0x00200000) ret |= MIX_AUXB_L;
        if (mixerControl & 0x00400000) ret |= MIX_AUXB_R;
        if (mixerControl & 0x00800000) ret |= MIX_AUXB_L | MIX_AUXB_R | MIX_AUXB_L_RAMP | MIX_AUXB_R_RAMP;
        if (mixerControl & 0x01000000) ret |= MIX_AUXB_S;
        if (mixerControl & 0x02000000) ret |= MIX_AUXB_S | MIX_AUXB_S_RAMP;
        if (mixerControl & 0x04000000) ret |= MIX_AUXC_L;
        if (mixerControl & 0x08000000) ret |= MIX_AUXC_R;
        if (mixerControl & 0x10000000) ret |= MIX_AUXC_L | MIX_AUXC_R | MIX_AUXC_L_RAMP | MIX_AUXC_R_RAMP;
        if (mixerControl & 0x20000000) ret |= MIX_AUXC_S;
        if (mixerControl & 0x40000000) ret |= MIX_AUXC_S | MIX_AUXC_S_RAMP;
        return static_cast<AXMixControl>(ret);
    }

    static void GenerateVolumeRamp(uint16_t* output, uint16_t from, uint16_t to, size_t count) {
        float current = static_cast<float>(from);
        const float delta = (static_cast<float>(to) - static_cast<float>(from)) / static_cast<float>(count);
        for (size_t i = 0; i < count; ++i) {
            current += delta;
            output[i] = static_cast<uint16_t>(current);
        }
    }

    void ProcessPBList(uint32_t pbAddr, bool newFilter, bool oldAxLayout) {
        if (m_fastPass && !oldAxLayout) {
            // (The old command layout's per-millisecond parameter updates stay on the reference.)
            ProcessPBListFast(pbAddr, newFilter);
            return;
        }
        uint32_t guard = 0;
        while (pbAddr && guard++ < 256) {
            pbAddr = ProcessOnePB(pbAddr, newFilter, oldAxLayout);
        }
    }

    // One parameter block of the chain on the reference path; returns the next one's address.
    AX_NOINLINE uint32_t ProcessOnePB(uint32_t pbAddr, bool newFilter, bool oldAxLayout) {
        AXPBWii pb{};
        ReadPB(pbAddr, pb);
        AXBuffers buffers = MixBuffers();
        const AXMixControl control = ConvertMixerControl(Hilo(pb.mixer_control_hi, pb.mixer_control_lo));
        if (g_axMode.stats && !m_statsMuted) {
            CensusPB(pb, control, newFilter);
        }
        if (oldAxLayout && (pb.updates.num_updates[0] | pb.updates.num_updates[1] | pb.updates.num_updates[2]) != 0) {
            const auto updates = LoadPBUpdates(pb);
            for (uint32_t ms = 0; ms < 3; ++ms) {
                ApplyPBUpdatesForMs(pb, updates, ms);
                ProcessVoice(pb, buffers, 32, control, BaseCoefficients(), newFilter);
                AdvanceBuffers(buffers, 32, 6);
            }
        } else {
            ProcessVoice(pb, buffers, kAxSamplesPerFrame, control, BaseCoefficients(), newFilter);
        }
        WritePB(pbAddr, pb);
        return Hilo(pb.next_pb_hi, pb.next_pb_lo);
    }

    // Four consecutive 0x200-halfword polyphase banks; only the voice resample selects a
    // bank (VoiceCoefficients). Wiimote downsample always uses bank 0 at its fixed ratio.
    const int16_t* BaseCoefficients() const {
        return m_hasCoeffs ? m_coeffs.data() : nullptr;
    }

    // Mirrors GetInputSamples' coeffs += pb.coef_select * 0x200, but bounds-checked since
    // coef_select is a guest halfword; a corrupt PB degrades to linear interpolation instead
    // of an out-of-bounds read.
    static const int16_t* VoiceCoefficients(const int16_t* base, uint16_t coefSelect) {
        if (base == nullptr) {
            return nullptr;
        }
        const uint32_t offset = static_cast<uint32_t>(coefSelect) * 0x200u;
        if (offset + 0x200u > kResamplingCoefficientCount) {
            return nullptr;
        }
        return base + offset;
    }

    void LoadResamplingCoefficients() {
        if (m_triedLoadCoeffs) {
            return;
        }
        m_triedLoadCoeffs = true;

        for (size_t i = 0; i < m_coeffs.size(); ++i) {
            const uint16_t word = static_cast<uint16_t>(np_ax_dsp_coef_rom[i * 2]) << 8 |
                                  static_cast<uint16_t>(np_ax_dsp_coef_rom[i * 2 + 1]);
            m_coeffs[i] = static_cast<int16_t>(word);
        }
        m_hasCoeffs = true;
        // Fast path: may a bank's four-tap sum be formed in 32 bits? |sample| <= 0x8000, so it
        // may when no phase's four magnitudes sum to more than 0xFFFF (ax_mix_fast.h).
        for (size_t bank = 0; bank < m_coefBankFits32.size(); ++bank) {
            uint32_t worst = 0;
            for (size_t phase = 0; phase < 0x80; ++phase) {
                uint32_t sum = 0;
                for (size_t tap = 0; tap < 4; ++tap) {
                    const int32_t c = m_coeffs[bank * 0x200 + phase * 4 + tap];
                    sum += static_cast<uint32_t>(c < 0 ? -c : c);
                }
                worst = std::max(worst, sum);
            }
            m_coefBankFits32[bank] = worst <= 0xFFFFu;
        }
    }

    static std::array<PBUpdate, 32> LoadPBUpdates(const AXPBWii& pb) {
        std::array<PBUpdate, 32> updates{};
        const uint32_t addr = Hilo(pb.updates.data_hi, pb.updates.data_lo);
        if (addr == 0) {
            return updates;
        }
        for (size_t i = 0; i < updates.size(); ++i) {
            updates[i].pb_offset = MixRead16(addr + static_cast<uint32_t>(i * sizeof(PBUpdate)));
            updates[i].new_value = MixRead16(addr + static_cast<uint32_t>(i * sizeof(PBUpdate) + sizeof(uint16_t)));
        }
        return updates;
    }

    void ApplyPBUpdatesForMs(AXPBWii& pb, const std::array<PBUpdate, 32>& updates, uint32_t ms) {
        uint32_t start = 0;
        for (uint32_t i = 0; i < ms; ++i) {
            start += pb.updates.num_updates[i];
        }
        const uint32_t count = pb.updates.num_updates[ms];
        uint16_t* words = reinterpret_cast<uint16_t*>(&pb);
        constexpr uint32_t wordCount = sizeof(AXPBWii) / sizeof(uint16_t);
        for (uint32_t i = start; i < start + count && i < updates.size(); ++i) {
            if (updates[i].pb_offset >= wordCount) {
                continue;
            }
            words[updates[i].pb_offset] = updates[i].new_value;
        }
    }

    static void AdvanceBuffers(AXBuffers& buffers, uint32_t regular, uint32_t wiimote) {
        for (size_t i = 0; i < kAxRegularBusCount; ++i) {
            buffers[i] += regular;
        }
        for (size_t i = kAxRegularBusCount; i < buffers.size(); ++i) {
            buffers[i] += wiimote;
        }
    }

    // Both directions take their offsets in the same order - guest side first,
    // then host side - so a mirrored read/write pair reads the same way round.
    static void CopyGuestToStruct(uint32_t addr, size_t guestOffset, char* dst, size_t structOffset,
                                  size_t byteCount) {
        if (const uint8_t* guest =
                MixResolveRange(addr + static_cast<uint32_t>(guestOffset), byteCount)) {
            for (size_t i = 0; i < byteCount; i += sizeof(uint16_t)) {
                uint16_t value = 0;
                std::memcpy(&value, guest + i, sizeof(value));
                *reinterpret_cast<uint16_t*>(dst + structOffset + i) = MemoryInline::ByteSwap16(value);
            }
            return;
        }
        for (size_t i = 0; i < byteCount; i += sizeof(uint16_t)) {
            *reinterpret_cast<uint16_t*>(dst + structOffset + i) =
                MixRead16(addr + static_cast<uint32_t>(guestOffset + i));
        }
    }

    static void CopyStructToGuest(uint32_t addr, size_t guestOffset, const char* src, size_t structOffset,
                                  size_t byteCount) {
        if (uint8_t* guest =
                MixResolveRange(addr + static_cast<uint32_t>(guestOffset), byteCount)) {
            MixNoteWrite(guest, byteCount);
            for (size_t i = 0; i < byteCount; i += sizeof(uint16_t)) {
                BigEndian::Write16(
                    guest + i, *reinterpret_cast<const uint16_t*>(src + structOffset + i));
            }
            return;
        }
        for (size_t i = 0; i < byteCount; i += sizeof(uint16_t)) {
            MixWrite16(addr + static_cast<uint32_t>(guestOffset + i),
                       *reinterpret_cast<const uint16_t*>(src + structOffset + i));
        }
    }

    // One run of halfwords that is present in both the guest parameter block and
    // AXPBWii, possibly at different offsets because the guest layout omits a
    // field this struct carries.
    struct PBSegment {
        size_t guestOffset;
        size_t structOffset;
        size_t byteCount;
    };

    // The segmentation is a property of the ucode's PB layout, not of the
    // direction of travel, so reads and writes share this one table. Returns the
    // number of segments written into `out`.
    static size_t PBSegmentsForLayout(PBLayout layout, std::array<PBSegment, 3>& out) {
        constexpr size_t pbBytes = sizeof(AXPBWii);
        constexpr size_t updatesBegin = offsetof(AXPBWii, updates);
        constexpr size_t updatesEnd = offsetof(AXPBWii, updates) + sizeof(PBUpdatesWii);
        constexpr size_t gapBegin = offsetof(AXPBWii, hpf) + sizeof(PBHighPassFilter);
        constexpr size_t gapEnd = offsetof(AXPBWii, biquad) + sizeof(PBBiquadFilter);

        switch (layout) {
        case PBLayout::SkipBiquadGapOnly:
            out[0] = {0, 0, gapBegin};
            out[1] = {gapBegin, gapEnd, pbBytes - gapEnd};
            return 2;
        case PBLayout::SkipUpdatesAndBiquadGap:
            out[0] = {0, 0, updatesBegin};
            out[1] = {updatesBegin, updatesEnd, gapBegin - updatesEnd};
            out[2] = {gapBegin, gapEnd, pbBytes - gapEnd};
            return 3;
        case PBLayout::SkipUpdatesOnly:
        case PBLayout::Unknown:
        default:
            // Unknown means detection has not settled yet. ReadPB deliberately
            // falls back to the SkipUpdatesOnly shape so it has something to
            // score candidates against; WritePBWithLayout refuses instead,
            // because guessing a segmentation on the way back out would corrupt
            // the guest's parameter block.
            out[0] = {0, 0, updatesBegin};
            out[1] = {updatesBegin, updatesEnd, pbBytes - updatesEnd};
            return 2;
        }
    }

    AX_NOINLINE static void ReadPBWithLayout(uint32_t addr, AXPBWii& pb, PBLayout layout) {
        std::memset(&pb, 0, sizeof(pb));
        char* dst = reinterpret_cast<char*>(&pb);
        std::array<PBSegment, 3> segments{};
        const size_t count = PBSegmentsForLayout(layout, segments);
        for (size_t i = 0; i < count; ++i) {
            CopyGuestToStruct(addr, segments[i].guestOffset, dst, segments[i].structOffset,
                              segments[i].byteCount);
        }
    }

    AX_NOINLINE static void WritePBWithLayout(uint32_t addr, const AXPBWii& pb, PBLayout layout) {
        if (layout == PBLayout::Unknown) {
            // See PBSegmentsForLayout: no segmentation is known to be correct,
            // so nothing is written back. WritePB refuses earlier for the same
            // reason; this is the guard that makes it safe on its own.
            return;
        }
        const char* src = reinterpret_cast<const char*>(&pb);
        std::array<PBSegment, 3> segments{};
        const size_t count = PBSegmentsForLayout(layout, segments);
        for (size_t i = 0; i < count; ++i) {
            CopyStructToGuest(addr, segments[i].guestOffset, src, segments[i].structOffset,
                              segments[i].byteCount);
        }
    }

    static bool LooksLikeMainRamPointer(uint32_t addr) {
        return addr == 0 || MixContains(addr, sizeof(uint16_t) * 8);
    }

    static bool LooksLikeAxSampleFormat(uint16_t format) {
        return format == 0 || format == 0x19 || format == 0x0a;
    }

    static int ScorePBLayout(uint32_t addr, const AXPBWii& pb) {
        int score = 0;
        const uint32_t next = Hilo(pb.next_pb_hi, pb.next_pb_lo);
        const uint32_t self = Hilo(pb.this_pb_hi, pb.this_pb_lo);
        const uint32_t ratio = Hilo(pb.src.ratio_hi, pb.src.ratio_lo);
        const uint32_t loop = Hilo(pb.audio_addr.loop_addr_hi, pb.audio_addr.loop_addr_lo);
        const uint32_t end = Hilo(pb.audio_addr.end_addr_hi, pb.audio_addr.end_addr_lo);
        const uint32_t current = Hilo(pb.audio_addr.cur_addr_hi, pb.audio_addr.cur_addr_lo);

        score += LooksLikeMainRamPointer(next) ? 8 : -16;
        score += self == addr ? 12 : (LooksLikeMainRamPointer(self) ? 2 : -8);
        score += pb.running <= 1 ? 8 : -16;
        score += pb.running == 1 ? 16 : 0;
        score += pb.is_stream <= 1 ? 3 : -4;
        score += pb.src_type <= 2 ? 8 : -10;
        score += LooksLikeAxSampleFormat(pb.audio_addr.sample_format) ? 14 : -12;
        score += ratio > 0 && ratio <= 0x00040000u ? 12 : (pb.running == 1 ? -12 : 0);
        score += (loop & 0xfc000000u) == 0 ? 5 : -5;
        score += (end & 0xfc000000u) == 0 ? 5 : -5;
        score += (current & 0xc0000000u) == 0 || (current & 0xc0000000u) == 0x80000000u ? 5 : -5;
        score += end >= loop ? 4 : -4;
        return score;
    }

    PBLayout DetectPBLayout(uint32_t addr) {
        struct Candidate {
            PBLayout layout;
            AXPBWii pb;
            int score;
        };
        std::array<Candidate, 3> candidates{{
            {PBLayout::SkipUpdatesOnly, {}, 0},
            {PBLayout::SkipUpdatesAndBiquadGap, {}, 0},
            {PBLayout::SkipBiquadGapOnly, {}, 0},
        }};
        Candidate* best = nullptr;
        for (Candidate& candidate : candidates) {
            ReadPBWithLayout(addr, candidate.pb, candidate.layout);
            candidate.score = ScorePBLayout(addr, candidate.pb);
            if (candidate.pb.running == 1 && (!best || candidate.score > best->score)) {
                best = &candidate;
            }
        }
        if (!best || best->score < 40) {
            return PBLayout::Unknown;
        }
        return best->layout;
    }

    void ReadPB(uint32_t addr, AXPBWii& pb) {
        PBLayout layout = m_pbLayout;
        if (layout == PBLayout::Unknown) {
            layout = DetectPBLayout(addr);
            if (layout != PBLayout::Unknown) {
                m_pbLayout = layout;
            } else {
                layout = PBLayout::SkipUpdatesOnly;
            }
        }
        ReadPBWithLayout(addr, pb, layout);
    }

    void ConfigureFromIramLocked(const uint8_t* iram, uint32_t iramLength) {
        const uint32_t crc = HashEctor(iram, iramLength);
        if (crc == 0) {
            return;
        }
        m_ucodeCrc = crc;
        const PBLayout derivedPBLayout = PBLayoutForUCode(crc);
        if (derivedPBLayout != PBLayout::Unknown) {
            m_pbLayout = derivedPBLayout;
        }
        m_commandLayout = CommandLayoutForUCode(crc);
    }

    // PB write-back goes straight into guest memory even from the worker, deliberately:
    // the CPU's only PB chain access is __AXServiceVPB, reached from __AXSyncPBs before
    // the mail that starts a mix (and after the previous mix was joined), so guest and mix
    // never touch a PB concurrently. Shadowing it would only risk a stale-snapshot clobber.
    void WritePB(uint32_t addr, const AXPBWii& pb) const {
        if (m_pbLayout == PBLayout::Unknown) {
            return;
        }
        WritePBWithLayout(addr, pb, m_pbLayout);
    }

    AX_NOINLINE void RunCompressor(uint16_t threshold, uint16_t releaseFrames, uint32_t tableAddr) {
        bool triggered = false;
        for (uint32_t i = 0; i < kAxSamplesPerFrame; ++i) {
            const int64_t left = m_bus[kBusMainL][i];
            const int64_t right = m_bus[kBusMainR][i];
            if ((left < 0 ? -left : left) > threshold || (right < 0 ? -right : right) > threshold) {
                triggered = true;
                break;
            }
        }
        ApplyCompressor(triggered, releaseFrames, tableAddr);
    }

    // What the compressor does once it knows whether this frame is above the threshold (shared
    // by the reference scan above and the fast path's scan, AxFast::AnyAbove96).
    AX_NOINLINE void ApplyCompressor(bool triggered, uint16_t releaseFrames, uint32_t tableAddr) {
        if (g_axMode.stats && !m_statsMuted) {
            ++g_axStats.compressorScans;
        }
        constexpr uint32_t kAttackEntryCount = 11;
        const uint32_t frameByteSize = kAxSamplesPerFrame * sizeof(uint16_t);
        uint32_t tableOffset = 0;
        if (triggered) {
            tableOffset = m_compressorPos * frameByteSize;
            m_compressorPos = releaseFrames;
        } else if (m_compressorPos != 0) {
            --m_compressorPos;
            tableOffset = (kAttackEntryCount + m_compressorPos) * frameByteSize;
        } else {
            return;
        }
        if (g_axMode.stats && !m_statsMuted) {
            ++g_axStats.compressorApplied;
        }

        for (uint32_t i = 0; i < kAxSamplesPerFrame; ++i) {
            const uint16_t coef = MixRead16(tableAddr + tableOffset + i * sizeof(uint16_t));
            m_bus[kBusMainL][i] =
                static_cast<int>((static_cast<int64_t>(m_bus[kBusMainL][i]) * coef) >> 15);
            m_bus[kBusMainR][i] =
                static_cast<int>((static_cast<int64_t>(m_bus[kBusMainR][i]) * coef) >> 15);
        }
    }

    // (Out of line for the profile only; the kernel itself is AxMixKernels::ScaleRamp.)
    AX_NOINLINE static uint16_t ScaleEnvelope(int16_t* samples, uint32_t count, uint16_t volume, uint16_t delta) {
        return AxMixKernels::ScaleRamp(samples, count, volume, delta);
    }

    AX_NOINLINE void ProcessVoice(AXPBWii& pb, const AXBuffers& buffers, uint32_t count, AXMixControl control,
                      const int16_t* coeffs, bool newFilter) {
        if (pb.running != 1) {
            return;
        }
        Accelerator accel;
        accel.Setup(&pb);
        std::array<int16_t, kAxSamplesPerFrame> samples{};
        Resample(accel, pb, samples.data(), count, coeffs);
        // Same wrapping 16-bit ramp as MixAdd, minus the accumulate. The two's
        // complement wrap of the int16 envelope is bit-identical to the uint16
        // wrap the kernel performs, so the value stored back is unchanged.
        pb.vol_env.cur_volume = static_cast<int16_t>(ScaleEnvelope(
            samples.data(), count, static_cast<uint16_t>(pb.vol_env.cur_volume),
            static_cast<uint16_t>(pb.vol_env.cur_volume_delta)));
        if (pb.lpf.on) {
            LowPassFilter(samples.data(), count, pb.lpf);
        }
        if (newFilter && pb.biquad.on) {
            BiquadFilter(samples.data(), count, pb.biquad);
        }

        auto mix = [&](uint32_t bit, uint32_t rampBit, int* out, VolumeData& vd, int16_t& dpop) {
            if ((control & bit) == 0) {
                return;
            }
            MixAdd(out, samples.data(), count, vd, dpop, (control & rampBit) != 0);
        };
        mix(MIX_MAIN_L, MIX_MAIN_L_RAMP, buffers[kBusMainL], pb.mixer.main_left, pb.dpop.main_left);
        mix(MIX_MAIN_R, MIX_MAIN_R_RAMP, buffers[kBusMainR], pb.mixer.main_right, pb.dpop.main_right);
        mix(MIX_MAIN_S, MIX_MAIN_S_RAMP, buffers[kBusMainS], pb.mixer.main_surround, pb.dpop.main_surround);
        mix(MIX_AUXA_L, MIX_AUXA_L_RAMP, buffers[kBusAuxAL], pb.mixer.auxA_left, pb.dpop.auxA_left);
        mix(MIX_AUXA_R, MIX_AUXA_R_RAMP, buffers[kBusAuxAR], pb.mixer.auxA_right, pb.dpop.auxA_right);
        mix(MIX_AUXA_S, MIX_AUXA_S_RAMP, buffers[kBusAuxAS], pb.mixer.auxA_surround, pb.dpop.auxA_surround);
        mix(MIX_AUXB_L, MIX_AUXB_L_RAMP, buffers[kBusAuxBL], pb.mixer.auxB_left, pb.dpop.auxB_left);
        mix(MIX_AUXB_R, MIX_AUXB_R_RAMP, buffers[kBusAuxBR], pb.mixer.auxB_right, pb.dpop.auxB_right);
        mix(MIX_AUXB_S, MIX_AUXB_S_RAMP, buffers[kBusAuxBS], pb.mixer.auxB_surround, pb.dpop.auxB_surround);
        mix(MIX_AUXC_L, MIX_AUXC_L_RAMP, buffers[kBusAuxCL], pb.mixer.auxC_left, pb.dpop.auxC_left);
        mix(MIX_AUXC_R, MIX_AUXC_R_RAMP, buffers[kBusAuxCR], pb.mixer.auxC_right, pb.dpop.auxC_right);
        mix(MIX_AUXC_S, MIX_AUXC_S_RAMP, buffers[kBusAuxCS], pb.mixer.auxC_surround, pb.dpop.auxC_surround);

        if (pb.remote) {
            ProcessVoiceRemote(pb, samples.data(), count, buffers, coeffs, newFilter);
        }
    }

    // The Wii Remote speaker part of a voice (its own filter, 6 kHz conversion and eight small
    // buses), shared by the reference and the fast path: `samples` is the voice's frame after
    // envelope and filters, and is filtered in place.
    AX_NOINLINE void ProcessVoiceRemote(AXPBWii& pb, int16_t* samples, uint32_t count, const AXBuffers& buffers,
                                        const int16_t* coeffs, bool newFilter) {
        {
            if (newFilter && pb.remote_iir.on) {
                if (pb.remote_iir.on == 2) {
                    BiquadFilter(samples, count, pb.remote_iir.biquad);
                } else {
                    LowPassFilter(samples, count, pb.remote_iir.lpf);
                }
            }

            const uint32_t wmCount = count == kAxSamplesPerFrame ? 18u : 6u;
            std::array<int16_t, 18> wmSamples{};
            pb.remote_src.cur_addr_frac = static_cast<uint16_t>(ResampleAudio(
                [samples, count](uint32_t index) {
                    return samples[std::min<uint32_t>(index, count - 1)];
                },
                wmSamples.data(), wmCount, pb.remote_src.last_samples, pb.remote_src.cur_addr_frac,
                0x00055555u, 0, coeffs));

            auto mixRemote = [&](uint32_t channel, int* out, VolumeData& vd, int16_t& dpop) {
                const uint16_t mode = static_cast<uint16_t>((pb.remote_mixer_control >> (channel * 2)) & 3u);
                if (mode == 0) {
                    return;
                }
                MixAdd(out, wmSamples.data(), wmCount, vd, dpop, (mode & 2u) != 0);
            };
            mixRemote(0, buffers[kBusWm0Main], pb.remote_mixer.main0, pb.remote_dpop.main0);
            mixRemote(1, buffers[kBusWm0Aux], pb.remote_mixer.aux0, pb.remote_dpop.aux0);
            mixRemote(2, buffers[kBusWm1Main], pb.remote_mixer.main1, pb.remote_dpop.main1);
            mixRemote(3, buffers[kBusWm1Aux], pb.remote_mixer.aux1, pb.remote_dpop.aux1);
            mixRemote(4, buffers[kBusWm2Main], pb.remote_mixer.main2, pb.remote_dpop.main2);
            mixRemote(5, buffers[kBusWm2Aux], pb.remote_mixer.aux2, pb.remote_dpop.aux2);
            mixRemote(6, buffers[kBusWm3Main], pb.remote_mixer.main3, pb.remote_dpop.main3);
            mixRemote(7, buffers[kBusWm3Aux], pb.remote_mixer.aux3, pb.remote_dpop.aux3);
        }
    }

    template <typename Reader>
    static uint32_t ResampleAudio(Reader&& readSample, int16_t* out, uint32_t count, int16_t* lastSamples,
                                  uint32_t pos, uint32_t ratio, uint16_t srcType,
                                  const int16_t* coeffs = nullptr) {
        if (coeffs && srcType == 0) {
            std::array<int16_t, 4> temp{};
            std::memcpy(temp.data(), lastSamples, sizeof(int16_t) * 4);
            uint32_t idx = 4;
            uint32_t readSamples = 0;
            for (uint32_t i = 0; i < count; ++i) {
                pos += ratio;
                while (pos >= 0x10000u) {
                    temp[idx++ & 3] = readSample(readSamples++);
                    pos -= 0x10000u;
                }

                const uint16_t fracIndex = static_cast<uint16_t>(((pos & 0xffffu) >> 9) << 2);
                const int16_t* c = coeffs + fracIndex;
                const int64_t t0 = temp[idx++ & 3];
                const int64_t t1 = temp[idx++ & 3];
                const int64_t t2 = temp[idx++ & 3];
                const int64_t t3 = temp[idx++ & 3];
                out[i] = ClampS16((t0 * c[0] + t1 * c[1] + t2 * c[2] + t3 * c[3]) >> 15);
            }
            lastSamples[3] = temp[--idx & 3];
            lastSamples[2] = temp[--idx & 3];
            lastSamples[1] = temp[--idx & 3];
            lastSamples[0] = temp[--idx & 3];
            return pos & 0xffffu;
        }

        // SRCTYPE_POLYPHASE (0) without a table and SRCTYPE_LINEAR (1) both degrade to
        // LINEAR; anything else (SRCTYPE_NEAREST or undefined) reads the accelerator 1:1
        // and ignores the ratio, matching DSPHLE's ResampleAudio branch order.
        if (srcType != 0 && srcType != 1) {
            for (uint32_t i = 0; i < count; ++i) {
                out[i] = readSample(i);
            }
            if (count >= 4) {
                std::memcpy(lastSamples, out + count - 4, sizeof(int16_t) * 4);
            }
            return pos;
        }

        std::array<int16_t, 4> temp{};
        std::memcpy(temp.data(), lastSamples, sizeof(int16_t) * 4);
        uint32_t idx = 4;
        uint32_t readSamples = 0;
        for (uint32_t i = 0; i < count; ++i) {
            pos += ratio;
            while (pos >= 0x10000u) {
                temp[idx++ & 3] = readSample(readSamples++);
                pos -= 0x10000u;
            }
            const uint16_t frac = pos & 0xffffu;
            if (frac == 0) {
                out[i] = temp[idx++ & 3];
                idx += 3;
            } else {
                const int32_t s0 = temp[idx++ & 3];
                const int32_t s1 = temp[idx++ & 3];
                idx += 2;
                out[i] = static_cast<int16_t>((s0 * static_cast<uint16_t>(-frac) + s1 * frac) >> 16);
            }
        }
        lastSamples[3] = temp[--idx & 3];
        lastSamples[2] = temp[--idx & 3];
        lastSamples[1] = temp[--idx & 3];
        lastSamples[0] = temp[--idx & 3];
        return pos & 0xffffu;
    }

    // `coeffs` is the DROM base; the bank is selected here, per voice, exactly
    // where GetInputSamples selects it.
    AX_NOINLINE static void Resample(Accelerator& accel, AXPBWii& pb, int16_t* out, uint32_t count,
                         const int16_t* coeffs) {
        pb.src.cur_addr_frac = static_cast<uint16_t>(ResampleAudio(
            [&accel](uint32_t) {
                return accel.ReadSample();
            },
            out, count, pb.src.last_samples, pb.src.cur_addr_frac,
            Hilo(pb.src.ratio_hi, pb.src.ratio_lo), pb.src_type,
            VoiceCoefficients(coeffs, pb.coef_select)));
    }

    AX_NOINLINE static void MixAdd(int* out, const int16_t* input, uint32_t count, VolumeData& vd, int16_t& dpop, bool ramp) {
        // int16 x uint16 always fits in int32, so the 64-bit clamp the generic
        // helper performs is unnecessary here.
        const uint16_t delta = ramp ? vd.volume_delta : 0;
        vd.volume = AxMixKernels::MixAddRamp(out, input, count, vd.volume, delta, dpop);
    }

    AX_NOINLINE static void LowPassFilter(int16_t* samples, uint32_t count, PBLowPassFilter& filter) {
        for (uint32_t i = 0; i < count; ++i) {
            filter.yn1 = samples[i] = ClampS16((filter.a0 * static_cast<int32_t>(samples[i]) +
                                                filter.b0 * static_cast<int32_t>(filter.yn1)) >> 15);
        }
    }

    AX_NOINLINE static void BiquadFilter(int16_t* samples, uint32_t count, PBBiquadFilter& filter) {
        for (uint32_t i = 0; i < count; ++i) {
            const int16_t xn0 = samples[i];
            int64_t value = 0;
            value += filter.b0 * static_cast<int32_t>(xn0);
            value += filter.b1 * static_cast<int32_t>(filter.xn1);
            value += filter.b2 * static_cast<int32_t>(filter.xn2);
            value += filter.a1 * static_cast<int32_t>(filter.yn1);
            value += filter.a2 * static_cast<int32_t>(filter.yn2);
            value <<= 2;
            value += (value & 0x10000) ? 0x8000 : 0x7fff;
            const int16_t yn0 = ClampS16(value >> 16);
            filter.xn2 = filter.xn1;
            filter.yn2 = filter.yn1;
            filter.xn1 = xn0;
            filter.yn1 = yn0;
            samples[i] = yn0;
        }
    }

    AX_NOINLINE void MixAUXSamples(uint32_t auxId, uint32_t writeAddr, uint32_t readAddr, uint16_t volume) {
        std::array<uint16_t, kAxSamplesPerFrame> ramp{};
        GenerateVolumeRamp(ramp.data(), m_lastAuxVolumes[auxId], volume, ramp.size());
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
        std::array<int, kAxSamplesPerFrame> aux{};
        for (int* buffer : dst) {
            ReadAuxInBuffer(readAddr, aux.data(), kAxSamplesPerFrame);
            readAddr += kAxSamplesPerFrame * sizeof(int32_t);
            AxMixKernels::MixAccumRamp32(buffer, aux.data(), ramp.data(), kAxSamplesPerFrame);
        }
    }

    AX_NOINLINE void UploadAUXMixLRSC(int auxId, uint32_t* addresses, uint16_t volume) {
        int* auxLeft = auxId ? m_bus[kBusAuxBL].data() : m_bus[kBusAuxAL].data();
        int* auxRight = auxId ? m_bus[kBusAuxBR].data() : m_bus[kBusAuxAR].data();
        int* auxSurround = auxId ? m_bus[kBusAuxBS].data() : m_bus[kBusAuxAS].data();
        int* auxCBuffer = auxId ? m_bus[kBusAuxCS].data() : m_bus[kBusAuxCR].data();
        WriteAuxOutBuffer(addresses[0], auxLeft, kAxSamplesPerFrame);
        WriteAuxOutBuffer(addresses[0] + kAxSamplesPerFrame * sizeof(int32_t), auxRight, kAxSamplesPerFrame);
        WriteAuxOutBuffer(addresses[0] + 2 * kAxSamplesPerFrame * sizeof(int32_t), auxSurround, kAxSamplesPerFrame);
        WriteAuxOutBuffer(addresses[1], auxCBuffer, kAxSamplesPerFrame);
        std::array<uint16_t, kAxSamplesPerFrame> ramp{};
        GenerateVolumeRamp(ramp.data(), m_lastAuxVolumes[auxId], volume, ramp.size());
        m_lastAuxVolumes[auxId] = volume;
        std::array<int*, 4> mixDest = {m_bus[kBusMainL].data(), m_bus[kBusMainR].data(),
                                       m_bus[kBusMainS].data(), m_bus[kBusAuxCL].data()};
        std::array<int, kAxSamplesPerFrame> temp{};
        for (uint32_t i = 0; i < mixDest.size(); ++i) {
            ReadAuxInBuffer(addresses[2 + i], temp.data(), kAxSamplesPerFrame);
            AxMixKernels::MixAccumRamp32(mixDest[i], temp.data(), ramp.data(),
                                         kAxSamplesPerFrame);
        }
    }

    AX_NOINLINE void OutputSamples(uint32_t lrAddr, uint32_t surroundAddr, uint16_t volume, bool uploadAuxC) {
        std::array<uint16_t, kAxSamplesPerFrame> ramp{};
        GenerateVolumeRamp(ramp.data(), m_lastMainVolume, volume, ramp.size());
        m_lastMainVolume = volume;
        WriteGuestS32Buffer(surroundAddr, m_bus[kBusMainS].data(), kAxSamplesPerFrame);
        if (uploadAuxC) {
            WriteGuestS32Buffer(surroundAddr + kAxSamplesPerFrame * sizeof(int32_t),
                                m_bus[kBusAuxCL].data(), kAxSamplesPerFrame);
        }
        std::array<int16_t, kAxSamplesPerFrame * 2> pcm{};
        for (uint32_t i = 0; i < kAxSamplesPerFrame; ++i) {
            const int16_t left = ClampS16((static_cast<int64_t>(m_bus[kBusMainL][i]) * ramp[i]) >> 15);
            const int16_t right = ClampS16((static_cast<int64_t>(m_bus[kBusMainR][i]) * ramp[i]) >> 15);
            // OUTPUT is destructive on the DSP: the main buses are left holding the ramped,
            // clamped result (not the wide accumulator) so any mixing after it sees that value.
            m_bus[kBusMainL][i] = left;
            m_bus[kBusMainR][i] = right;
            pcm[i * 2] = right;
            pcm[i * 2 + 1] = left;
        }
        // The AI frame (contiguous right/left interleaved) lands in guest memory directly
        // even from the worker: its only reader, PushAudioBlock, runs after Audio_HLE_Tick
        // has joined the worker, so the guest never observes a half-written frame.
        if (uint8_t* host = MixResolveRange(lrAddr, pcm.size() * sizeof(int16_t))) {
            MixNoteWrite(host, pcm.size() * sizeof(int16_t));
            for (size_t i = 0; i < pcm.size(); ++i) {
                BigEndian::Write16(host + i * sizeof(uint16_t), static_cast<uint16_t>(pcm[i]));
            }
        } else {
            for (uint32_t i = 0; i < kAxSamplesPerFrame; ++i) {
                MixWrite16(lrAddr + i * 4, static_cast<uint16_t>(pcm[i * 2]));
                MixWrite16(lrAddr + i * 4 + 2, static_cast<uint16_t>(pcm[i * 2 + 1]));
            }
        }
    }

    AX_NOINLINE void OutputWMSamples(uint32_t* addresses) {
        const AXBuffers all = MixBuffers();
        const std::array<int*, 4> buffers = {all[kBusWm0Main], all[kBusWm1Main], all[kBusWm2Main],
                                             all[kBusWm3Main]};
        for (uint32_t i = 0; i < buffers.size(); ++i) {
            for (uint32_t j = 0; j < kAxWiimoteSamplesPerFrame; ++j) {
                MixWrite16(addresses[i] + j * 2, static_cast<uint16_t>(ClampS16(buffers[i][j])));
            }
        }
    }

    // Aux OUT/IN stay off guest memory during a deferred mix, since __AXProcessAux reads a mix
    // result inside the same __AXOutNewFrame that produced it: OUT shadows to a host buffer
    // JoinMix publishes one AX frame later (a 3 ms older reverb tap, not a dry-signal change),
    // and IN snapshots at mail time to match a synchronous mix. Reads check this frame's shadow
    // first, so a same-buffer write/read still behaves like a synchronous read-back.
    void WriteAuxOutBuffer(uint32_t addr, const int* src, uint32_t count) {
        if (!m_deferredAux) {
            WriteGuestS32Buffer(addr, src, count);
            return;
        }
        if (addr == 0) {
            return;
        }
        if (m_auxOutCount >= m_auxOut.size()) {
            ReportAuxShadowOverflow();
            return;
        }
        AuxBlock& block = m_auxOut[m_auxOutCount++];
        block.addr = addr;
        std::memcpy(block.data.data(), src, count * sizeof(int32_t));
    }

    void ReadAuxInBuffer(uint32_t addr, int* dst, uint32_t count) {
        if (m_deferredAux) {
            for (uint32_t i = m_auxOutCount; i-- > 0;) {
                if (m_auxOut[i].addr == addr) {
                    std::memcpy(dst, m_auxOut[i].data.data(), count * sizeof(int32_t));
                    return;
                }
            }
            for (uint32_t i = 0; i < m_auxInCount; ++i) {
                if (m_auxIn[i].addr == addr) {
                    std::memcpy(dst, m_auxIn[i].data.data(), count * sizeof(int32_t));
                    return;
                }
            }
        }
        ReadGuestS32Buffer(addr, dst, count);
    }

    // Guest thread only, with the worker joined.
    void PublishAuxOutputs() {
        for (uint32_t i = 0; i < m_auxOutCount; ++i) {
            WriteGuestS32Buffer(m_auxOut[i].addr, m_auxOut[i].data.data(), kAxSamplesPerFrame);
        }
        // Publish exactly once: __AXProcessAux may process the aux buffer in
        // place, so replaying the shadow at the next join would overwrite the
        // reverb result the guest just computed.
        m_auxOutCount = 0;
    }

    void ReportAuxShadowOverflow() {
        if (m_loggedAuxShadowOverflow.exchange(true, std::memory_order_relaxed)) {
            return;
        }
        RT_LOGF(RT_TAG_AUDIO, "aux-out shadow overflow; dropped an aux upload\n");
        std::fflush(stderr);
    }

    bool EnsureMixWorker() { return true; }   // the "worker" is RunPendingMix

    void ReportMixWorkerFailure(const char* what) {
        if (m_loggedMixWorkerFailure.exchange(true, std::memory_order_relaxed)) {
            return;
        }
        RT_LOGF(RT_TAG_AUDIO, "worker aborted a command list: %s\n", what);
        std::fflush(stderr);
    }

#include "ax_mix_fast_members.inc"

    mutable std::mutex m_mutex;
    MailState m_mailState = MailState::WaitingForCmdListSize;
    bool m_toDspBusy = false;
    uint32_t m_cmdListSize = 0;
    std::array<uint16_t, 512> m_cmdList{};
    std::deque<uint32_t> m_cpuMails;
    uint32_t m_lastCpuMail = 0;
    uint32_t m_compressorPos = 0;
    uint32_t m_deferredResumeCallbacks = 0;
    uint32_t m_ucodeCrc = 0;
    PBLayout m_pbLayout = PBLayout::Unknown;
    AXCommandLayout m_commandLayout = AXCommandLayout::Auto;
    bool m_triedLoadCoeffs = false;
    bool m_hasCoeffs = false;
    std::array<int16_t, 0x800> m_coeffs{};
    std::array<bool, 4> m_coefBankFits32{};
    // Workstream AUD: which path the command list pass in progress takes; statistics off for the
    // verification's second pass.
    bool m_fastPass = false;
    bool m_statsMuted = false;
    // Indexed by AxBus. The wiimote buses live in their own array because they
    // are 18 samples per frame rather than 96.
    std::array<std::array<int, kAxSamplesPerFrame>, kAxRegularBusCount> m_bus{};
    std::array<std::array<int, kAxWiimoteSamplesPerFrame>, kAxWiimoteBusCount> m_wmBus{};
    uint16_t m_lastMainVolume = 0x8000;
    std::array<uint16_t, 3> m_lastAuxVolumes{};

    bool m_mixPending = false;
    bool m_mixWorkerEnabled = false;
    bool m_inlineNext = false; // SetMixInlineOnce
    AXCommandLayout m_mixLayout = AXCommandLayout::New;
    uint32_t m_mixCmdListSize = 0;
    std::atomic<bool> m_loggedMixWorkerFailure{false};
    std::atomic<bool> m_loggedAuxShadowOverflow{false};
    std::atomic<bool> m_loggedUnknownCommand{false};
    std::atomic<bool> m_loggedCmdListTruncated{false};

    // Aux marshalling. m_deferredAux is written by the guest thread before the
    // worker is signalled and read by whichever thread runs the mix.
    bool m_deferredAux = false;
    uint32_t m_auxOutCount = 0;
    uint32_t m_auxInCount = 0;
    std::array<AuxBlock, 24> m_auxOut{};
    std::array<AuxBlock, 24> m_auxIn{};
};

AXWii& Instance() {
    static AXWii ax;
    return ax;
}

} // namespace

void Init() {
    Instance().Initialize();
}

void ConfigureFromIram(const uint8_t* iram, uint32_t length) {
    Instance().ConfigureFromIram(iram, length);
}

void Stop() {
    Instance().Stop();
}

void SendMailToDSP(uint32_t mail) {
    Instance().SendMail(mail);
}

uint32_t CheckMailToDSP() {
    return Instance().CheckMailToDSP();
}

uint32_t CheckMailFromDSP() {
    return Instance().CheckMailFromDSP();
}

uint32_t ReadMailFromDSP() {
    return Instance().ReadMailFromDSP();
}

void QueueResumeCallback() {
    Instance().QueueResumeCallback();
}

void SetResumeCallback(void (*resume)()) {
    g_resumeCallback = resume;
}

void ServiceDeferredCallbacks() {
    Instance().ServiceDeferredCallbacks();
}

void RunPendingMix() {
    Instance().JoinMix();
}

// MSC_AX_VERIFY: called by the mix's guest-memory writers before they write (ax_internal.h).
void MixJournalRecord(uint8_t* host, size_t bytes) {
    MixJournal* journal = g_mixJournal;
    if (journal == nullptr || bytes == 0) {
        return;
    }
    journal->entries.push_back({host, static_cast<uint32_t>(bytes), static_cast<uint32_t>(journal->before.size())});
    journal->before.insert(journal->before.end(), host, host + bytes);
}

void MixJournalNoteUnjournaled() {
    if (g_mixJournal != nullptr) {
        ++g_axStats.verifyUnjournaled;
    }
}

} // namespace AxDspHle

// Workstream AUD: the mixer's own account of a run (MSC_AX_STATS=1 and/or MSC_AX_VERIFY=1),
// printed by the headless harness before it leaves (scripts/aud/plat/plat_headless.cpp) and
// callable from any platform layer.
extern "C" void MscAxPrintStats(void) {
    using namespace AxDspHle;
    const AxStats& st = g_axStats;
    if (g_axMode.verify) {
        std::fprintf(stderr, "[ax] verify: %llu command lists mixed both ways, %llu with a difference, %llu guest writes outside the journal\n",
                     static_cast<unsigned long long>(st.verifyLists), static_cast<unsigned long long>(st.verifyBad),
                     static_cast<unsigned long long>(st.verifyUnjournaled));
    }
    if (!g_axMode.stats) {
        return;
    }
    auto u = [](uint64_t v) { return static_cast<unsigned long long>(v); };
    std::fprintf(stderr, "[ax] mode: fast=%d verify=%d neon=%d\n", g_axMode.fast, g_axMode.verify ? 1 : 0,
                 (MKW_AX_MIX_NEON && g_axMode.fast == 1) ? 1 : 0);
    std::fprintf(stderr, "[ax] lists=%llu pbs=%llu running=%llu (%.2f per list of %.2f) stopped-skipped=%llu fast-voices=%llu reference-voices=%llu\n",
                 u(st.lists), u(st.pbs), u(st.running), st.lists ? double(st.running) / st.lists : 0.0,
                 st.lists ? double(st.pbs) / st.lists : 0.0, u(st.stoppedSkipped), u(st.fastVoices), u(st.referenceVoices));
    std::fprintf(stderr, "[ax] format: adpcm=%llu pcm16=%llu pcm8=%llu other=%llu | src: polyphase=%llu (bank0=%llu bank1=%llu bank2=%llu bank3=%llu) linear=%llu (of which type 0 without table %llu) nearest=%llu\n",
                 u(st.fmtAdpcm), u(st.fmtPcm16), u(st.fmtPcm8), u(st.fmtOther), u(st.src[0]), u(st.coefSel[0]),
                 u(st.coefSel[1]), u(st.coefSel[2]), u(st.coefSel[3]), u(st.src[1]), u(st.srcNoTable), u(st.src[2]));
    std::fprintf(stderr, "[ax] ratio: <=1 %llu, <=2 %llu, <=4 %llu, >4 %llu | input samples %llu (%.1f per running voice)\n",
                 u(st.ratioLe1), u(st.ratioLe2), u(st.ratioLe4), u(st.ratioMore), u(st.inputSamples),
                 st.running ? double(st.inputSamples) / st.running : 0.0);
    std::fprintf(stderr, "[ax] envelope: unity=%llu zero=%llu constant=%llu moving=%llu | lpf=%llu biquad=%llu remote=%llu updates=%llu\n",
                 u(st.envUnity), u(st.envZero), u(st.envConst), u(st.envRamp), u(st.lpf), u(st.biquad), u(st.remote), u(st.updates));
    std::fprintf(stderr, "[ax] bus mixes=%llu (%.2f per running voice): ramp bit %llu, moving %llu, volume 0 not moving %llu, volume 0x8000 not moving %llu | buses per voice:",
                 u(st.busMixes), st.running ? double(st.busMixes) / st.running : 0.0, u(st.busRampBit),
                 u(st.busRampMoving), u(st.busZeroVolume), u(st.busUnity));
    for (int i = 0; i <= 12; ++i) {
        std::fprintf(stderr, " %d:%llu", i, u(st.busesPerVoice[i]));
    }
    std::fprintf(stderr, "\n[ax] fast path: silent voices=%llu fallbacks: ratio=%llu table=%llu resolve=%llu | aux return: unity=%llu constant=%llu moving=%llu | output: unity=%llu constant=%llu moving=%llu\n",
                 u(st.silentVoices), u(st.fallbackRatio), u(st.fallbackTable), u(st.fallbackResolve), u(st.auxUnity),
                 u(st.auxConst), u(st.auxRamp), u(st.outUnity), u(st.outConst), u(st.outRamp));
    std::fprintf(stderr, "[ax] commands:");
    for (int i = 0; i < 16; ++i) {
        if (st.cmd[i]) std::fprintf(stderr, " %02X:%llu", i, u(st.cmd[i]));
    }
    std::fprintf(stderr, " | compressor: scans=%llu applied=%llu\n", u(st.compressorScans), u(st.compressorApplied));
    std::fprintf(stderr, "[ax] worker mix time (real clock): %llu mixes, mean %.1f us, longest %.1f us\n", u(st.workerMixes),
                 st.workerMixes ? st.workerMixNs / 1000.0 / st.workerMixes : 0.0, st.workerMixMaxNs / 1000.0);
    std::fprintf(stderr, "[ax] command lists: %llu to the worker, %llu mixed on the guest thread (bursts); joins by the guest thread: %llu, of which %llu found the mix unfinished and waited\n",
                 u(st.listsDeferred), u(st.listsInline), u(st.joins), u(st.joinsWaited));
}
