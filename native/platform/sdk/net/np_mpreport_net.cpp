// The production environment of np_mpreport.h: the online client's HTTP (np_http), the menu's account files (np_menu_flow.cpp) and the game clock.
#include "platform/sdk/net/np_mpreport.h"

#include "platform/sdk/net/online/np_app_version.h"
#include "platform/sdk/net/online/np_http.h"
#include "platform/sdk/net/online/np_net.h"

extern "C" int NpMenuReportAccount(char* name, size_t nameSize, char* token, size_t tokenSize, char* urls, size_t urlsSize);   // np_menu_flow.cpp: 1 when an account exists
extern "C" void NpMenuForgetAccount(void);                                                                                   // np_menu_flow.cpp
extern "C" unsigned NpLanFingerprint() __attribute__((weak));

namespace NpMpReport {
namespace {

bool Account_(Account* a) {
    return NpMenuReportAccount(a->name, sizeof a->name, a->token, sizeof a->token, a->urls, sizeof a->urls) != 0;
}
void Forget_() { NpMenuForgetAccount(); }
int Start_(const char* method, const std::string& url, const std::string& body, const std::vector<std::string>& headers, uint32_t timeoutMs, std::string* err) {
    return NpNet::HttpStart(method, url, body, headers, timeoutMs, err);
}
int Poll_(int handle, Answer* out) {
    NpNet::HttpResponse r;
    const int rc = NpNet::HttpPoll(handle, &r);
    if (rc == 1) {
        out->error = r.error;
        out->status = r.status;
        out->body = r.body;
        out->text = r.text;
    }
    return rc;
}
void Cancel_(int handle) { NpNet::HttpCancel(handle); }
uint64_t Now_() { return NpNet::SteadyNowMs(0); }
const char* Version_() { return NpNet::DefaultAppVersion(); }
unsigned Fingerprint_() { return NpLanFingerprint ? NpLanFingerprint() : 0u; }

}  // namespace

void UseNetwork() {
    Env e;
    e.account = Account_;
    e.forget = Forget_;
    e.start = Start_;
    e.poll = Poll_;
    e.cancel = Cancel_;
    e.nowMs = Now_;
    e.appVersion = Version_;
    e.fingerprint = Fingerprint_;
    SetEnv(e);
}

}  // namespace NpMpReport
