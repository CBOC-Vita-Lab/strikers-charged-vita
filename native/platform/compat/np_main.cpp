// Default program entry for the np-<target> executables.
// src/Game/main.cpp is compiled with -Dmain=np_game_main (see Makefile), so the game's own `main`
// is the C++ function np_game_main(). Platform harnesses (NPV: platform/harness) override this weak
// `main` with a strong one that sets up the platform first and then calls np_game_main().
int np_game_main();

extern "C" __attribute__((weak)) int main(int, char**)
{
    return np_game_main();
}
