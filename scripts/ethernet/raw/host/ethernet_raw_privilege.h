#ifndef ETHERNET_RAW_PRIVILEGE_H
#define ETHERNET_RAW_PRIVILEGE_H

#include <stdbool.h>

/* Nonroot callers are unchanged; root requires valid SUDO_UID/SUDO_GID. */
bool raw_drop_privileges(void);

#endif
