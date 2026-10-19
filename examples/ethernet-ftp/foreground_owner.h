#ifndef FTP_FOREGROUND_OWNER_H
#define FTP_FOREGROUND_OWNER_H
#include <stdbool.h>

/* Opt-in singleton foreground owner. Call only after real application init and
 * C_READY. Uses boot_startup_continuity in place, never adopts/reinitializes it.
 * False quarantines this owner permanently: caller must stop without retry.
 * Active reentry marks failure and calls nonreturning boot_startup_stop(), so
 * the suspended turn cannot resume work or feed. Already-failed inactive calls
 * return false without effects.
 * No candidate deadline, acceptance grace/reset actuator or ftp_boot binding. */
bool foreground_owner_turn(void);
#endif
