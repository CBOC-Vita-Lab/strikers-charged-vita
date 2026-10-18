// Unit test of the camera zoom default and the one-time move (np/camzoom.h, platform/compat/np_camzoom.cpp), host only:
//   clang++ -m32 -std=gnu++17 -DNP_NATIVE=1 -Iinclude -Iplatform/sdk/include tools/cam/test_camzoom.cpp platform/compat/np_camzoom.cpp -o $TMPDIR/t && $TMPDIR/t
// The SDK is not linked: NpSdk_GetConfig is a weak reference, so the marker goes to $NP_DATA_DIR (a temporary directory per case).
#include "np/camzoom.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_fail = 0, g_ok = 0;
#define CHECK(c) do { if (c) ++g_ok; else { ++g_fail; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static char g_dir[256];
static void FreshDir(const char* name)
{
    const char* tmp = getenv("TMPDIR");
    snprintf(g_dir, sizeof g_dir, "%s/camzoom-test-%d-%s", tmp && *tmp ? tmp : "/tmp", (int)getpid(), name);
    char cmd[400];
    snprintf(cmd, sizeof cmd, "rm -rf %s && mkdir -p %s", g_dir, g_dir);
    if (system(cmd) != 0) { fprintf(stderr, "cannot make %s\n", g_dir); exit(2); }
    setenv("NP_DATA_DIR", g_dir, 1);
}
static bool Marker()
{
    char p[300];
    snprintf(p, sizeof p, "%s/np-camera-zoom.txt", g_dir);
    FILE* f = fopen(p, "rb");
    if (f) fclose(f);
    return f != NULL;
}
static float Lv(int level) { return (level - 1) / 4.0f; }

// one "start of the game": new settings (constructor), then optionally a load of a save holding (autoZoom, level)
struct Settings { bool autoZoom; float zoom; };
static Settings NewSave()
{
    Settings s{true, 0.5f};
    NpCameraZoomNewSettings(&s.autoZoom, &s.zoom);
    return s;
}
static Settings Load(bool autoZoom, float zoom)
{
    Settings s = NewSave();
    s.autoZoom = autoZoom;
    s.zoom = zoom;   // DeserializeSettings
    NpCameraZoomSettingsLoaded(&s.autoZoom, &s.zoom);
    return s;
}

