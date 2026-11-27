// The game's friend calls (DWC_*Friend*, the friend status, the friend key) on top of the online service's friends (np_online_friends.h), and that
// state machine itself. The original game (a Nintendo WFC title) has its own friend roster, friend-key entry and "host inviting player" screens in
// Online Central; they call the functions below, which until 1.3 wave 10 were empty stubs, so the roster always looked empty.
//
//   friend key      12 digits: the profile id plus a 7-bit CRC of it and the game code (dwc_account.c, DWC_Acc_CreateFriendKey): the game shows and
//                   takes it exactly as on the original service. The service identifies players by profile id; the key is decoded here.
//   friend list     the game's save holds 64 DWCFriendData. UpdateServersAsync sends the entries the service does not know (an add by key) and reads
//                   the answer: an entry that is mutual becomes a "buddy" (the buddy callback), the others keep their type; every friend's presence
//                   and status string reach the status callback (presence: 0 offline 1 online 2 playing 3 looking for anybody 4 friend match).
//   status string  DWC_SetOwnStatusData: the game's own friend status (is it available, is it hosting, who for ...) as DWC's base64; the service
//                   relays it to friends untouched.
//   traffic        one held poll while the online menu runs (np_online_friends.h, PROTOCOL.md "Held polls"): no polling loop of its own beyond that.
#include <dwc/dwc_account.h>
#include <dwc/dwc_base64.h>
#include <dwc/dwc_friend.h>
#include <dwc/dwc_main_fwd.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <deque>
#include <string>
#include <vector>

#include "platform/sdk/dwc/np_dwc.h"
#include "platform/sdk/dwc/np_online_api.h"
#include "platform/sdk/dwc/np_online_friends.h"
#include "platform/sdk/dwc/np_friends_policy.h"
#include "platform/sdk/net/online/np_service_busy.h"

#define FRIEND_LOG(...) fprintf(stderr, "[friends] " __VA_ARGS__)

extern "C" int NpRankedBusy(void) __attribute__((weak));   // np_dwc_match.cpp: a match is being made or played

using namespace NpOnline;

// ---- DWC's own base64 (dwc_base64.c): its alphabet ends in ".-" and the padding is '*' ---------------------------------------------------------
static const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789.-";

extern "C" int DWC_Base64Encode(const char* src, u32 srclen, char* dst, u32 dstlen) {
    const u32 size = (srclen / 3) * 4 + (srclen % 3 != 0 ? 4 : 0);
    if (!dst) return int(size);
    if (dstlen < size) return -1;
    const unsigned char* s = reinterpret_cast<const unsigned char*>(src);
    char* d = dst;
    for (u32 i = 0; i < srclen; i += 3) {
        const u32 remain = srclen - i;
        unsigned char w[3] = {0, 0, 0};
        for (u32 k = 0; k < 3 && k < remain; ++k) w[k] = s[i + k];
        const u32 count = (remain * 8) / 6 + (((remain * 8) % 6) != 0);
        d[0] = kB64[w[0] >> 2];
        d[1] = count >= 2 ? kB64[((w[0] & 3) << 4) | (w[1] >> 4)] : '*';
        d[2] = count >= 3 ? kB64[((w[1] & 15) << 2) | (w[2] >> 6)] : '*';
        d[3] = count >= 4 ? kB64[w[2] & 63] : '*';
        d += 4;
    }
    return int(d - dst);
}

extern "C" int DWC_Base64Decode(const char* src, u32 srclen, char* dst, u32 dstlen) {
    if (srclen % 4 != 0) return -1;
    int bits = 0;
    for (u32 i = 0; i < srclen; ++i) if (src[i] != '*') bits += 6;
    const int size = bits / 8;
    if (!dst) return size;
    if (int(dstlen) < size) return -1;
    if (srclen == 0) { *dst = 0; return 0; }
    int out = 0;
    for (u32 i = 0; i < srclen && out < size; i += 4) {
        unsigned char w[4];
        for (int j = 0; j < 4; ++j) {
            const char c = src[i + j];
            w[j] = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52 : c == '.' ? 62 : c == '-' ? 63 : 0;
        }
        dst[out++] = char((w[0] << 2) | (w[1] >> 4));
        if (out >= size) break;
        dst[out++] = char((w[1] << 4) | (w[2] >> 2));
        if (out >= size) break;
        dst[out++] = char((w[2] << 6) | w[3]);
    }
    return size;
}

