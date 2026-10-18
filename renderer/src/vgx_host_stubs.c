/* No-op definitions of desktop input / window API entry points the runtime
 * links against but that have no meaning in the headless harness. (The
 * graphics entry points that used to live in vita/src/generic_stubs.c are
 * implemented for real by the renderer.) */
long PADControlMotor(void) { return 0; }
long PADCount(void) { return 0; }
long PADGetAltButtonMappings(void) { return 0; }
long PADGetButtonMappings(void) { return 0; }
long PADGetNameForControllerIndex(void) { return 0; }
long PADGetSDLGamepadForIndex(void) { return 0; }
long PADReset(void) { return 0; }
long PADSetExternalPorts(void) { return 0; }
long SDL_GamepadHasSensor(void) { return 0; }
long SDL_GamepadSensorEnabled(void) { return 0; }
long SDL_GetCurrentDisplayMode(void) { return 0; }
long SDL_GetDisplayForWindow(void) { return 0; }
long SDL_GetGamepadAxis(void) { return 0; }
long SDL_GetGamepadButton(void) { return 0; }
long SDL_GetGamepadFromPlayerIndex(void) { return 0; }
long SDL_GetGamepadID(void) { return 0; }
long SDL_GetGamepadJoystick(void) { return 0; }
long SDL_GetGamepadName(void) { return 0; }
long SDL_GetGamepadSensorData(void) { return 0; }
long SDL_GetHint(void) { return 0; }
long SDL_GetJoystickButton(void) { return 0; }
long SDL_GetPrimaryDisplay(void) { return 0; }
long SDL_GetWindows(void) { return 0; }
long SDL_SetGamepadSensorEnabled(void) { return 0; }
long SDL_SetHint(void) { return 0; }
long SDL_free(void) { return 0; }