int main()
{
    unsetenv("MSC_CAMERA_ZOOM");
    unsetenv("MSC_CAMERA_AUTO");

    // ---- host default (NP_CAMERA_ZOOM_DEFAULT 3): retail's camera, nothing moves ----
    unsetenv("MSC_CAMERA_ZOOM_DEFAULT");
    FreshDir("host");
    CHECK(NpCameraZoomDefaultLevel() == NP_CAMERA_ZOOM_DEFAULT);
#if NP_CAMERA_ZOOM_DEFAULT == 3
    {
        Settings s = NewSave();
        CHECK(s.autoZoom && s.zoom == 0.5f);   // retail's value, bit for bit
        s = Load(true, 0.5f);
        CHECK(s.autoZoom && s.zoom == 0.5f);
        CHECK(!Marker());
    }
#endif

    // ---- the Vita's default, through the environment ----
    setenv("MSC_CAMERA_ZOOM_DEFAULT", "4", 1);
    CHECK(NpCameraZoomDefaultLevel() == 4);

    // 1. fresh save: new settings at level 4, Auto on; the first save write makes the marker; the next start loads 4 and keeps it
    FreshDir("fresh");
    {
        Settings s = NewSave();
        CHECK(s.autoZoom && s.zoom == 0.75f && (int)(4.0f * s.zoom) == 3);   // the Options menu shows button 3 = "4"
        CHECK(!Marker());
        NpCameraZoomSettingsWritten(s.autoZoom, s.zoom);
        CHECK(Marker());
        s = Load(true, 0.75f);
        CHECK(s.autoZoom && s.zoom == 0.75f);
        // the player then picks level 3 in Options and the game writes the save: kept at every later start
        NpCameraZoomSettingsWritten(true, 0.5f);
        s = Load(true, 0.5f);
        CHECK(s.autoZoom && s.zoom == 0.5f);
        s = Load(true, 0.5f);
        CHECK(s.autoZoom && s.zoom == 0.5f);
    }

    // 2. imported save at the TV default (level 3, Auto on), no marker: moved to 4; again at the next start while the file was not rewritten;
    //    after the first write the marker exists, and a 3 chosen later stays 3
    FreshDir("import");
    {
        Settings s = Load(true, 0.5f);
        CHECK(s.autoZoom && s.zoom == 0.75f);
        CHECK(!Marker());
        s = Load(true, 0.5f);   // second start without a save write in between (the file still holds 3)
        CHECK(s.autoZoom && s.zoom == 0.75f);
        NpCameraZoomSettingsWritten(s.autoZoom, s.zoom);   // the game writes its save (level 4)
        CHECK(Marker());
        s = Load(true, 0.75f);
        CHECK(s.zoom == 0.75f);
        NpCameraZoomSettingsWritten(true, 0.5f);   // the player picks 3 in Options
        s = Load(true, 0.5f);
        CHECK(s.autoZoom && s.zoom == 0.5f);       // never changed again
    }

    // 3. a save at level 2 (Auto on), or at level 3 with Auto off: the player's choice, left alone
    FreshDir("level2");
    {
        Settings s = Load(true, Lv(2));
        CHECK(s.autoZoom && s.zoom == Lv(2));
        s = Load(false, 0.5f);
        CHECK(!s.autoZoom && s.zoom == 0.5f);
        s = Load(false, Lv(5));
        CHECK(!s.autoZoom && s.zoom == Lv(5));
        CHECK(!Marker());
        NpCameraZoomSettingsWritten(true, Lv(2));
        CHECK(Marker());
        s = Load(true, Lv(2));
        CHECK(s.zoom == Lv(2));
    }

    // 4. a marker already there (an earlier start wrote the save): level 3 + Auto is kept
    FreshDir("marker");
    {
        NpCameraZoomSettingsWritten(true, 0.5f);
        CHECK(Marker());
        Settings s = Load(true, 0.5f);
        CHECK(s.autoZoom && s.zoom == 0.5f);
    }

    // 5. the default set to 3 (MSC_CAMERA_ZOOM_DEFAULT=3): nothing moves even without a marker
    setenv("MSC_CAMERA_ZOOM_DEFAULT", "3", 1);
    FreshDir("def3");
    {
        Settings s = Load(true, 0.5f);
        CHECK(s.zoom == 0.5f);
        CHECK(NewSave().zoom == 0.5f);
    }
    // a bad value is ignored (the build's default applies)
    setenv("MSC_CAMERA_ZOOM_DEFAULT", "8", 1);
    CHECK(NpCameraZoomDefaultLevel() == NP_CAMERA_ZOOM_DEFAULT);
    // levels 6 and 7 (17 Oct): accepted, and the on-screen window widens only past level 5
    setenv("MSC_CAMERA_ZOOM_DEFAULT", "7", 1);
    CHECK(NpCameraZoomDefaultLevel() == 7);
    CHECK(NpCameraVisibilityWindow(1.0f) == 0.95f && NpCameraVisibilityWindow(0.5f) == 0.95f);
    CHECK(NpCameraVisibilityWindow(1.25f) > 1.1f && NpCameraVisibilityWindow(1.5f) > NpCameraVisibilityWindow(1.25f));
    setenv("MSC_CAMERA_ZOOM_DEFAULT", "4", 1);

    // 5b. the Vita's default since 17 Oct 07:27 (level 5, the closest): a fresh save and an imported TV-default save both get 5
    setenv("MSC_CAMERA_ZOOM_DEFAULT", "5", 1);
    FreshDir("def5");
    {
        CHECK(NpCameraZoomDefaultLevel() == 5);
        Settings s = NewSave();
        CHECK(s.autoZoom && s.zoom == 1.0f && (int)(4.0f * s.zoom) == 4);   // the Options menu shows button 4 = "5"
        s = Load(true, 0.5f);
        CHECK(s.autoZoom && s.zoom == 1.0f);
        NpCameraZoomSettingsWritten(s.autoZoom, s.zoom);
        CHECK(Marker());
        NpCameraZoomSettingsWritten(true, 0.5f);   // the player picks 3 later: kept
        s = Load(true, 0.5f);
        CHECK(s.zoom == 0.5f);
    }
    setenv("MSC_CAMERA_ZOOM_DEFAULT", "4", 1);

    // 6. the test override wins over new and loaded settings, and changes no marker
    FreshDir("override");
    setenv("MSC_CAMERA_ZOOM", "5", 1);
    setenv("MSC_CAMERA_AUTO", "0", 1);
    {
        Settings s = NewSave();
        CHECK(!s.autoZoom && s.zoom == 1.0f);
        s = Load(true, Lv(2));
        CHECK(!s.autoZoom && s.zoom == 1.0f);
        CHECK(!Marker());
    }
    unsetenv("MSC_CAMERA_AUTO");
    {
        Settings s = Load(false, Lv(2));
        CHECK(!s.autoZoom && s.zoom == 1.0f);   // only the level is forced
    }
    unsetenv("MSC_CAMERA_ZOOM");

    char cmd[300];
    const char* tmp = getenv("TMPDIR");
    snprintf(cmd, sizeof cmd, "rm -rf %s/camzoom-test-%d-*", tmp && *tmp ? tmp : "/tmp", (int)getpid());
    if (system(cmd) != 0) fprintf(stderr, "(cleanup failed)\n");
    printf("%s: %d checks, %d failed\n", g_fail ? "FAILED" : "all passed", g_ok + g_fail, g_fail);
    return g_fail ? 1 : 0;
}
