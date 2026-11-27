// SEND A REPORT / DELETE MY ONLINE ACCOUNT: see np_mpreport.h.
#include "platform/sdk/net/np_mpreport.h"

#include <stdio.h>
#include <string.h>

#include "platform/sdk/net/online/np_json.h"

namespace NpMpReport {

namespace {

Env g_env;

// ---- text helpers -----------------------------------------------------------------------------------------------------------------------------
std::string Scrubbed(const char* text) {
    if (!text || !*text) return std::string();
    const size_t n = strlen(text);
    std::string out(n * 2 + 64, '\0');   // a replacement is never longer than twice what it replaces plus its own marker; Scrub never writes past cap - 1
    const size_t w = NpMpDiag::Scrub(text, n, &out[0], out.size());
    out.resize(w);
    return out;
}

// the last `max` characters, starting at a line start (so a cut never leaves half a line)
std::string TailLines(const std::string& s, size_t max) {
    if (s.size() <= max) return s;
    size_t from = s.size() - max;
    const size_t nl = s.find('\n', from);
    return nl == std::string::npos ? std::string() : s.substr(nl + 1);
}

// drops the oldest line; false when there is nothing left to drop
bool DropFirstLine(std::string* s) {
    const size_t nl = s->find('\n');
    if (nl == std::string::npos) { const bool had = !s->empty(); s->clear(); return had; }
    s->erase(0, nl + 1);
    return true;
}

// JSON string content: ASCII only (anything else becomes '?'), control characters other than a new line become '?'
std::string Esc(const std::string& s) {
    std::string o;
    o.reserve(s.size() + s.size() / 8 + 8);
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == '"') o += "\\\"";
        else if (c == '\\') o += "\\\\";
        else if (c == '\n') o += "\\n";
        else if (c < 0x20 || c >= 0x7F) o += '?';
        else o += char(c);
    }
    return o;
}

std::string NameOnly(const char* name) {   // letters, digits, space . _ - only: what the service allows in a name (the screen has already cut it to 10)
    std::string o;
    for (size_t i = 0; name && name[i] && o.size() < 16; ++i) {
        const unsigned char c = static_cast<unsigned char>(name[i]);
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == ' ' || c == '.' || c == '_' || c == '-') o += char(c);
    }
    return o;
}

std::string SessionJson(const NpMpDiag::SessionNumbers& s) {
    if (!s.valid) return std::string();
    char h[120];
    if (!NpMpDiag::SessionHeader(s, h, sizeof h)) return std::string();
    // "d=312;dl=2;rs=0;dr=1;sv=0;e=1" -> {"d":312,"dl":2,...}: the same keys and ranges as the header of the leave request
    std::string j = "{";
    const char* p = h;
    while (*p) {
        const char* eq = strchr(p, '=');
        const char* end = strchr(p, ';');
        if (!eq) break;
        if (j.size() > 1) j += ',';
        j += '"';
        j.append(p, size_t(eq - p));
        j += "\":";
        j.append(eq + 1, size_t((end ? end : p + strlen(p)) - eq - 1));
        if (!end) break;
        p = end + 1;
    }
    return j + "}";
}

}  // namespace

// ---- the payload ------------------------------------------------------------------------------------------------------------------------------
size_t BuildPayload(const Inputs& in, char* out, size_t cap) {
    if (!out || cap == 0) return 0;
    out[0] = 0;
    std::string log = TailLines(Scrubbed(in.log), kMaxLogChars);
    std::string events = TailLines(Scrubbed(in.events), kMaxEventChars);
    std::string conn = Scrubbed(in.conn);
    if (conn.size() > kMaxConnChars) conn.resize(kMaxConnChars);
    const std::string session = SessionJson(in.session);
    char ver[32], build[72];
    snprintf(ver, sizeof ver, "%.24s", in.version ? in.version : "");
    snprintf(build, sizeof build, "%.64s", in.build ? in.build : "");
    // the frame without the three long texts
    std::string head = "{\"v\":1,\"app\":\"" + Esc(ver) + "\",\"build\":\"" + Esc(build) + "\",\"name\":\"" + Esc(NameOnly(in.name)) + "\"";
    std::string tail = session.empty() ? std::string("}") : ",\"session\":" + session + "}";
    size_t limit = cap - 1 < kMaxBytes ? cap - 1 : kMaxBytes;
#ifdef NP_MPREPORT_MUT_NOCAP   // mutant of tests/test_mpreport.py: the size limit is not applied
    limit = cap - 1;
#endif
    std::string body;
    for (;;) {
        body = head + ",\"log\":\"" + Esc(log) + "\",\"events\":\"" + Esc(events) + "\",\"conn\":\"" + Esc(conn) + "\"" + tail;
#ifdef NP_MPREPORT_MUT_NOCAP
        break;
#endif
        if (body.size() <= limit) break;
        if (!log.empty()) { DropFirstLine(&log); continue; }     // the oldest log lines go first,
        if (!events.empty()) { DropFirstLine(&events); continue; }   // then the oldest events
        if (!conn.empty()) { conn.clear(); continue; }
        return 0;                                                  // the frame alone does not fit
    }
    if (body.size() > cap - 1) return 0;
    memcpy(out, body.data(), body.size());
    out[body.size()] = 0;
    return body.size();
}

