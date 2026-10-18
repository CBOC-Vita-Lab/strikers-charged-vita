// VLink core: sockets, frames, peer address learning, impairment. See vlink.h.
// Ported from the recompiled port's MscLink (port/runtime/src/hle/net/msc_link.cpp, branch mp-one) to an
// instance-based, lock-protected form with an injectable clock; the frame format is unchanged.
#include "platform/sdk/net/vlink.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <deque>
#include <vector>

#if defined(__linux__)
#include <sched.h>
#endif

#define VLINK_LOG(...) fprintf(stderr, "[vlink] " __VA_ARGS__)

namespace VLink {
namespace {

// Frame (big-endian), 24 bytes then the payload:
//   0 magic "MSLK"  4 version  5 reserved  6 payload length (u16)  8 fingerprint  12 src vip  16 dst vip
//   20 src port (u16)  22 dst port (u16)
const uint32_t kMagic = 0x4D534C4Bu;
// Fragment (big-endian), kFragHeader = 12 bytes then a piece of a frame: 0 magic "MSLF"  4 id (u16)  6 index (u8)  7 count (u8)
//   8 offset of the piece in the frame (u16)  10 total frame length (u16). Only frames longer than Backend::maxFrame are split.
const uint32_t kFragMagic = 0x4D534C46u;
const size_t kMaxFragments = 8;
const int kReasmSlots = 4;
const uint8_t kVersion = 1;
const size_t kHeader = 24;
const size_t kMaxFrame = kHeader + kMaxPayload;
const size_t kQueueLimit = 128;  // datagrams per socket; the game drains every frame
const int kAddrSlots = 16;

void Put16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v >> 8); p[1] = uint8_t(v); }
void Put32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v); }
uint16_t Get16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
uint32_t Get32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

uint64_t SteadyNowMs(void*) {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000u + uint64_t(ts.tv_nsec / 1000000);
}

uint64_t SteadyNowUs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000u + uint64_t(ts.tv_nsec / 1000);
}

// The critical sections are a few dozen instructions, contention is rare (the network update task and a
// startup thread): a spin lock avoids a dependency on the platform's threading library.
struct Lock {
    volatile int flag;
    Lock() : flag(0) {}
    void Acquire() {
        while (__atomic_test_and_set(&flag, __ATOMIC_ACQUIRE)) {
#if defined(__linux__)
            sched_yield();
#endif
        }
    }
    void Release() { __atomic_clear(&flag, __ATOMIC_RELEASE); }
};
struct Guard {
    Lock& l;
    explicit Guard(Lock& lock) : l(lock) { l.Acquire(); }
    ~Guard() { l.Release(); }
};

struct Datagram {
    uint32_t srcVip;
    uint16_t srcPort;
    std::vector<uint8_t> data;
};

struct Socket {
    bool open;
    uint16_t port;
    std::deque<Datagram> queue;
    Socket() : open(false), port(0) {}
};

struct AddrEntry {
    bool used;
    uint32_t vip;
    PeerAddr addr;
    AddrEntry() : used(false), vip(0) { memset(&addr, 0, sizeof addr); }
};

struct Reasm {
    bool used;
    PeerAddr from;
    uint16_t id, total;
    uint8_t count;
    uint32_t mask;
    uint64_t startedMs;
    uint8_t data[kMaxFrame];
    Reasm() : used(false), id(0), total(0), count(0), mask(0), startedMs(0) { memset(&from, 0, sizeof from); }
};

struct Delayed {
    uint64_t dueMs;
    bool broadcast, hasTo;
    PeerAddr to;
    std::vector<uint8_t> frame;
};

}  // namespace

struct LinkImpl {
    mutable Lock lock;
    const Backend* backend;
    bool up;
    uint32_t vip, fingerprint;
    Socket sockets[kMaxSockets];
    AddrEntry addrs[kAddrSlots];
    int nextAddr;
    uint16_t nextEphemeral;
    Stats stats;
    Impairment impairment;
    uint32_t rng;
    std::deque<Delayed> delayed;
    Reasm* reasm;  // kReasmSlots, allocated on first use (the frame buffers are large)
    uint16_t nextFragId;
    uint64_t (*nowMs)(void*);
    void* nowCtx;
    int trace;
    WindowStats window;
    bool lost;

