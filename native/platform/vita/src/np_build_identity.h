// What this build is called: the version a player sees (Settings, log header, the settings file's stamp) and the build id (a hash of the source and the options, vita.mk
// NP_BUILD_ID, the same string the multiplayer fingerprint uses). One place, so that the screen, the log and the stamp cannot disagree. The version is the one definition of
// platform/sdk/net/online/np_app_version.h; tests/test_version_identity.py holds it equal to NP_APP_VER (param.sfo's APP_VER) and cboc-release.json.
#pragma once

#include "../../sdk/net/online/np_app_version.h"

namespace NpBuildIdentity {
inline const char* Version() { return NpNet::BuiltAppVersion(); }   // "2.0.0"
inline const char* BuildId() {
#ifdef NP_BUILD_ID
    return NP_BUILD_ID;
#else
    return "";
#endif
}
}  // namespace NpBuildIdentity
