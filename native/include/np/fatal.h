/* np/fatal.h (1.2 audit): the reason of a fatal path, kept without a logging build.
 *
 *   NpFatalNote(kind, caller, detail)   one line for the player's basic log (the Vita: ux0:data/mscharged/log.txt, written only when
 *                                       something like this happens, never during normal play; at most 64 lines per run and the file
 *                                       is rotated at 96 KiB). kind names the path ("nlBreak", "OSPanic", "abort" ...), caller is the
 *                                       code address that raised it (__builtin_return_address(0)), detail is free text (or NULL).
 *                                       On the Vita the line also lists the return addresses found on the stack above the caller (a
 *                                       poor man's backtrace: look the addresses up in the build's .map or with vita-parse-core's ELF).
 *   The platform provides the sink (NpPlatformFatalLine, weak: without it the line goes to stderr).
 */
#ifndef NP_FATAL_H
#define NP_FATAL_H

#ifdef __cplusplus
extern "C" {
#endif
void NpFatalNote(const char* kind, const void* caller, const char* detail);
#ifdef __cplusplus
}
#endif

#endif /* NP_FATAL_H */