    LinkImpl()
        : backend(0), up(false), vip(0), fingerprint(0), nextAddr(0), nextEphemeral(49152), rng(12345u), reasm(0), nextFragId(1),
          nowMs(SteadyNowMs), nowCtx(0), trace(0), lost(false) {
        memset(&stats, 0, sizeof stats);
        memset(&impairment, 0, sizeof impairment);
        memset(&window, 0, sizeof window);
    }

    // Every send of the backend goes through here: timed (wave 5 NET instrumentation).
    int BackendSend(const PeerAddr* to, bool broadcast, const void* data, size_t len) {
        const uint64_t t0 = SteadyNowUs();
        const int rc = backend->send(backend->ctx, to, broadcast, data, len);
        const uint64_t us = SteadyNowUs() - t0;
        const uint32_t us32 = us > 0xFFFFFFFFu ? 0xFFFFFFFFu : uint32_t(us);
        ++stats.backendSends;
        stats.sendUsTotal += us;
        if (us32 > stats.sendUsMax) stats.sendUsMax = us32;
        if (us32 > window.sendUsMax) window.sendUsMax = us32;
        return rc;
    }
    ~LinkImpl() { delete[] reasm; }

    bool Tracing() {
        if (trace <= 0) return false;
        --trace;
        return true;
    }
    double Uniform() {  // xorshift32, deterministic per console
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        return double(rng) / 4294967296.0;
    }
    bool Impaired() const { return impairment.drop > 0 || impairment.dup > 0 || impairment.delayMs || impairment.jitterMs; }

    Socket* Get(int handle) {
        if (handle < 1 || handle > kMaxSockets) return 0;
        Socket& s = sockets[handle - 1];
        return s.open ? &s : 0;
    }
    const PeerAddr* Lookup(uint32_t v) const {
        for (int i = 0; i < kAddrSlots; ++i)
            if (addrs[i].used && addrs[i].vip == v) return &addrs[i].addr;
        return 0;
    }
    void Learn(uint32_t v, const PeerAddr& addr) {
        for (int i = 0; i < kAddrSlots; ++i) {
            AddrEntry& e = addrs[i];
            if (e.used && e.vip == v) {
                if (memcmp(&e.addr, &addr, sizeof addr) != 0) {
                    e.addr = addr;
                    VLINK_LOG("peer %u.%u.%u.%u moved\n", v >> 24, (v >> 16) & 255, (v >> 8) & 255, v & 255);
                }
                return;
            }
        }
        AddrEntry& slot = addrs[nextAddr];
        nextAddr = (nextAddr + 1) % kAddrSlots;
        slot.used = true;
        slot.vip = v;
        slot.addr = addr;
        VLINK_LOG("learned peer %u.%u.%u.%u\n", v >> 24, (v >> 16) & 255, (v >> 8) & 255, v & 255);
    }
    Socket* FindBound(uint16_t port) {
        for (int i = 0; i < kMaxSockets; ++i)
            if (sockets[i].open && sockets[i].port == port) return &sockets[i];
        return 0;
    }

