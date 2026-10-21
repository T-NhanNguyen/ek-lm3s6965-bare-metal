# A/B boot demo (checked-application handoff)

Two near-identical offline LED fixtures, one per authenticated application
slot, sharing wiring, entry, startup/watchdog boundary, link flags, filtered
image-generator logic and package/client shape:

- `slot-a-breathing/` — slot A breathing LED, moved verbatim from the retired
  `examples/boot-breathing-led` (`linker/app_a.ld`, `BASES["A"]`,
  `pwm_envelope.h`, `test_pwm_envelope.c`). Its README preserves the retired
  fixture README, including the retained offline outcome.
- `slot-b-blink/` — slot B dual-blink LED (`linker/app_b.ld`,
  `BASES["B"]`, `blink_pattern.h`, `test_blink_pattern.c`). Same wiring and
  boundary; only the slot and the LED timing differ. No gate recorded.

Both are **default OFF, not hardware ready**. The installed boot image is
decision-only and dispatches neither slot. Enable with
`-DLM3S6965_BUILD_AB_BOOT_DEMO=ON`; the legacy
`-DLM3S6965_BUILD_BOOT_BREATHING_LED=ON` still enables this directory as a
compat alias. These LED-only targets need no public key or production verifier.
Key-free Release configure and LED-only builds were checked after the move;
that is not a new acceptance gate or physical execution result.

## Production FTP variants (separate opt-in)

With the demo enabled, `-DLM3S6965_BUILD_AB_BOOT_DEMO_FTP=ON` additionally
creates `lm3s6965_ab_ftp_a` and `lm3s6965_ab_ftp_b`. This option defaults OFF.
These variants use the foreground FTP receiver with the slot's LED pattern;
they are not the offline LED-only fixtures described above.

FTP configuration requires `-DLM3S6965_BUILD_BOOT_RESET=ON` or
`-DLM3S6965_BUILD_BOOT_DISPATCH_IMAGE=ON`, together with
`-DLM3S6965_BOOT_PHYSICAL_PRODUCTION=ON` and an explicit
`LM3S6965_BOOT_PUBLIC_KEY_FILE` (raw PUBLIC SEC1 bytes). It also requires the
already populated lwIP submodule; configuration does not fetch it. Missing
production verifier/crypto closure is a fatal error, not a demo-root fallback.
See [boot image configuration](../../docs/boot-reset-image.md).

Enabling these targets is not evidence of slot fit, dispatch, watchdog timing
or physical LED/network acceptance. It grants no signing or deployment authority.