size_t GatherPayload(char* out, size_t cap) {
    const Env& e = g_env;
    Account a;
    if (e.account) e.account(&a);
    char* log = new char[NpMpDiag::kLogTailBytes + 64];
    char* events = new char[kMaxEventChars + 64];
    log[0] = events[0] = 0;
    NpMpDiag::LogTail(log, size_t(NpMpDiag::kLogTailBytes) + 64);
    NpMpDiag::BuildCurrentReport(events, kMaxEventChars + 1);
    char build[16];
    snprintf(build, sizeof build, "%08x", e.fingerprint ? e.fingerprint() : 0u);
    Inputs in;
    in.version = e.appVersion ? e.appVersion() : "";
    in.build = build;
    in.name = a.name;
    in.log = log;
    in.events = events;
    in.conn = NpMpDiag::LastConnTest();
    in.session = NpMpDiag::LastSession();
    const size_t n = BuildPayload(in, out, cap);
    delete[] log;
    delete[] events;
    return n;
}

// ---- the texts ----------------------------------------------------------------------------------------------------------------------------------
const char* ConsentText() {
    return "Send a problem report? It contains: your online name, the last lines of the game's multiplayer log, your last connection check and numbers from your last online game "
           "(length, delay, reconnects, packet loss). IP addresses, ids and codes are removed first. Your friend list is not sent. "
           "It is kept for 30 days or until you delete the account.";
}
const char* OfferText() {
    return "Your last online game ended with a problem. Send a report to help fix it? You will see what is sent first.";
}
const char* SendingText(int kind) {
    return kind == Op::kDelete ? "Deleting your online account... Press Circle to cancel." : "Sending the report... Press Circle to cancel.";
}

// ---- the environment ------------------------------------------------------------------------------------------------------------------------------
void SetEnv(const Env& env) { g_env = env; }
const Env& GetEnv() { return g_env; }

// ---- the operation ------------------------------------------------------------------------------------------------------------------------------
bool Op::Ask(Kind kind) {
    if (phase_ == kSending) return false;
    kind_ = kind;
    outcome_ = kPending;
    handle_ = 0;
    baseIdx_ = 0;
    triedMe_ = false;
    body_.clear();
    acct_ = Account();
    if (g_env.account) g_env.account(&acct_);
    if (kind == kReport && !acct_.Ready()) {
        phase_ = kDone;
        outcome_ = kNoAccount;
        return false;
    }
    phase_ = kAsked;
    return true;
}

void Op::Consent() {
    if (phase_ == kAsked) phase_ = kConsented;
}

void Op::Cancel() {
    if (phase_ == kSending && handle_ && g_env.cancel) g_env.cancel(handle_);
    handle_ = 0;
    if (phase_ == kAsked || phase_ == kConsented || phase_ == kSending) { phase_ = kIdle; outcome_ = kPending; }
}

bool Op::Start() {
#ifdef NP_MPREPORT_MUT_NOCONSENT   // mutant of tests/test_mpreport.py: the consent step can be skipped
    if (phase_ != kConsented && phase_ != kAsked) return false;
#else
    if (phase_ != kConsented) return false;
#endif
    startedAt_ = g_env.nowMs ? g_env.nowMs() : 0;
    if (kind_ == kDelete && !acct_.Ready()) {   // nothing on the service to delete: only this Vita's leftovers (a name) go
        if (g_env.forget) g_env.forget();
        phase_ = kDone;
        outcome_ = kNoAccount;
        return true;
    }
    if (kind_ == kReport) {
        char* buf = new char[kMaxBytes + 16];
        const size_t n = GatherPayload(buf, kMaxBytes + 16);
        body_.assign(buf, n);
        delete[] buf;
        if (!n) { phase_ = kDone; outcome_ = kRefused; return true; }
        method_ = "POST";
        path_ = "/v1/report";
    } else {
        body_ = "{}";
        method_ = "POST";   // the game's HTTPS client speaks GET and POST only: the service takes POST /v1/account like DELETE /v1/account
        path_ = "/v1/account";
    }
    phase_ = kSending;
    if (!BeginRequest()) {
        phase_ = kDone;
        outcome_ = kind_ == kDelete ? kKeptNoConnection : kNoConnection;
    }
    return true;
}

