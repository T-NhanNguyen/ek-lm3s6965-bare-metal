#!/usr/bin/env python3
"""Create a new external demo signing identity; never rotate or overwrite one."""
import argparse
import hashlib
import importlib.util
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def package_tools():
    spec = importlib.util.spec_from_file_location("boot_package", ROOT / "tools/boot/package.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def output_directory(path):
    """Reject aliases before resolving; only the final directory may be new."""
    path = Path(path).absolute()
    for component in (*reversed(path.parents), path):
        if component.is_symlink():
            raise ValueError("output path must not contain symlinks")
    if path.resolve().is_relative_to(ROOT):
        raise ValueError("output directory must be outside the repository")
    if path.exists():
        info = path.stat()
        if (not stat.S_ISDIR(info.st_mode) or stat.S_IMODE(info.st_mode) != 0o700
                or info.st_uid != os.getuid()):
            raise ValueError("existing output directory must be owned by you with mode 0700")
    return path.resolve()


def env_content(public, private):
    # The build wrapper accepts literal matched quotes, not shell escapes, and
    # splits Unicode line boundaries as well as ASCII newlines.
    paths = (str(public), str(private))
    if any(any(c in value for c in "\r\n\x00\"'\x85\u2028\u2029") or
           any(ord(c) < 32 or ord(c) == 127 for c in value) for value in paths):
        raise ValueError("output path cannot be represented in literal .env configuration")
    content = (f'LM3S6965_BOOT_PUBLIC_KEY_FILE="{public}"\n'
               f'LM3S6965_BOOT_PRIVATE_KEY_FILE="{private}"\n'
               'LM3S6965_BOOT_RELEASE_A=1\nLM3S6965_BOOT_RELEASE_B=2\n')
    if len(content.encode("utf-8")) > 4096:
        raise ValueError("output paths exceed .env size limit")
    return content.encode("utf-8")


def publish(source, destination, owned):
    """Hard-link publication refuses even dangling symlink destinations."""
    os.link(source, destination)
    owned.append((destination, source.stat().st_ino))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, epilog=(
        "Requires installed OpenSSL. Existing identities are refused, never rotated. "
        "No build, flash or initial board provisioning is performed."))
    parser.add_argument("--out-dir", type=Path,
                        default=ROOT.parent / (ROOT.name + "-keys"),
                        help="external mode-0700 directory (default: resolved sibling <repo>-keys)")
    parser.add_argument("--init-env", action="store_true",
                        help="exclusively create absent repository .env, releases 1 and 2")
    args = parser.parse_args(argv)
    owned, created_directory, complete = [], None, False
    try:
        if args.init_env and os.path.lexists(ROOT / ".env"):
            raise ValueError(".env already exists; preserved unchanged. Recover its original key "
                             "from backup. If lost, deliberately plan a new identity and matching "
                             "device bootloader root, then manually migrate .env; FTP cannot rotate it.")
        output = output_directory(args.out_dir)
        private, public = output / "signing-key.pem", output / "authorized-public.sec1"
        content = env_content(public, private)
        if any(os.path.lexists(path) for path in (private, public)):
            raise ValueError("key identity already exists; no files overwritten. Use the existing "
                             "pair and manually configure .env, or choose a new external directory.")
        if shutil.which("openssl") is None:
            raise ValueError("OpenSSL is required; install it before setup")
        tools = package_tools()
        if not output.exists():
            output.mkdir(mode=0o700)
            created_directory = output
        # All temporary files stay inside the private directory. Only OpenSSL reads
        # private bytes; genpkey output is a file, never a Python pipe or log.
        with tempfile.TemporaryDirectory(prefix=".setup-", dir=output) as temporary:
            staging = Path(temporary)
            key, point = staging / private.name, staging / public.name
            fd = os.open(key, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
            os.close(fd)
            result = subprocess.run(["openssl", "genpkey", "-algorithm", "EC", "-pkeyopt",
                                     "ec_paramgen_curve:P-256", "-pkeyopt",
                                     "ec_param_enc:named_curve", "-out", str(key)],
                                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            if result.returncode:
                raise ValueError("OpenSSL key generation failed")
            der = tools.openssl_run(["pkey", "-in", str(key), "-pubout", "-outform", "DER",
                                     "-ec_conv_form", "uncompressed", "-passin", "pass:"])
            tools.require(len(der) == 91 and der[:26] == tools.SPKI_PREFIX,
                          "key must be named-curve P-256")
            raw = tools.validate_public(der[26:])
            tools.require(raw != tools.trusted_public(), "production requires a non-demo root")
            with point.open("xb") as stream:
                stream.write(raw)
            tools.validate_key(key, point)
            publish(key, private, owned)
            publish(point, public, owned)
            complete = True
            # A later config error must not destroy a successfully published identity.
            if args.init_env:
                env_stage = staging / "env"
                fd = os.open(env_stage, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
                with os.fdopen(fd, "wb") as stream:
                    stream.write(content)
                    stream.flush()
                    os.fsync(stream.fileno())
                os.link(env_stage, ROOT / ".env")
        print("Signing identity created outside the repository; keep a secure backup.")
        print("Public key SHA256 (raw SEC1): " + hashlib.sha256(raw).hexdigest())
        print("Next: review .env paths and releases before scripts/build_ab_boot_demo.")
        return 0
    except (ValueError, OSError) as error:
        if not complete:
            for path, inode in reversed(owned):
                if path.exists() and not path.is_symlink() and path.stat().st_ino == inode:
                    path.unlink()
            if created_directory is not None:
                try:
                    created_directory.rmdir()
                except OSError:
                    pass  # Never remove pre-existing or concurrently created user files.
        detail = str(error) if isinstance(error, ValueError) else "filesystem/OpenSSL operation failed"
        print("Key setup: " + detail, file=sys.stderr)
        if complete:
            print("Key pair preserved; manually configure .env to use it. Do not regenerate.", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
