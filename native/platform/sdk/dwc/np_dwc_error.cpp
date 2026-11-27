// DWC_GetLastErrorEx / DWC_ClearError and the error numbers of the online layer.
#include <dwc/dwc_error_fwd.h>
#include <dwc/dwc_error.h>

#include <pthread.h>

#include "platform/sdk/dwc/np_dwc.h"

namespace {
pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
int g_lastError = 0;      // DWCErrorType, 0 = none
int g_errorCode = 0;
void (*g_clearHook)(void) = 0;
}  // namespace

void NpDwcSetError(int lastError, int code) {
    pthread_mutex_lock(&g_lock);
    if (g_lastError != DWC_ERROR_FATAL) {   // a fatal error stays
        g_lastError = lastError;
        g_errorCode = code;
    }
    pthread_mutex_unlock(&g_lock);
}

void NpDwcClearErrorHook(void (*hook)(void)) { g_clearHook = hook; }
void NpDwcErrorResetForTest() { g_lastError = 0; g_errorCode = 0; }

// Numbers the game shows and classifies with its sOnlineErrorPopupRanges (SHOnlineLogin.cpp):
//   20100 -> "unable to connect"   20101 -> "service down"   20102..20109 -> "no connection"   20110 -> "discontinued"   80430 -> "no response"
void NpDwcErrorFor(NpOnline::Fail fail, int* lastError, int* code) {
    int type = DWC_ERROR_NETWORK, number = 20100;
    switch (fail) {
    case NpOnline::Fail::Transport: type = DWC_ERROR_NETWORK; number = 20100; break;
    case NpOnline::Fail::Server: type = DWC_ERROR_AUTH_OUT_OF_SERVICE; number = 20101; break;
    case NpOnline::Fail::Rejected: type = DWC_ERROR_AUTH_ANY; number = 20102; break;
    case NpOnline::Fail::Auth: type = DWC_ERROR_AUTH_ANY; number = 20103; break;
    case NpOnline::Fail::Protocol: type = DWC_ERROR_GHTTP_ANY; number = 20104; break;
    case NpOnline::Fail::Cancelled: type = DWC_ERROR_NETWORK; number = 20105; break;
    case NpOnline::Fail::Timeout: type = DWC_ERROR_GHTTP_ANY; number = 80430; break;
    case NpOnline::Fail::RateLimited: type = DWC_ERROR_AUTH_ANY; number = 20109; break;       // popup "no connection"; the retry waits
    case NpOnline::Fail::Upgrade: type = DWC_ERROR_AUTH_ANY; number = 20110; break;           // popup "service discontinued": this build is too old
    case NpOnline::Fail::Disabled: type = DWC_ERROR_AUTH_OUT_OF_SERVICE; number = 20101; break;   // popup "service down"
    case NpOnline::Fail::None: break;
    }
    if (lastError) *lastError = type;
    if (code) *code = number;
}

extern "C" long DWC_GetLastErrorEx(long* errorCode, DWCErrorType* errorType) {
    pthread_mutex_lock(&g_lock);
    const int last = g_lastError;
    if (errorCode) *errorCode = g_errorCode;
    if (errorType) {   // the original's classification (dwc_error.c)
        switch (last) {
        case 2: case 3: case 4: case 5: case 8: *errorType = (DWCErrorType)6; break;
        case 6: *errorType = (DWCErrorType)3; break;
        case 7: *errorType = (DWCErrorType)4; break;
        case 10: case 11: case 12: case 13: *errorType = (DWCErrorType)1; break;
        case 1: case DWC_ERROR_FATAL: *errorType = (DWCErrorType)7; break;
        case 14: *errorType = (DWCErrorType)5; break;
        case 15: case 17: *errorType = (DWCErrorType)6; break;
        case 16: *errorType = (DWCErrorType)2; break;
        default: *errorType = (DWCErrorType)0; break;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return last;
}

extern "C" void DWC_ClearError() {
    pthread_mutex_lock(&g_lock);
    const bool cleared = g_lastError != DWC_ERROR_FATAL;
    if (cleared) {
        g_lastError = 0;
        g_errorCode = 0;
    }
    pthread_mutex_unlock(&g_lock);
    if (cleared && g_clearHook) g_clearHook();
}
