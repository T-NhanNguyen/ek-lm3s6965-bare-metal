#ifndef FTP_FOREGROUND_OWNER_H
#define FTP_FOREGROUND_OWNER_H
#include <stdbool.h>

/* Opt-in singleton foreground owner. Call only after real application init and
 * C_READY. Uses boot_startup_continuity in place, never adopts/reinitializes it.
 * False quarantines this owner permanently: caller must stop without retry.
 * Active reentry marks failure and calls nonreturning boot_startup_stop(), so
 * the suspended turn cannot resume work or feed. Already-failed inactive calls
 * return false without effects.
 * No candidate deadline, acceptance grace/reset actuator, or enabled updater.
 * The only firmware-receive path is the opt-in ftp_firmware_receive bind
 * (STOR firmware/confirm, RETR boot-status) onto the shared ftp_core
 * singleton; without production flash/receiver/publication/sources it stays
 * RAM-only hello. Watchdog servicing remains the caller's SAME-APPLICATION
 * epoch duty with genuine ETHERNET/TIMERS/FTP reports. */
bool foreground_owner_turn(void);
#endif
