/*
 * Interface between the Win32 core and the AT-SPI side of wine-atspi.
 *
 * This header must include neither Windows nor Linux headers: the Win32
 * core sees it through Wine's msvcrt headers, the AT-SPI side through
 * glibc. Only plain C types cross the boundary; strings are UTF-8.
 */
#ifndef WINE_ATSPI_BRIDGE_H
#define WINE_ATSPI_BRIDGE_H

/*
 * Runs the AT-SPI main loop on the calling thread and returns when it
 * stops. app_name is the name of the one test application to register.
 * Returns 0 on a clean stop, non-zero if the a11y bus was not reachable.
 */
int atspi_run(const char *app_name);

#endif