// ---- account-level friend data (dwc_account.c): friend keys and the friend data union ------------------------------------------------------------
namespace {

u32 FlagsOf(const DWCAccFlag& f) { return (f.flags >> DWC_ACC_FLAGS_SHIFT) & DWC_ACC_FLAGS_MASK; }
u32 TypeOf(const DWCAccFriendData* d) { return FlagsOf(d->flags) & DWC_ACC_FRIENDDATA_MASK; }
void SetFlags(DWCAccFlag* f, u32 flags) { f->flags = (f->flags & ~(DWC_ACC_FLAGS_MASK << DWC_ACC_FLAGS_SHIFT)) | ((flags & DWC_ACC_FLAGS_MASK) << DWC_ACC_FLAGS_SHIFT); }
void SetType(DWCAccFriendData* d, u32 type) { SetFlags(&d->flags, (FlagsOf(d->flags) & ~u32(DWC_ACC_FRIENDDATA_MASK)) | type); }
u64 KeyOf(const DWCAccFriendKey& k) { return (u64(k.friendkey_hi32) << 32) | k.friendkey_lo32; }

unsigned char Crc8(const void* data, size_t size) {   // MATH_CalcCRC8: polynomial 0x07, initial value 0
    const unsigned char* p = static_cast<const unsigned char*>(data);
    unsigned crc = 0;
    for (size_t i = 0; i < size; ++i) {
        crc ^= p[i];
        for (int b = 0; b < 8; ++b) crc = (crc & 0x80) ? ((crc << 1) ^ 0x07) & 0xFF : (crc << 1) & 0xFF;
    }
    return static_cast<unsigned char>(crc);
}

void LittleEndian(const u32* in, unsigned char* out, int words) {
    for (int i = 0; i < words; ++i) for (int k = 0; k < 4; ++k) out[i * 4 + k] = static_cast<unsigned char>(in[i] >> (8 * k));
}

}  // namespace

extern "C" u64 DWC_Acc_CreateFriendKey(int profile, u32 gamecode) {
    const u32 code[2] = {u32(profile), gamecode};
    unsigned char bytes[8];
    LittleEndian(code, bytes, 2);
    const u64 crc = Crc8(bytes, 8) & 0x7f;
    return (crc << 32) | u32(profile);
}
extern "C" BOOL DWC_Acc_CheckFriendKey(u64 key, u32 gamecode) {
    if (key & 0x80000000u) return FALSE;   // negative profile ids are invalid
    const u32 code[2] = {u32(key), gamecode};
    unsigned char bytes[8];
    LittleEndian(code, bytes, 2);
    return u64(Crc8(bytes, 8) & 0x7f) == (key >> 32) ? TRUE : FALSE;
}
extern "C" int DWC_Acc_FriendKeyToGsProfileId(u64 key, u32 gamecode) { return DWC_Acc_CheckFriendKey(key, gamecode) ? int(key & 0xffffffffu) : 0; }
extern "C" void DWC_Acc_FriendKeyToNumericKey(u64 key, char* out) { snprintf(out, DWC_ACC_FRIENDKEY_STRING_BUFSIZE, "%012llu", static_cast<unsigned long long>(key)); }
extern "C" u64 DWC_Acc_NumericKeyToFriendKey(const char* text) {
    u64 v = 0;
    for (int i = 0; i < DWC_ACC_FRIENDKEY_STRING_LENGTH && text[i]; ++i) v = v * 10 + u64(text[i] - '0');
    return v;
}
extern "C" u64 DWC_CreateFriendKey(const DWCAccUserData* user) { return user && user->gs_profile_id != 0 ? DWC_Acc_CreateFriendKey(user->gs_profile_id, user->gamecode) : 0; }
extern "C" BOOL DWC_CheckFriendKey(const DWCAccUserData* user, u64 key) { return user ? DWC_Acc_CheckFriendKey(key, user->gamecode) : FALSE; }
extern "C" void DWC_CreateFriendKeyToken(DWCAccFriendData* token, u64 key) {
    memset(token, 0, sizeof *token);
    token->friend_key.friendkey_lo32 = u32(key);
    token->friend_key.friendkey_hi32 = u32(key >> 32);
    SetType(token, DWC_ACC_FRIENDDATA_FRIEND_KEY);
}
extern "C" void DWC_SetGsProfileId(DWCAccFriendData* d, int profile) {
    memset(d, 0, sizeof *d);
    d->gs_profile_id.id = profile;
    SetType(d, DWC_ACC_FRIENDDATA_GS_PROFILE_ID);
}
extern "C" BOOL DWC_IsValidFriendData(const DWCAccFriendData* d) { return d && TypeOf(d) != DWC_ACC_FRIENDDATA_NODATA ? TRUE : FALSE; }
extern "C" int DWC_GetFriendDataType(const DWCAccFriendData* d) { return d ? int(TypeOf(d)) : 0; }
extern "C" BOOL DWC_IsBuddyFriendData(const DWCAccFriendData* d) {
    return d && TypeOf(d) == DWC_ACC_FRIENDDATA_GS_PROFILE_ID && (FlagsOf(d->flags) & DWC_ACC_FRIENDDATA_ISBUDDY) ? TRUE : FALSE;
}
extern "C" void DWCi_SetBuddyFriendData(DWCAccFriendData* d) {
    if (d && TypeOf(d) == DWC_ACC_FRIENDDATA_GS_PROFILE_ID) SetFlags(&d->flags, (FlagsOf(d->flags) & ~u32(DWC_ACC_FRIENDDATA_ISBUDDY_MASK)) | DWC_ACC_FRIENDDATA_ISBUDDY);
}
extern "C" void DWC_ClearBuddyFlagFriendData(DWCAccFriendData* d) {
    if (d && TypeOf(d) == DWC_ACC_FRIENDDATA_GS_PROFILE_ID) SetFlags(&d->flags, FlagsOf(d->flags) & ~u32(DWC_ACC_FRIENDDATA_ISBUDDY_MASK));
}
extern "C" u64 DWC_GetFriendKey(const DWCAccFriendData* d) { return d && TypeOf(d) == DWC_ACC_FRIENDDATA_FRIEND_KEY ? KeyOf(d->friend_key) : 0; }
extern "C" int DWC_GetGsProfileId(const DWCAccUserData* user, const DWCAccFriendData* d) {
    if (!d) return 0;
    switch (TypeOf(d)) {
    case DWC_ACC_FRIENDDATA_FRIEND_KEY: return user ? DWC_Acc_FriendKeyToGsProfileId(KeyOf(d->friend_key), user->gamecode) : 0;
    case DWC_ACC_FRIENDDATA_GS_PROFILE_ID: return d->gs_profile_id.id;
    case DWC_ACC_FRIENDDATA_LOGIN_ID: return -1;
    default: return 0;
    }
}
extern "C" BOOL DWC_IsEqualFriendData(const DWCAccFriendData* a, const DWCAccFriendData* b) {
    if (!a || !b) return FALSE;
    const u32 ta = TypeOf(a), tb = TypeOf(b);
    if (ta != tb) return FALSE;
    if (ta == DWC_ACC_FRIENDDATA_GS_PROFILE_ID) return a->gs_profile_id.id == b->gs_profile_id.id ? TRUE : FALSE;
    if (ta == DWC_ACC_FRIENDDATA_LOGIN_ID) return a->login_id.userid_lo32 == b->login_id.userid_lo32 && a->login_id.playerid == b->login_id.playerid ? TRUE : FALSE;
    if (ta == DWC_ACC_FRIENDDATA_FRIEND_KEY) return KeyOf(a->friend_key) == KeyOf(b->friend_key) ? TRUE : FALSE;
    return FALSE;
}