bool Op::BeginRequest() {
    // the address list: "https://a,https://b"; baseIdx_ picks the next one when the one before did not answer
    std::string urls = acct_.urls;
    size_t at = 0;
    for (int i = 0; i < baseIdx_; ++i) {
        const size_t c = urls.find(',', at);
        if (c == std::string::npos) return false;
        at = c + 1;
    }
    size_t end = urls.find(',', at);
    if (end == std::string::npos) end = urls.size();
    const std::string base = urls.substr(at, end - at);
    if (base.empty() || !g_env.start) return false;
    std::vector<std::string> h;
    h.push_back("Content-Type: application/json");
    h.push_back(std::string("Authorization: Bearer ") + acct_.token);
    h.push_back("X-Msc-Proto: 1");
    h.push_back(std::string("X-Msc-App: ") + (g_env.appVersion ? g_env.appVersion() : ""));
    char b[40];
    snprintf(b, sizeof b, "X-Msc-Build: %08x", g_env.fingerprint ? g_env.fingerprint() : 0u);
    h.push_back(b);
    std::string err;
    handle_ = g_env.start(method_.c_str(), base + path_, body_, h, 25000, &err);
    transportText_ = err;
    return handle_ > 0;
}

Op::Phase Op::Step() {
    if (phase_ != kSending) return phase_;
    Answer a;
    const int rc = g_env.poll ? g_env.poll(handle_, &a) : -1;
    if (rc == 0) {
        const uint64_t now = g_env.nowMs ? g_env.nowMs() : 0;
        if (now > startedAt_ + 30000) {
            if (g_env.cancel) g_env.cancel(handle_);
            a = Answer();
            a.error = 1;
            a.text = "The online service did not answer in time.";
            Finish(a);
        }
        return phase_;
    }
    if (rc < 0) {
        a = Answer();
        a.error = 1;
        a.text = "The online service request was lost.";
    }
    handle_ = 0;
    Finish(a);
    return phase_;
}

void Op::Finish(const Answer& a) {
    handle_ = 0;
    const bool del = kind_ == kDelete;
    if (a.error != 0) {
        transportText_ = a.text;
        ++baseIdx_;
        if (BeginRequest()) return;   // the next address, same request
        phase_ = kDone;
        outcome_ = del ? kKeptNoConnection : kNoConnection;
        return;
    }
    std::string code, reason;
    {
        NpNet::Json j;
        if (!a.body.empty() && j.Parse(a.body.data(), a.body.size())) {
            const int e = j.Get(j.Root(), "error");
            code = j.GetStr(e, "code");
            reason = j.GetStr(e, "reason");
        }
    }
    phase_ = kDone;
    const int s = a.status;
    if (del) {
        if (s == 200 || s == 401) {   // 401: the token is already gone, which is what the player wanted
#ifdef NP_MPREPORT_MUT_401_FAIL   // mutant of tests/test_mpreport.py: "already gone" is not a success
            if (s == 401) { outcome_ = kRefused; return; }
#endif
            if (g_env.forget) g_env.forget();
            outcome_ = kDeleted;
        } else if (s == 404 || s == 405) outcome_ = kNotYet;
        else if (s == 429) outcome_ = kLimitReached;
        else if (s == 503 || s >= 500) outcome_ = kBusy;
        else outcome_ = kRefused;
#ifdef NP_MPREPORT_MUT_FORGET_ALWAYS   // mutant: the local identity is forgotten even when the service did not delete the account
        if (outcome_ != kDeleted && g_env.forget) g_env.forget();
#endif
        return;
    }
    if (s == 200) outcome_ = kSent;
    else if (s == 413) outcome_ = kTooBig;
    else if (s == 429) {
#ifdef NP_MPREPORT_MUT_LIMIT_AS_FAIL   // mutant: the daily limit is not told as such
        outcome_ = kRefused;
#else
        outcome_ = kLimitReached;
#endif
    } else if (s == 503 || s >= 500) outcome_ = kBusy;
    else if (s == 404) outcome_ = kNotYet;
    else outcome_ = kRefused;
    (void)code;
    (void)reason;
}

std::string Op::Sentence() const {
    // every text is a whole row of np_text_strings.tsv (the menu's popup and the settings line both translate it); none is longer than 60 characters in English
    if (kind_ == kDelete) {
        switch (outcome_) {
        case kDeleted: return "Your online account was deleted. This Vita forgot its name.";
        case kNoAccount: return "There is no online account on this Vita. Nothing to delete.";
        case kKeptNoConnection: return "No connection. Nothing was changed. Check your Wi-Fi.";
        case kNotYet: return "The online service cannot delete accounts yet. Try later.";
        case kLimitReached: return "Too many tries. Wait a while and try again.";
        case kBusy: return "The online service is busy. Nothing was changed. Try later.";
        case kPending: return "";
        default: return "The online service refused the request. Nothing was changed.";
        }
    }
    switch (outcome_) {
    case kSent: return "Report sent. Thank you.";
    case kLimitReached: return "You already sent 3 reports today. Try again tomorrow.";
    case kNoConnection: return "No connection to the online service. Check your Wi-Fi.";
    case kBusy: return "The online service is not taking reports now. Try tomorrow.";
    case kNoAccount: return "No online account on this Vita yet. Play online once first.";
    case kTooBig: return "The report was too big to send.";
    case kNotYet: return "The online service does not take reports yet.";
    case kPending: return "";
    default: return "The online service refused the report.";
    }
}

}  // namespace NpMpReport
