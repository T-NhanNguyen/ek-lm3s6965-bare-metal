"""Pinned offline gate compiler selection; never searches PATH or executes tools."""
import os
from pathlib import Path


def arm_gcc():
    """Return the pinned home default or an explicit executable BOOT_ARM_GCC.

    Explicit overrides must be nonempty absolute paths to executable files.
    Invalid overrides raise ValueError, never falling back to another compiler.
    The default is not probed here; existing gate preflights validate its bundle.
    """
    if 'BOOT_ARM_GCC' not in os.environ:
        return Path.home() / '.toolchains/arm-gnu-toolchain-15.3.rel1-darwin-arm64-arm-none-eabi/bin/arm-none-eabi-gcc'
    value = os.environ['BOOT_ARM_GCC']
    compiler = Path(value)
    if not value or not compiler.is_absolute():
        raise ValueError('BOOT_ARM_GCC must be a nonempty absolute path to an executable compiler')
    if not compiler.is_file() or not os.access(compiler, os.X_OK):
        raise ValueError(f'BOOT_ARM_GCC is not an executable file: {value}')
    return compiler