extern "C" uint32_t NpVitaInputActivity(void) __attribute__((weak));   // np_vita_input.cpp: advances on every poll that saw a button, stick or touch (absent on a host)

// ---- the state machine --------------------------------------------------------------------------------------------------------------------------
namespace {

const uint32_t kHoldSec = 45;               // the held friends poll (PROTOCOL.md): one request per 45 s while nothing changes
const uint32_t kHeldGapMs = 250, kPlainGapMs = 5000, kFailGapMs = 15000, kOpQueueMax = 16;

// O4 (np_friends_policy.h): Galaxy's pacing rules behind three switches, all off by default (0).
const NpFriendsPolicy::Config& PolicyConfig() {
    static const NpFriendsPolicy::Config c = [] {
        NpFriendsPolicy::Config k;
        const char* a = getenv("MSC_FRIENDS_IDLE_STOP_S"); const char* b = getenv("MSC_FRIENDS_MAX_REQS"); const char* d = getenv("MSC_FRIENDS_MIN_GAP_S");
        auto num = [](const char* v, long hi) { const long n = v && *v ? atol(v) : 0; return uint32_t(n < 0 ? 0 : (n > hi ? hi : n)); };
        k.idleStopMs = num(a, 3600) * 1000u;
        k.maxReqs = num(b, 1000);
        k.minGapMs = num(d, 600) * 1000u;
        return k;
    }();
    return c;
}
NpFriendsPolicy::Gate g_gate;
uint32_t g_manualRefreshes = 0;

enum class Req { None, Friends, Played, Op };

struct Engine {
    bool running, paused, haveSnap;
    FriendsReply snap;
    char rev[16];
    uint32_t revision;
    Req req;
    uint32_t reqId;
    uint64_t nextAt;
    uint32_t failStreak, scalePct;
    bool forceFull;
    std::deque<FriendOpRequest> ops;
    FriendOpRequest curOp;
    NpFriends::OpResult last;
    bool playedWanted, playedReady;
    std::vector<PlayedRow> played;
    char status[kStatusChars];
    bool statusDirty;
    // the game's friend list
    DWCFriendData* list;
    int listLen;
    DWCBuddyFriendCallback buddyCb; void* buddyParam;
    DWCFriendStatusCallback statusCb; void* statusParam;
    DWCUpdateServersCallback updateCb; void* updateParam;
    DWCDeleteFriendListCallback deleteCb; void* deleteParam;
    bool syncing;
    std::vector<int32_t> pushed;            // pids already sent in this sync
    std::vector<unsigned> lastSent;         // per list index: hash of the (presence, status) the status callback last got; 0 = none
    Engine() { Clear(); }
    void Clear() {
        running = paused = haveSnap = false;
        snap = FriendsReply();
        rev[0] = 0;
        revision = 0;
        req = Req::None; reqId = 0; nextAt = 0; failStreak = 0; scalePct = 100; forceFull = true;
        ops.clear(); last = NpFriends::OpResult();
        playedWanted = playedReady = false; played.clear();
        status[0] = 0; statusDirty = false;
        list = nullptr; listLen = 0; buddyCb = nullptr; buddyParam = nullptr; statusCb = nullptr; statusParam = nullptr; updateCb = nullptr; updateParam = nullptr; deleteCb = nullptr; deleteParam = nullptr;
        syncing = false; pushed.clear(); lastSent.clear();
    }
};

Engine g;
bool g_pumpSet = false;

unsigned Hash(const FriendRow& r) {
    unsigned h = 2166136261u ^ unsigned(r.presence);
    for (const char* c = r.status; *c; ++c) { h ^= static_cast<unsigned char>(*c); h *= 16777619u; }
    return h ? h : 1u;
}

const FriendRow* FindRow(int32_t pid) {
    for (size_t i = 0; i < g.snap.rows.size(); ++i) if (g.snap.rows[i].pid == pid) return &g.snap.rows[i];
    return nullptr;
}

int ListPid(const DWCFriendData* d) {
    if (!d) return 0;
    const u32 type = TypeOf(d);
    if (type == DWC_ACC_FRIENDDATA_GS_PROFILE_ID) return d->gs_profile_id.id;
    if (type == DWC_ACC_FRIENDDATA_FRIEND_KEY) return DWC_Acc_FriendKeyToGsProfileId(KeyOf(d->friend_key), NpDwcGameCode());
    return 0;
}

uint8_t StatusCode(const FriendRow& r) {
    switch (r.presence) {
    case Presence::Menu: return DWC_STATUS_ONLINE;
    case Presence::Queue: return DWC_STATUS_MATCH_ANYBODY;
    case Presence::Room: return DWC_STATUS_MATCH_FRIEND;
    case Presence::Match: return DWC_STATUS_PLAYING;
    default: return DWC_STATUS_OFFLINE;
    }
}

bool IsPaused() { return g.paused || (NpRankedBusy && NpRankedBusy()); }

// The game's list against the snapshot: convert keys to profile ids, mark buddies, report changed status. Callbacks run from here (game thread).
void ApplyList(bool final) {
    if (!g.list || g.listLen <= 0 || !g.haveSnap) return;
    if (int(g.lastSent.size()) < g.listLen) g.lastSent.resize(g.listLen, 0);
    for (int i = 0; i < g.listLen && i < 64; ++i) {
        DWCFriendData* e = &g.list[i];
        const int pid = ListPid(e);
        if (pid <= 0) continue;
        const FriendRow* row = FindRow(pid);
        if (!row || row->state == FriendState::Incoming) continue;
        const bool wasBuddy = DWC_IsBuddyFriendData(e) != 0;
        if (DWC_GetFriendDataType(e) != DWC_ACC_FRIENDDATA_GS_PROFILE_ID) DWC_SetGsProfileId(e, pid);   // the service resolved the key
        if (row->state == FriendState::Friend && !wasBuddy) {
            DWCi_SetBuddyFriendData(e);
            if (g.buddyCb) g.buddyCb(i, g.buddyParam);
        } else if (row->state != FriendState::Friend && wasBuddy) {
            DWC_ClearBuddyFlagFriendData(e);
        }
        if (row->state == FriendState::Friend && g.statusCb) {
            const unsigned h = Hash(*row);
            if (g.lastSent[i] != h) { g.lastSent[i] = h; g.statusCb(i, StatusCode(*row), row->status, g.statusParam); }
        } else if (row->state != FriendState::Friend) {
            g.lastSent[i] = 0;
        }
    }
    (void)final;
}

// Entries of the game's list the service does not know yet are sent as adds.
void QueuePushes() {
    if (!g.list || !g.haveSnap) return;
    for (int i = 0; i < g.listLen && i < 64; ++i) {
        const int pid = ListPid(&g.list[i]);
        if (pid <= 0 || FindRow(pid)) continue;
        bool done = false;
        for (size_t k = 0; k < g.pushed.size(); ++k) done = done || g.pushed[k] == pid;
        if (done) continue;
        g.pushed.push_back(pid);
        FriendOpRequest op;
        op.op = FriendOp::Add;
        op.pid = pid;
        if (g.ops.size() < kOpQueueMax) g.ops.push_back(op);
    }
}

void OnFailure(const Result& r) {
    ++g.failStreak;
    uint64_t gap = kFailGapMs * (g.failStreak > 4 ? 4 : g.failStreak);
    if (r.fail == Fail::RateLimited || r.fail == Fail::Disabled) gap = NpNet::PollWaitAfterBusyMs(r.retryAfterSec);
    if (r.fail == Fail::Auth || r.fail == Fail::Upgrade) gap = 5 * 60 * 1000;
    g.nextAt = NowMs() + gap;
}

void StartNext() {
    Api* api = GetApi();
    if (!api) return;
    const uint64_t now = NowMs();
    if (g.statusDirty && g.ops.size() < kOpQueueMax) {   // only the newest status is sent
        FriendOpRequest op;
        op.op = FriendOp::SetStatus;
        snprintf(op.status, sizeof op.status, "%s", g.status);
        g.statusDirty = false;
        g.ops.push_front(op);
    }
    if (!g.ops.empty()) {
        g.curOp = g.ops.front();
        g.ops.pop_front();
        const uint32_t id = api->BeginFriendOp(g.curOp);
        if (!id) {
            g.last.done = true; g.last.ok = false; g.last.op = g.curOp.op; g.last.fail = Fail::Disabled; g.last.code[0] = 0; ++g.last.serial;
            return;
        }
        g.req = Req::Op;
        g.reqId = id;
        return;
    }
    if (g.playedWanted) {
        g.playedWanted = false;
        const uint32_t id = api->BeginPlayedWith();
        if (id) { g.req = Req::Played; g.reqId = id; }
        return;
    }
    if (!g.running || now < g.nextAt) return;
    if (PolicyConfig().Any()) {   // O4: the idle stop and the request limit (off by default)
        const unsigned gate = g_gate.Step(PolicyConfig(), now, NpVitaInputActivity ? NpVitaInputActivity() : 0);
        if (gate & NpFriendsPolicy::kStopped) FRIEND_LOG("refresh paused: no button for %u s or %u requests since one (a button press starts it again)\n", unsigned(PolicyConfig().idleStopMs / 1000), unsigned(g_gate.reqsSinceInput));
        if (gate & NpFriendsPolicy::kResumed) FRIEND_LOG("refresh resumed by a button press\n");
        if (gate & NpFriendsPolicy::kHold) return;
        g_gate.NoteRequest();
    }
    FriendsRequest fr;
    if (!g.forceFull && g.rev[0]) snprintf(fr.rev, sizeof fr.rev, "%s", g.rev);
    uint32_t wait = kHoldSec * g.scalePct / 100;
    if (wait > 55) wait = 55;
    fr.waitSec = g.forceFull || !g.haveSnap || !g.rev[0] ? 0 : wait;   // the first look and the one after an operation answer at once
    const uint32_t id = api->BeginFriends(fr);
    if (!id) { g.nextAt = now + kFailGapMs; return; }
    g.req = Req::Friends;
    g.reqId = id;
}

void Poll() {
    Api* api = GetApi();
    if (!api) return;
    Result r;
    if (g.req == Req::Friends) {
        FriendsReply reply;
        r = api->PollFriends(g.reqId, &reply);
        if (r.status == Status::Pending) return;
        g.req = Req::None;
        if (r.ok()) {
            g.failStreak = 0;
            g.scalePct = reply.pollScalePct;
            g.forceFull = false;
            if (reply.changed) {
                g.snap = reply;
                g.haveSnap = true;
                snprintf(g.rev, sizeof g.rev, "%s", reply.rev);
                ++g.revision;
                QueuePushes();
                ApplyList(false);
            } else if (!g.haveSnap) {
                g.forceFull = true;
            }
            uint64_t gap = reply.heldMs > 0 ? kHeldGapMs : kPlainGapMs;
            if (reply.heldMs > 0 && reply.heldMs < 1000 && gap < 1000 - reply.heldMs) gap = 1000 - reply.heldMs;   // never faster than once a second
            if (reply.heldMs > 0) gap = gap * g.scalePct / 100;
            if (PolicyConfig().minGapMs) gap = NpFriendsPolicy::Gate::Gap(PolicyConfig(), gap);   // O4: MSC_FRIENDS_MIN_GAP_S
            g.nextAt = NowMs() + gap;
            if (g.syncing && g.ops.empty()) {   // the list is in step: the sync call is over
                g.syncing = false;
                ApplyList(true);
                if (g.updateCb) { DWCUpdateServersCallback cb = g.updateCb; g.updateCb = nullptr; cb(0, TRUE, g.updateParam); }
            }
        } else {
            OnFailure(r);
            if (g.syncing && (r.fail == Fail::Auth || r.fail == Fail::Upgrade || g.failStreak >= 3)) {
                g.syncing = false;
                if (g.updateCb) { DWCUpdateServersCallback cb = g.updateCb; g.updateCb = nullptr; cb(DWC_ERROR_NETWORK, FALSE, g.updateParam); }
            }
        }
        return;
    }
    if (g.req == Req::Played) {
        PlayedReply reply;
        r = api->PollPlayedWith(g.reqId, &reply);
        if (r.status == Status::Pending) return;
        g.req = Req::None;
        if (r.ok()) { g.played = reply.rows; g.playedReady = true; ++g.revision; }
        else OnFailure(r);
        return;
    }
    if (g.req == Req::Op) {
        FriendOpReply reply;
        r = api->PollFriendOp(g.reqId, &reply);
        if (r.status == Status::Pending) return;
        g.req = Req::None;
        g.last.done = true;
        g.last.ok = r.ok();
        g.last.op = g.curOp.op;
        g.last.fail = r.ok() ? Fail::None : r.fail;
        snprintf(g.last.code, sizeof g.last.code, "%s", r.ok() ? "" : r.code);
        g.last.state = reply.state;
        g.last.pid = reply.pid;
        snprintf(g.last.name, sizeof g.last.name, "%s", reply.name);
        ++g.last.serial;
        if (!r.ok() && (r.fail == Fail::RateLimited || r.fail == Fail::Disabled)) OnFailure(r);
        if (g.curOp.op != FriendOp::SetStatus || !r.ok()) { g.forceFull = true; if (g.nextAt > NowMs()) g.nextAt = NowMs(); }
        if (!r.ok() && g.syncing && g.curOp.op == FriendOp::Add) { /* an entry the service refuses (unknown player): the rest of the list goes on */ }
        if (g.curOp.op == FriendOp::SetStatus && !r.ok()) { snprintf(g.status, sizeof g.status, "%s", g.curOp.status); g.statusDirty = false; }
        return;
    }
}

void PumpHook() { NpFriends::Pump(); }

void Ensure() {
    if (!g_pumpSet) { NpDwcAddPump(PumpHook); g_pumpSet = true; }
}

bool PidOk(int32_t pid) { return pid > 0; }

bool Queue(const FriendOpRequest& op) {
    if (g.ops.size() >= kOpQueueMax) return false;
    g.ops.push_back(op);
    Ensure();
    return true;
}

}  // namespace

