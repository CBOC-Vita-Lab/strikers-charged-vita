// The console glue of the stale-settings guard (np_settings_guard.h): runs once at start-up, before np-player.txt is read, on the player build only.
#if defined(__vita__) && defined(NP_PLAYER)

#include "np_settings_guard_vita.h"

#include "np_build_identity.h"
#include "np_settings_guard.h"
#include "np_vita_internal.h"
#include "np_vita_settings.h"

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>

#include <cstdio>
#include <string>

namespace NpSettingsGuardVita {
namespace {

const char* g_loadingLine = nullptr;

bool Exists(const char* path) {
    SceIoStat st;
    return sceIoGetstat(path, &st) >= 0;
}

bool ReadAll(const char* path, std::string& out) {
    const SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) return false;
    char buffer[2048];
    for (;;) {
        const int n = sceIoRead(fd, buffer, sizeof(buffer));
        if (n <= 0) break;
        out.append(buffer, static_cast<size_t>(n));
        if (out.size() > 64 * 1024) break;
    }
    sceIoClose(fd);
    return true;
}

// Written complete and synced under a temporary name and put in place by np_safe_file.h, so a power-off leaves the old file or the new one, never half of it (the same
// writer as the Settings screen's save). The backup copy np-player.before-1.3.txt goes the same way.
bool WriteAtomic(const char* path, const std::string& text) { return NpSettingsScreen::WriteSettingsText(path, text); }

}  // namespace

void Run() {
    const char* path = NpSettingsScreen::SavedFilePath();
    std::string text;
    if (!Exists(path) || !ReadAll(path, text)) return;   // no file: the build's own defaults, nothing to guard or to stamp
    if (text.size() > 64 * 1024) return;                  // ReadAll stops there: a cut copy must never replace the player's file (nor become its backup)
    NpSettingsGuard::Report report;
    const std::string migrated = NpSettingsGuard::Migrate(text, NpBuildIdentity::Version(), NpBuildIdentity::BuildId(), report);
    if (report.current) return;
    if (!report.changed) return;
    const std::string backup = std::string("ux0:/data/mscharged/") + report.backupName;
    // The player's own file is kept first. Never over an earlier backup (that one is the oldest file); a failed copy leaves the file as it is.
    if (!Exists(backup.c_str()) && !WriteAtomic(backup.c_str(), text)) {
        VitaLog::Note("settings: np-player.txt is from an older version but its backup could not be written: left as it is");
        std::fprintf(stderr, "[vita] settings guard: backup %s failed, np-player.txt left as it is\n", backup.c_str());
        return;
    }
    if (!WriteAtomic(path, migrated)) {
        VitaLog::Note("settings: np-player.txt from an older version could not be rewritten: left as it is");
        return;
    }
    for (size_t i = 0; i < report.log.size() && i < 6; ++i) {
        std::fprintf(stderr, "[vita] %s\n", report.log[i].c_str());
        if (i < 3) VitaLog::Note(report.log[i].c_str());   // (Note keeps 24 lines per run: the summary and the first two lines are in the basic record)
    }
    g_loadingLine = NpSettingsGuard::LoadingLine(report);
}

const char* LoadingLine() { return g_loadingLine; }

}  // namespace NpSettingsGuardVita

#endif
