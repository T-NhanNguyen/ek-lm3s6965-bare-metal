#!/usr/bin/env python3
"""Validate an explicitly supplied raw PUBLIC P-256 root; emit build-only C data.

No signing, private-key handling, third-party dependencies or signature logic.
"""

import argparse
import hashlib
from pathlib import Path
import sys


DEMO_SHA256 = "f3ab1a07197e14750365367b1c09a2b0848c7062a419f82d7f1b087d03e7518d"
P = 0xFFFFFFFF00000001000000000000000000000000FFFFFFFFFFFFFFFFFFFFFFFF
B = 0x5AC635D8AA3A93E7B3EBBD55769886BC651D06B0CC53B0F63BCE3C3E27D2604B


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--public-key", required=True, type=Path)
    parser.add_argument("--header", required=True, type=Path)
    args = parser.parse_args()
    # Never leave a previously selected root available after failed validation.
    args.header.unlink(missing_ok=True)
    try:
        # Read only the explicitly supplied public file, bounded to detect excess.
        with args.public_key.open("rb") as public_file:
            key = public_file.read(66)
        if len(key) != 65 or key[0] != 4:
            raise ValueError("expected exactly 65 raw SEC1 uncompressed P-256 PUBLIC bytes")
        x = int.from_bytes(key[1:33], "big")
        y = int.from_bytes(key[33:65], "big")
        if x >= P or y >= P or (y * y - (x * x * x - 3 * x + B)) % P:
            raise ValueError("PUBLIC coordinates are out of range or not on P-256")
        # P-256 has cofactor one; a finite on-curve point is in its prime subgroup.
        fingerprint = hashlib.sha256(key).hexdigest()
        if fingerprint == DEMO_SHA256:
            raise ValueError("repository demo PUBLIC key is forbidden in production")
        data = ", ".join(f"0x{byte:02x}" for byte in key)
        args.header.parent.mkdir(parents=True, exist_ok=True)
        args.header.write_text(
            "/* Generated PUBLIC material only; build-selected, not authorization. */\n"
            "#ifndef LM3S6965_BOOT_PRODUCTION_PUBLIC_KEY_H\n"
            "#define LM3S6965_BOOT_PRODUCTION_PUBLIC_KEY_H\n"
            "#include <stdint.h>\n"
            "#define LM3S6965_BOOT_GENERATED_PUBLIC_ROOT 1\n"
            f"static const uint8_t lm3s6965_boot_public_key[65] = {{ {data} }};\n"
            "#endif\n", encoding="ascii")
        print(fingerprint)
    except (OSError, ValueError) as error:
        print(f"Production PUBLIC root rejected: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