namespace NpFriends {

void Start() { g.running = true; if (g.nextAt > NowMs() + 2000) g.nextAt = NowMs(); Ensure(); }
void Stop() { g.running = false; if (g.req == Req::Friends) { GetApi()->Cancel(g.reqId); g.req = Req::None; } }
bool Running() { return g.running; }
// A hand refresh (Galaxy: START on the Friends list): the held request in flight is dropped and the next one asks for the whole list at once. Costs one request. Nothing in the
// native tree calls it yet: the game's own friend screens are retail code with no START hook, and the platform's extra screens are not part of this tree.
void RefreshNow() {
    if (!g.running) return;
    if (g.req == Req::Friends) { GetApi()->Cancel(g.reqId); g.req = Req::None; }
    g.forceFull = true;
    g.nextAt = NowMs();
    ++g_manualRefreshes;
    Ensure();
}
uint32_t ManualRefreshes() { return g_manualRefreshes; }
void SetPaused(bool paused) { g.paused = paused; }
void Reset() { if (g.req != Req::None) { Api* api = GetApi(); if (api) api->Cancel(g.reqId); } g.Clear(); }

void Pump() {
    if (g.req == Req::None && !g.running && g.ops.empty() && !g.playedWanted && !g.statusDirty) return;
    if (IsPaused()) {
        if (g.req == Req::Friends) { GetApi()->Cancel(g.reqId); g.req = Req::None; g.nextAt = NowMs() + 1000; }   // a held poll does not run into a match
        return;
    }
    if (g.req != Req::None) Poll();
    if (g.req == Req::None) StartNext();
}

const FriendsReply& Snapshot() { return g.snap; }
uint32_t Revision() { return g.revision; }
bool HaveSnapshot() { return g.haveSnap; }
const FriendRow* Find(int32_t pid) { return FindRow(pid); }
bool HasInvite(int32_t* from, char* room, size_t size) {
    for (size_t i = 0; i < g.snap.rows.size(); ++i) {
        const FriendRow& r = g.snap.rows[i];
        if (r.state == FriendState::Friend && r.inviteRoom[0]) { if (from) *from = r.pid; if (room && size) snprintf(room, size, "%s", r.inviteRoom); return true; }
    }
    return false;
}

bool Add(const char* who) {
    if (!who) return false;
    std::string digits, text;
    for (const char* c = who; *c; ++c) {
        if (*c == ' ' || *c == '-') continue;
        text += *c;
        if (*c >= '0' && *c <= '9') digits += *c;
    }
    FriendOpRequest op;
    op.op = FriendOp::Add;
    if (digits.size() == 12 && digits.size() == text.size()) {   // the game's friend key
        const u64 key = DWC_Acc_NumericKeyToFriendKey(digits.c_str());
        const int pid = DWC_Acc_FriendKeyToGsProfileId(key, NpDwcGameCode());
        if (pid <= 0) { g.last = OpResult(); g.last.done = true; g.last.fail = Fail::Rejected; snprintf(g.last.code, sizeof g.last.code, "bad_code"); ++g.last.serial; return false; }
        op.pid = pid;
    } else {
        if (text.size() != 8) { g.last = OpResult(); g.last.done = true; g.last.fail = Fail::Rejected; snprintf(g.last.code, sizeof g.last.code, "bad_code"); ++g.last.serial; return false; }
        snprintf(op.code, sizeof op.code, "%.4s-%.4s", text.c_str(), text.c_str() + 4);
        for (char* c = op.code; *c; ++c) if (*c >= 'a' && *c <= 'z') *c = char(*c - 32);
    }
    return Queue(op);
}
bool AddPid(int32_t pid) { if (!PidOk(pid)) return false; FriendOpRequest o; o.op = FriendOp::Add; o.pid = pid; return Queue(o); }
bool Decline(int32_t pid) { if (!PidOk(pid)) return false; FriendOpRequest o; o.op = FriendOp::Decline; o.pid = pid; return Queue(o); }
bool Remove(int32_t pid) { if (!PidOk(pid)) return false; FriendOpRequest o; o.op = FriendOp::Remove; o.pid = pid; return Queue(o); }
bool Block(int32_t pid) { if (!PidOk(pid)) return false; FriendOpRequest o; o.op = FriendOp::Block; o.pid = pid; return Queue(o); }
bool Unblock(int32_t pid) { if (!PidOk(pid)) return false; FriendOpRequest o; o.op = FriendOp::Unblock; o.pid = pid; return Queue(o); }
bool Invite(int32_t pid, const char* roomCode) {
    if (!PidOk(pid) || !roomCode || !*roomCode) return false;
    FriendOpRequest o; o.op = FriendOp::Invite; o.pid = pid; snprintf(o.roomCode, sizeof o.roomCode, "%s", roomCode); return Queue(o);
}
bool ClearInvite(int32_t pid) { if (!PidOk(pid)) return false; FriendOpRequest o; o.op = FriendOp::ClearInvite; o.pid = pid; return Queue(o); }
bool SetStatus(const char* statusString) {
    if (!statusString || strlen(statusString) >= kStatusChars) return false;
    snprintf(g.status, sizeof g.status, "%s", statusString);
    g.statusDirty = true;
    Ensure();
    return true;
}

const OpResult& LastOp() { return g.last; }
bool Busy() { return g.req == Req::Op || !g.ops.empty(); }
void RefreshPlayedWith() { g.playedWanted = true; Ensure(); }
bool PlayedWithReady() { return g.playedReady; }
const std::vector<PlayedRow>& PlayedWith() { return g.played; }

bool OwnKeyText(char* out, size_t size) {
    const int32_t pid = NpDwcLoggedInProfile();
    if (pid <= 0 || !out || size < DWC_ACC_FRIENDKEY_STRING_BUFSIZE) return false;
    DWC_Acc_FriendKeyToNumericKey(DWC_Acc_CreateFriendKey(pid, NpDwcGameCode()), out);
    return true;
}

}  // namespace NpFriends