    void Deliver(const uint8_t* frame, size_t len, const PeerAddr& from) {
        if (len < kHeader || Get32(frame) != kMagic || frame[4] != kVersion) {
            ++stats.droppedMalformed;
            return;
        }
        const uint16_t payload = Get16(frame + 6);
        if (payload > kMaxPayload || kHeader + payload != len) {
            ++stats.droppedMalformed;
            return;
        }
        if (Get32(frame + 8) != fingerprint) {
            ++stats.droppedFingerprint;
            static bool warned = false;
            if (!warned) {
                warned = true;
                VLINK_LOG("ignoring a console running a different build or settings (fingerprint %08X, ours %08X)\n",
                          (unsigned)Get32(frame + 8), (unsigned)fingerprint);
            }
            return;
        }
        const uint32_t src = Get32(frame + 12), dst = Get32(frame + 16);
        if (src == vip || src == 0 || src == kBroadcastVip) return;  // our own broadcast echo, or garbage
        if (dst != vip && dst != kBroadcastVip) return;
        Learn(src, from);
        Socket* socket = FindBound(Get16(frame + 22));
        if (Tracing())
            VLINK_LOG("rx %u.%u.%u.%u:%u -> %s:%u len %u%s\n", src >> 24, (src >> 16) & 255, (src >> 8) & 255, src & 255,
                      Get16(frame + 20), dst == kBroadcastVip ? "bcast" : "me", Get16(frame + 22), payload,
                      socket ? "" : " (no socket)");
        if (!socket) {
            ++stats.droppedNoSocket;
            return;
        }
        if (socket->queue.size() >= kQueueLimit) {
            ++stats.droppedQueueFull;
            socket->queue.pop_front();
        }
        Datagram d;
        d.srcVip = src;
        d.srcPort = Get16(frame + 20);
        d.data.assign(frame + kHeader, frame + len);
        socket->queue.push_back(d);
        ++stats.framesReceived;
        stats.bytesReceived += payload;
        const uint32_t depth = uint32_t(socket->queue.size());
        if (depth > stats.queueHighWater) stats.queueHighWater = depth;
        if (depth > window.queueHighWater) window.queueHighWater = depth;
        if (payload > window.payloadReceivedMax) window.payloadReceivedMax = payload;
    }

    // The one place a frame leaves for the backend: whole when it fits the medium (Backend::maxFrame), else in fragments. Returns the frame
    // length, or <0 when the backend refused a piece.
    int SendFrame(const PeerAddr* to, bool broadcast, const uint8_t* frame, size_t len) {
        const size_t limit = backend->maxFrame;
        if (len > stats.maxBackendFrame) stats.maxBackendFrame = uint32_t(len > limit && limit ? limit : len);
        if (!limit || len <= limit) return BackendSend(to, broadcast, frame, len);
        const size_t chunk = limit - kFragHeader;
        const size_t count = (len + chunk - 1) / chunk;
        if (limit <= kFragHeader || count > kMaxFragments) return -1;
        const uint16_t id = nextFragId++;
        if (nextFragId == 0) nextFragId = 1;
        ++stats.framesSplit;
        uint8_t piece[kMaxFrame];
        int result = int(len);
        for (size_t i = 0; i < count; ++i) {
            const size_t offset = i * chunk, n = (len - offset < chunk) ? len - offset : chunk;
            Put32(piece, kFragMagic);
            Put16(piece + 4, id);
            piece[6] = uint8_t(i);
            piece[7] = uint8_t(count);
            Put16(piece + 8, uint16_t(offset));
            Put16(piece + 10, uint16_t(len));
            memcpy(piece + kFragHeader, frame + offset, n);
            ++stats.fragmentsSent;
            if (BackendSend(to, broadcast, piece, kFragHeader + n) < 0) result = -1;
        }
        return result;
    }

    // A fragment arrived: file it under (sender, id); when the frame is complete, deliver it as if it had come whole.
    void HandleFragment(const uint8_t* piece, size_t len, const PeerAddr& from) {
        if (len <= kFragHeader) { ++stats.droppedFragments; return; }
        const uint16_t id = Get16(piece + 4), offset = Get16(piece + 8), total = Get16(piece + 10);
        const unsigned index = piece[6], count = piece[7];
        const size_t n = len - kFragHeader;
        if (count < 2 || count > kMaxFragments || index >= count || total < kHeader || total > kMaxFrame || size_t(offset) + n > total) {
            ++stats.droppedFragments;
            return;
        }
        if (!reasm) reasm = new Reasm[kReasmSlots];
        const uint64_t now = nowMs(nowCtx);
        Reasm* slot = 0;
        for (int i = 0; i < kReasmSlots; ++i) {
            Reasm& r = reasm[i];
            if (r.used && now - r.startedMs > kReassemblyMs) { r.used = false; ++stats.droppedFragments; }  // never completed
            if (r.used && r.id == id && memcmp(&r.from, &from, sizeof from) == 0) slot = &r;
        }
        if (!slot) {
            for (int i = 0; i < kReasmSlots && !slot; ++i)
                if (!reasm[i].used) slot = &reasm[i];
            if (!slot) {  // all busy: replace the oldest
                slot = &reasm[0];
                for (int i = 1; i < kReasmSlots; ++i)
                    if (reasm[i].startedMs < slot->startedMs) slot = &reasm[i];
                ++stats.droppedFragments;
            }
            slot->used = true;
            slot->from = from;
            slot->id = id;
            slot->total = total;
            slot->count = uint8_t(count);
            slot->mask = 0;
            slot->startedMs = now;
        }
        if (slot->total != total || slot->count != count) { slot->used = false; ++stats.droppedFragments; return; }
        memcpy(slot->data + offset, piece + kFragHeader, n);
        slot->mask |= 1u << index;  // a duplicate fragment sets the same bit again
        if (slot->mask != (1u << count) - 1) return;
        slot->used = false;
        ++stats.framesReassembled;
        Deliver(slot->data, slot->total, from);
    }

