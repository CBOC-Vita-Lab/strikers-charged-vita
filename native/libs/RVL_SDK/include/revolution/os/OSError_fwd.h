#ifndef REVOLUTION_OS_ERROR_FWD_H
#define REVOLUTION_OS_ERROR_FWD_H

#ifdef __cplusplus
extern "C" {
#endif

void OSReport(const char* message, ...) __attribute__((format(printf, 1, 2)));

#ifdef __cplusplus
}
#endif

#endif
