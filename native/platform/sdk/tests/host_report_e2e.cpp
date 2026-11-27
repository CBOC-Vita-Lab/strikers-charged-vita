// Host end-to-end check of SEND A REPORT and DELETE MY ONLINE ACCOUNT against a LOCAL fake service (server/fake/server.mjs: the Worker's own application code over
// in-memory SQLite, plain HTTP on 127.0.0.1). The real HTTP client of the game (np_http / np_http_plain), the real np_mpreport Op and the real production Env are
// used; only the account lookup is replaced (the menu's account files live in np_menu_flow.cpp), reading the token and the service address from the environment.
//   host_report_e2e send    -> one POST /v1/report, exit 0 when the answer is "report sent"
//   host_report_e2e send4   -> four reports in a row: the first three are sent, the fourth is "limit reached"
//   host_report_e2e delete  -> one POST /v1/account, exit 0 when the account was deleted and this "Vita" forgot it
//   host_report_e2e delete2 -> deleting an account that is already gone: still success (401 counts)
// Environment: E2E_URL (http://127.0.0.1:port), E2E_TOKEN, E2E_NAME, E2E_FORGET_FILE (removed by "forget").
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <string>

#include "platform/sdk/net/np_https.h"
#include "platform/sdk/net/np_mpdiag.h"
#include "platform/sdk/net/np_mpreport.h"

// The service here is plain HTTP on loopback: the TLS client is never reached, these only satisfy the linker.
NpHttpsHandle np_https_start(const NpHttpsRequest*, NpHttpsError*) { return 0; }
int np_https_poll(NpHttpsHandle, NpHttpsResult*) { return -1; }
void np_https_release(NpHttpsHandle) {}
const char* np_https_error_name(NpHttpsError) { return "none"; }
const char* np_https_error_text(NpHttpsError) { return "none"; }
const char* np_https_stage_name(NpHttpsStage) { return "none"; }

extern "C" int NpMenuReportAccount(char* name, size_t nameSize, char* token, size_t tokenSize, char* urls, size_t urlsSize) {
    const char* u = getenv("E2E_URL");
    const char* t = getenv("E2E_TOKEN");
    const char* n = getenv("E2E_NAME");
    if (!u || !t || !*t) return 0;
    snprintf(name, nameSize, "%s", n ? n : "");
    snprintf(token, tokenSize, "%s", t);
    snprintf(urls, urlsSize, "%s", u);
    return 1;
}
extern "C" void NpMenuForgetAccount(void) {
    const char* f = getenv("E2E_FORGET_FILE");
    if (f) unlink(f);
}

static void LogLine(const char* format, ...) {
    va_list a;
    va_start(a, format);
    NpMpDiag::NoteLogLine(format, a);
    va_end(a);
}

static int RunOnce(NpMpReport::Op::Kind kind, std::string* sentence, NpMpReport::Op::Outcome* outcome) {
    NpMpReport::Op op;
    if (!op.Ask(kind)) { *sentence = op.Sentence(); *outcome = op.GetOutcome(); return 0; }
    op.Consent();
    if (!op.Start()) return -1;
    for (int i = 0; i < 6000 && op.GetPhase() == NpMpReport::Op::kSending; ++i) {
        op.Step();
        usleep(5000);
    }
    *sentence = op.Sentence();
    *outcome = op.GetOutcome();
    return op.GetPhase() == NpMpReport::Op::kDone ? 1 : -1;
}

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    NpMpReport::UseNetwork();
    // a session with a problem, so there are numbers and a log tail to send (the address below must not survive the scrub)
    LogLine("[lan] hello from the host check, peer 192.168.77.5:3658");
    NpMpDiag::Note(NpMpDiag::kSessionStart, 1, 2);
    NpMpDiag::Note(NpMpDiag::kInputDelay, 3, 60, 1);
    NpMpDiag::Note(NpMpDiag::kDesync, 120, 2);
    NpMpDiag::Note(NpMpDiag::kSessionEnd);
    const std::string mode = argv[1];
    std::string s;
    NpMpReport::Op::Outcome o = NpMpReport::Op::kPending;
    if (mode == "send" || mode == "send4") {
        const int n = mode == "send" ? 1 : 4;
        for (int i = 0; i < n; ++i) {
            const int rc = RunOnce(NpMpReport::Op::kReport, &s, &o);
            printf("report %d: rc %d outcome %d \"%s\"\n", i + 1, rc, int(o), s.c_str());
            if (i < 3 && o != NpMpReport::Op::kSent) return 1;
            if (i == 3 && o != NpMpReport::Op::kLimitReached) return 1;
        }
        return 0;
    }
    if (mode == "delete" || mode == "delete2") {
        const int rc = RunOnce(NpMpReport::Op::kDelete, &s, &o);
        printf("delete: rc %d outcome %d \"%s\"\n", rc, int(o), s.c_str());
        return o == NpMpReport::Op::kDeleted ? 0 : 1;
    }
    return 2;
}