// ---- the game's friend calls ----------------------------------------------------------------------------------------------------------------------
extern "C" void NpDwcFriendsRegisterList(DWCFriendData* list, int len) {
    g.list = list;
    g.listLen = len;
    g.lastSent.assign(len > 0 ? len : 0, 0);
}

extern "C" BOOL DWC_SetBuddyFriendCallback(DWCBuddyFriendCallback callback, void* param) { g.buddyCb = callback; g.buddyParam = param; return TRUE; }
extern "C" BOOL DWC_SetFriendStatusCallback(DWCFriendStatusCallback callback, void* param) { g.statusCb = callback; g.statusParam = param; return TRUE; }

extern "C" BOOL DWC_UpdateServersAsync(const char*, DWCUpdateServersCallback updateCallback, void* updateParam, DWCFriendStatusCallback statusCallback,
                                       void* statusParam, DWCDeleteFriendListCallback deleteCallback, void* deleteParam) {
    if (NpDwcLoggedInProfile() <= 0) return FALSE;
    g.updateCb = updateCallback; g.updateParam = updateParam;
    if (statusCallback) { g.statusCb = statusCallback; g.statusParam = statusParam; }
    g.deleteCb = deleteCallback; g.deleteParam = deleteParam;
    g.syncing = true;
    g.pushed.clear();
    g.lastSent.assign(g.listLen > 0 ? g.listLen : 0, 0);   // the status callback is given every friend's current state again
    g.forceFull = true;
    g.nextAt = 0;
    NpFriends::Start();
    return TRUE;
}

