// The match camera's default zoom on the Vita and the one-time move of a save that holds the TV default: see include/np/camzoom.h.
#include "np/camzoom.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if __has_include("np/np_sdk.h")
#include "np/np_sdk.h"
extern "C" const NpSdkConfig* NpSdk_GetConfig(void) __attribute__((weak));
#define NP_CAMZOOM_HAVE_SDK 1
#endif

namespace {

const int kTvDefaultLevel = 3;   // retail's VisualSettings(): Auto Zoom on, mCameraZoomLevel 0.5
const int kMaxLevel = NP_CAMERA_ZOOM_MAX_LEVEL;   // 7: levels 6 and 7 continue the menu's steps closer than retail's level 5 (17 Oct)

int EnvLevel(const char* name)
{
    const char* e = getenv(name);
    if (e && e[0] >= '1' && e[0] <= '0' + kMaxLevel && e[1] == 0) return e[0] - '0';
    if (e && *e) fprintf(stderr, "[camera] %s=%s ignored (a level 1..%d)\n", name, e, kMaxLevel);
    return 0;
}

int EnvFlag(const char* name)
{
    const char* e = getenv(name);
    if (!e || !*e) return -1;
    return *e != '0';
}

// The game's units: the Options menu stores item / 4.0 (item 0..4 = level 1..5; 5, 6 = levels 6, 7) and reads (int)(4 * level).
float LevelToZoom(int level) { return (float)(level - 1) / 4.0f; }
int ZoomToLevel(float zoom) { return (int)(4.0f * zoom) + 1; }
bool IsLevel(float zoom, int level) { return zoom == LevelToZoom(level); }

void DescribeZoom(char* out, size_t size, bool autoZoom, float zoom)
{
    if (zoom >= 0.0f && zoom <= LevelToZoom(kMaxLevel) && IsLevel(zoom, ZoomToLevel(zoom)))
        snprintf(out, size, "level %d, Auto Zoom %s", ZoomToLevel(zoom), autoZoom ? "on" : "off");
    else
        snprintf(out, size, "zoom %g (not one of the menu's levels), Auto Zoom %s", (double)zoom, autoZoom ? "on" : "off");
}

// <data dir>/np-camera-zoom.txt, the directory that holds the save's NAND/ (ux0:data/mscharged/np-data on the Vita by default).
const char* MarkerPath()
{
    static char path[1100];
    const char* dir = nullptr;
#ifdef NP_CAMZOOM_HAVE_SDK
    if (NpSdk_GetConfig) dir = NpSdk_GetConfig()->data_dir;
#endif
    if (!dir || !*dir) dir = getenv("NP_DATA_DIR");
    if (!dir || !*dir) dir = ".";
    snprintf(path, sizeof path, "%s/np-camera-zoom.txt", dir);
    return path;
}

bool MarkerExists()
{
    FILE* f = fopen(MarkerPath(), "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

void ApplyTestOverride(bool* autoZoom, float* zoom, const char* what)
{
    const int level = EnvLevel("MSC_CAMERA_ZOOM");
    const int autoFlag = EnvFlag("MSC_CAMERA_AUTO");
    if (!level && autoFlag < 0) return;
    if (level) *zoom = LevelToZoom(level);
    if (autoFlag >= 0) *autoZoom = autoFlag != 0;
    char d[96];
    DescribeZoom(d, sizeof d, *autoZoom, *zoom);
    fprintf(stderr, "[camera] test override (MSC_CAMERA_ZOOM / MSC_CAMERA_AUTO) on the %s: %s\n", what, d);
}

} // namespace

extern "C" int NpCameraZoomDefaultLevel(void)
{
    const int level = EnvLevel("MSC_CAMERA_ZOOM_DEFAULT");
    if (level) return level;
    return (NP_CAMERA_ZOOM_DEFAULT >= 1 && NP_CAMERA_ZOOM_DEFAULT <= kMaxLevel) ? NP_CAMERA_ZOOM_DEFAULT : kTvDefaultLevel;
}

// Levels 6 and 7 (see np/camzoom.h): the on-screen test of the players machine 0 sends to the simulation keeps about what level 5 would see,
// so the closer picture does not change how the CPU team plays (it reads "on screen" for passes, power-ups, marking). Level 6: the window
// reaches 1.175 x the screen, level 7: 1.35 x (the camera is about that much closer in open play). MSC_CAMERA_VIS=screen: the real screen.
extern "C" float NpCameraVisibilityWindow(float zoomLevel)
{
    static const bool screenOnly = [] { const char* e = getenv("MSC_CAMERA_VIS"); return e && strcmp(e, "screen") == 0; }();
    if (zoomLevel <= 1.0f || screenOnly) return 0.95f;
    const float extra = zoomLevel > 1.5f ? 0.5f : zoomLevel - 1.0f;
    return 0.95f * (1.0f + 1.4f * extra);
}

extern "C" void NpCameraZoomNewSettings(bool* autoZoom, float* zoomLevel)
{
    const int level = NpCameraZoomDefaultLevel();
    *zoomLevel = LevelToZoom(level);   // level 3 = 0.5f exactly: retail's value
    static bool said = false;
    if (!said && level != kTvDefaultLevel)
    {
        said = true;
        fprintf(stderr, "[camera] default camera for a new save: zoom level %d, Auto Zoom %s (%s)\n", level, *autoZoom ? "on" : "off",
                getenv("MSC_CAMERA_ZOOM_DEFAULT") && EnvLevel("MSC_CAMERA_ZOOM_DEFAULT") ? "MSC_CAMERA_ZOOM_DEFAULT" : "the build's default");
    }
    ApplyTestOverride(autoZoom, zoomLevel, "new settings");
}

extern "C" void NpCameraZoomSettingsLoaded(bool* autoZoom, float* zoomLevel)
{
    const int target = NpCameraZoomDefaultLevel();
    char before[96];
    DescribeZoom(before, sizeof before, *autoZoom, *zoomLevel);
    const bool tvDefault = *autoZoom && IsLevel(*zoomLevel, kTvDefaultLevel);
    if (target != kTvDefaultLevel && tvDefault && !MarkerExists())
    {
        *zoomLevel = LevelToZoom(target);
        fprintf(stderr,
                "[camera] the save holds the TV default camera (level 3, Auto Zoom on) and %s does not exist yet: zoom level %d from now on "
                "(one-time move; the marker is written when the game next writes its save, and from then on the save's level is kept)\n",
                MarkerPath(), target);
    }
    else
    {
        fprintf(stderr, "[camera] save loaded: %s (%s)\n", before,
                target == kTvDefaultLevel ? "the default level is 3: nothing to move"
                : !tvDefault              ? "not the TV default: the player's choice, kept"
                                          : "the marker exists: the player's choice, kept");
    }
    ApplyTestOverride(autoZoom, zoomLevel, "loaded save");
}

extern "C" void NpCameraZoomSettingsWritten(bool autoZoom, float zoomLevel)
{
    if (MarkerExists()) return;
    const char* path = MarkerPath();
    char d[96];
    DescribeZoom(d, sizeof d, autoZoom, zoomLevel);
    FILE* f = fopen(path, "wb");
    if (!f)
    {
        fprintf(stderr, "[camera] cannot write %s: the one-time camera move may run again at the next start\n", path);
        return;
    }
    char when[32] = "";
    time_t now = time(nullptr);
    if (const struct tm* t = localtime(&now)) strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", t);
    fprintf(f,
            "# Mario Strikers Charged (native): the camera zoom of the save in this directory is the player's own choice.\n"
            "# Written when the game first wrote its save (%s) with %s. While this file exists the native build never changes the\n"
            "# camera level of the save (the one-time move of a save that held the TV default, level 3 with Auto Zoom on, to level %d\n"
            "# is over). Delete it only to have that move run again.\n"
            "written=%s\n",
            when, d, NpCameraZoomDefaultLevel(), d);
    fclose(f);
    fprintf(stderr, "[camera] marker %s written (the save now holds %s)\n", path, d);
}

extern "C" int NpCameraTestVisibility(void)
{
    static int mode = -2;
    if (mode == -2)
    {
        const char* e = getenv("MSC_TEST_VISIBILITY");
        mode = -1;
        if (e && !strcmp(e, "all")) mode = 0x1F;
        else if (e && !strcmp(e, "none")) mode = 0;
        else if (e && *e) fprintf(stderr, "[camera] MSC_TEST_VISIBILITY=%s ignored (all or none)\n", e);
        if (mode >= 0) fprintf(stderr, "[camera] TEST: MSC_TEST_VISIBILITY=%s: machine 0 sends every player as %s screen (changes the match)\n", e, mode ? "on" : "off");
    }
    return mode;
}