    void ReleaseDelayed() {
        if (delayed.empty()) return;
        const uint64_t now = nowMs(nowCtx);
        // Jitter can reorder: release every due frame, in due-time order.
        for (;;) {
            size_t best = delayed.size();
            for (size_t i = 0; i < delayed.size(); ++i)
                if (delayed[i].dueMs <= now && (best == delayed.size() || delayed[i].dueMs < delayed[best].dueMs)) best = i;
            if (best == delayed.size()) break;
            Delayed d = delayed[best];
            delayed.erase(delayed.begin() + best);
            if (SendFrame(d.hasTo ? &d.to : 0, d.broadcast, &d.frame[0], d.frame.size()) < 0) ++stats.sendErrors;
        }
    }

    void PumpLocked() {
        if (!up) return;
        ReleaseDelayed();
        uint8_t frame[kMaxFrame + 64];
        for (int budget = 0; budget < 256; ++budget) {
            PeerAddr from;
            memset(&from, 0, sizeof from);
            const int n = backend->recv(backend->ctx, &from, frame, sizeof frame);
            if (n <= 0) break;
            if (size_t(n) >= 4 && Get32(frame) == kFragMagic) HandleFragment(frame, size_t(n), from);
            else Deliver(frame, size_t(n), from);
        }
    }
};

Link::Link() : m(new LinkImpl) {}
Link::~Link() {
    Stop();
    delete m;
}

void Link::SetBackend(const Backend* backend) { Guard g(m->lock); m->backend = backend; }
const Backend* Link::GetBackend() const { Guard g(m->lock); return m->backend; }
void Link::SetFingerprint(uint32_t fingerprint) { Guard g(m->lock); m->fingerprint = fingerprint; }
uint32_t Link::Fingerprint() const { Guard g(m->lock); return m->fingerprint; }
void Link::SetClock(uint64_t (*nowMs)(void*), void* ctx) {
    Guard g(m->lock);
    m->nowMs = nowMs ? nowMs : SteadyNowMs;
    m->nowCtx = ctx;
}
void Link::SetImpairment(const Impairment& impairment, uint32_t seed) {
    Guard g(m->lock);
    m->impairment = impairment;
    if (seed) m->rng = seed;
    if (m->Impaired())
        VLINK_LOG("test impairment: drop %.1f%% dup %.1f%% delay %u ms jitter %u ms\n", impairment.drop * 100, impairment.dup * 100,
                  (unsigned)impairment.delayMs, (unsigned)impairment.jitterMs);
}
void Link::SetTrace(int frames) { Guard g(m->lock); m->trace = frames; }

bool Link::Start() {
    Guard g(m->lock);
    if (m->up) return true;
    if (!m->backend) {
        VLINK_LOG("no network backend available\n");
        return false;
    }
    m->vip = m->backend->local_vip(m->backend->ctx);
    if (m->vip == 0 || m->vip == kBroadcastVip) {
        VLINK_LOG("backend %s has no usable address\n", m->backend->name);
        return false;
    }
    for (int i = 0; i < kAddrSlots; ++i) m->addrs[i] = AddrEntry();
    for (int i = 0; i < kMaxSockets; ++i) m->sockets[i] = Socket();
    m->delayed.clear();
    if (m->reasm)
        for (int i = 0; i < kReasmSlots; ++i) m->reasm[i].used = false;
    memset(&m->stats, 0, sizeof m->stats);
    memset(&m->window, 0, sizeof m->window);
    m->lost = false;
    m->up = true;
    VLINK_LOG("up on %s as %u.%u.%u.%u, fingerprint %08X\n", m->backend->name, m->vip >> 24, (m->vip >> 16) & 255,
              (m->vip >> 8) & 255, m->vip & 255, (unsigned)m->fingerprint);
    return true;
}