extern "C" BOOL DWC_CanChangeFriendList(void) { return g.syncing ? FALSE : TRUE; }

extern "C" void DWC_DeleteBuddyFriendData(DWCFriendData* d) {
    const int pid = ListPid(d);
    if (pid > 0 && FindRow(pid)) NpFriends::Remove(pid);   // the service forgets the pair, as GP's gpDeleteBuddy
    if (d) memset(d, 0, 12);
}

extern "C" u8 DWC_GetFriendStatus(const DWCFriendData* d, char* statusString) {
    if (statusString) statusString[0] = 0;
    const int pid = ListPid(d);
    const FriendRow* row = pid > 0 ? FindRow(pid) : nullptr;
    if (!row || row->state != FriendState::Friend) return DWC_STATUS_OFFLINE;
    if (statusString) snprintf(statusString, 256, "%s", row->status);
    return StatusCode(*row);
}
extern "C" u8 DWC_GetFriendStatusSC(const DWCFriendData* d, u8* maxEntry, u8* numEntry, char* statusString) {
    if (maxEntry) *maxEntry = 0;
    if (numEntry) *numEntry = 0;
    return DWC_GetFriendStatus(d, statusString);
}
extern "C" u8 DWC_GetFriendStatusData(const DWCFriendData* d, char* statusData, int* size) {
    char text[256];
    const u8 st = DWC_GetFriendStatus(d, text);
    if (st == DWC_STATUS_OFFLINE) { if (size) *size = -1; return st; }
    const int n = DWC_Base64Decode(text, u32(strlen(text)), nullptr, 0);
    if (size) *size = n;
    if (statusData && n > 0) DWC_Base64Decode(text, u32(strlen(text)), statusData, u32(n));
    return st;
}
extern "C" int DWC_GetNumFriend(const DWCFriendData list[], int len) {
    int n = 0;
    for (int i = 0; list && i < len; ++i) if (DWC_IsValidFriendData(&list[i])) ++n;
    return n;
}

