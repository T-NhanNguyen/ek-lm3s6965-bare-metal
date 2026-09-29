## Verify the build

```bash
TCBIN="$HOME/.toolchains/arm-gnu-toolchain-15.3.rel1-darwin-arm64-arm-none-eabi/bin"
$TCBIN/arm-none-eabi-objdump -f build/examples/baremetal/lm3s6965_firmware   # architecture: armv7
$TCBIN/arm-none-eabi-readelf -A build/examples/baremetal/lm3s6965_firmware   # Tag_THUMB_ISA_use: Thumb-2
$TCBIN/arm-none-eabi-size   build/examples/baremetal/lm3s6965_firmware
```

Expect these `readelf -A` attributes: `Tag_CPU_name: "7-M"`, `Tag_CPU_arch: v7`,
`Tag_CPU_arch_profile: Microcontroller`, and `Tag_THUMB_ISA_use: Thumb-2`.