void Link::Stop() {
    Guard g(m->lock);
    if (!m->up) return;
    for (int i = 0; i < kMaxSockets; ++i) m->sockets[i] = Socket();
    m->delayed.clear();
    if (m->backend && m->backend->close) m->backend->close(m->backend->ctx);
    m->up = false;
    const Stats& s = m->stats;
    VLINK_LOG("down (sent %llu frames, received %llu, dropped fp=%llu bad=%llu nosock=%llu full=%llu, send errors %llu; largest guest payload %u, "
              "largest frame to the backend %u, split %llu into %llu fragments, reassembled %llu, fragments dropped %llu)\n",
              (unsigned long long)s.framesSent, (unsigned long long)s.framesReceived, (unsigned long long)s.droppedFingerprint,
              (unsigned long long)s.droppedMalformed, (unsigned long long)s.droppedNoSocket, (unsigned long long)s.droppedQueueFull,
              (unsigned long long)s.sendErrors, (unsigned)s.maxPayloadSent, (unsigned)s.maxBackendFrame, (unsigned long long)s.framesSplit,
              (unsigned long long)s.fragmentsSent, (unsigned long long)s.framesReassembled, (unsigned long long)s.droppedFragments);
    VLINK_LOG("down: %llu backend sends, %.1f us average, %u us the longest; most datagrams waiting in one socket %u; sends refused while lost %llu\n",
              (unsigned long long)s.backendSends, s.backendSends ? double(s.sendUsTotal) / double(s.backendSends) : 0.0, (unsigned)s.sendUsMax,
              (unsigned)s.queueHighWater, (unsigned long long)s.sendsRefusedLost);
}

bool Link::IsUp() const { Guard g(m->lock); return m->up; }
uint32_t Link::LocalVip() const { Guard g(m->lock); return m->up ? m->vip : 0; }

int Link::OpenSocket() {
    Guard g(m->lock);
    if (!m->up) return -1;
    for (int i = 0; i < kMaxSockets; ++i) {
        if (!m->sockets[i].open) {
            m->sockets[i] = Socket();
            m->sockets[i].open = true;
            return i + 1;
        }
    }
    return -1;
}

bool Link::Bind(int handle, uint16_t port) {
    Guard g(m->lock);
    Socket* s = m->Get(handle);
    if (!s) return false;
    if (port == 0) {
        do {
            port = m->nextEphemeral++;
            if (m->nextEphemeral == 0) m->nextEphemeral = 49152;
        } while (m->FindBound(port));
    } else {
        Socket* other = m->FindBound(port);
        if (other && other != s) return false;
    }
    s->port = port;
    return true;
}

void Link::Close(int handle) {
    Guard g(m->lock);
    if (Socket* s = m->Get(handle)) *s = Socket();
}

bool Link::IsOpen(int handle) const { Guard g(m->lock); return m->Get(handle) != 0; }

uint16_t Link::LocalPort(int handle) const {
    Guard g(m->lock);
    const Socket* s = m->Get(handle);
    return s ? s->port : 0;
}