extern "C" BOOL DWC_SetOwnStatusData(const char* statusData, u32 size) {
    if (NpDwcLoggedInProfile() <= 0) return FALSE;
    char encoded[kStatusChars];
    const int n = DWC_Base64Encode(statusData, size, encoded, sizeof encoded - 1);
    if (n < 0) return FALSE;
    encoded[n] = 0;
    return NpFriends::SetStatus(encoded) ? TRUE : FALSE;
}
extern "C" BOOL DWC_SetOwnStatusString(const char* statusString) { return NpDwcLoggedInProfile() > 0 && NpFriends::SetStatus(statusString ? statusString : "") ? TRUE : FALSE; }
extern "C" BOOL DWC_GetOwnStatusString(char* statusString) { if (!statusString) return FALSE; snprintf(statusString, 256, "%s", g.status); return TRUE; }
extern "C" int DWC_GetOwnStatusData(char* statusData) {
    const int n = DWC_Base64Decode(g.status, u32(strlen(g.status)), nullptr, 0);
    if (!statusData || n < 0) return n;
    return DWC_Base64Decode(g.status, u32(strlen(g.status)), statusData, u32(n));
}

extern "C" void NpDwcFriendsShutdown(void) {   // DWC_ShutdownFriendsMatch: the list pointer is no longer valid
    NpFriends::Stop();
    g.list = nullptr;
    g.listLen = 0;
    g.syncing = false;
    g.buddyCb = nullptr; g.statusCb = nullptr; g.updateCb = nullptr; g.deleteCb = nullptr;
}

// Tests
void NpDwcFriendsResetForTest() { NpFriends::Reset(); }