int Link::SendTo(int handle, uint32_t dstVip, uint16_t dstPort, const void* data, size_t len) {
    Guard g(m->lock);
    Socket* s = m->Get(handle);
    if (!s || !m->up || len > kMaxPayload || (len && !data)) return -1;
    if (m->lost) {
        ++m->stats.sendsRefusedLost;
        return -1;
    }
    if (len > m->window.payloadSentMax) m->window.payloadSentMax = uint32_t(len);
    if (s->port == 0) {
        // implicit bind, as the BSD stack does on the first send
        uint16_t port;
        do {
            port = m->nextEphemeral++;
            if (m->nextEphemeral == 0) m->nextEphemeral = 49152;
        } while (m->FindBound(port));
        s->port = port;
    }
    uint8_t frame[kMaxFrame];
    Put32(frame, kMagic);
    frame[4] = kVersion;
    frame[5] = 0;
    Put16(frame + 6, uint16_t(len));
    Put32(frame + 8, m->fingerprint);
    Put32(frame + 12, m->vip);
    Put32(frame + 16, dstVip);
    Put16(frame + 20, s->port);
    Put16(frame + 22, dstPort);
    if (len) memcpy(frame + kHeader, data, len);
    const bool broadcast = dstVip == kBroadcastVip;
    const PeerAddr* to = broadcast ? 0 : m->Lookup(dstVip);
    if (m->Impaired()) {
        if (len > m->stats.maxPayloadSent) m->stats.maxPayloadSent = uint32_t(len);
        ++m->stats.framesSent;
        m->stats.bytesSent += len;
        if (m->Uniform() < m->impairment.drop) return int(len);  // lost in the air
        const int copies = m->Uniform() < m->impairment.dup ? 2 : 1;
        for (int c = 0; c < copies; ++c) {
            Delayed d;
            d.dueMs = m->nowMs(m->nowCtx) + m->impairment.delayMs + uint64_t(m->Uniform() * m->impairment.jitterMs);
            d.broadcast = broadcast || !to;
            d.hasTo = to != 0;
            memset(&d.to, 0, sizeof d.to);
            if (to) d.to = *to;
            d.frame.assign(frame, frame + kHeader + len);
            m->delayed.push_back(d);
        }
        m->PumpLocked();
        return int(len);
    }
    // Unknown unicast destinations go out as broadcast frames; only the addressed console keeps them.
    if (len > m->stats.maxPayloadSent) m->stats.maxPayloadSent = uint32_t(len);
    const int sent = m->SendFrame(to, broadcast || !to, frame, kHeader + len);
    if (m->Tracing())
        VLINK_LOG("tx :%u -> %u.%u.%u.%u:%u len %u%s%s\n", s->port, dstVip >> 24, (dstVip >> 16) & 255, (dstVip >> 8) & 255,
                  dstVip & 255, dstPort, unsigned(len), (!broadcast && !to) ? " (unknown peer, broadcast)" : "",
                  sent < 0 ? " FAILED" : "");
    if (sent < 0) {
        ++m->stats.sendErrors;
        return -1;
    }
    ++m->stats.framesSent;
    m->stats.bytesSent += len;
    return int(len);
}

int Link::RecvFrom(int handle, void* buffer, size_t capacity, uint32_t* srcVip, uint16_t* srcPort) {
    Guard g(m->lock);
    Socket* s = m->Get(handle);
    if (!s) return -1;
    m->PumpLocked();
    if (s->queue.empty()) return -1;
    Datagram d = s->queue.front();
    s->queue.pop_front();
    const size_t n = d.data.size() < capacity ? d.data.size() : capacity;  // UDP truncates
    if (n) memcpy(buffer, &d.data[0], n);
    if (srcVip) *srcVip = d.srcVip;
    if (srcPort) *srcPort = d.srcPort;
    return int(n);
}

void Link::Pump() {
    Guard g(m->lock);
    m->PumpLocked();
}

void Link::ReleaseDue() {
    Guard g(m->lock);
    if (m->up) m->ReleaseDelayed();
}

Stats Link::GetStats() const {
    Guard g(m->lock);
    return m->stats;
}

WindowStats Link::TakeWindow() {
    Guard g(m->lock);
    const WindowStats w = m->window;
    memset(&m->window, 0, sizeof m->window);
    return w;
}

void Link::SetLost(bool lost, const char* why) {
    Guard g(m->lock);
    if (m->lost == lost) return;
    m->lost = lost;
    if (lost) VLINK_LOG("link declared lost (%s): every send fails from now on\n", why ? why : "no reason given");
}

bool Link::IsLost() const { Guard g(m->lock); return m->lost; }

Link& Default() {
    static Link link;
    return link;
}

}  // namespace VLink
