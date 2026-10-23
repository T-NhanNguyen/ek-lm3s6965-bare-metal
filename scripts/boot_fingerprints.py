#!/usr/bin/env python3
"""Fingerprint allowlisted PUBLIC boot sources; not full environment evidence."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import sys


ROOT = Path(__file__).resolve().parents[1]
SOURCE_DIRS = (
    "src", "include/lm3s6965", "config", "cmake",
    "third_party/mbedtls/include", "third_party/mbedtls/library",
    "tests/boot/physical_root", "freertos", "examples",
)
SOURCE_SUFFIXES = (".c", ".h", ".S", ".cmake", ".txt", ".ld")
REQUIRED_FILES = (
    "CMakeLists.txt", "include/CMakeLists.txt",
    "scripts/validate-boot-public-key.py", "tests/boot/CMakeLists.txt",
    "tests/boot/run_physical_root.py", "tests/boot/physical_root_manifest.json",
    "tests/boot/run_sequence.py", "tests/boot/arm_compiler.py", "tests/boot/sequence_host.c",
    "tests/boot/flash_model.c", "tests/boot/flash_model.h",
    "tests/boot/verify_fixtures.h", "tests/boot/test_crypto.c",
    "scripts/boot_fingerprints.py",
)

SEQUENCE_SOURCE_DIRS = (
    "include/lm3s6965", "config",
    "third_party/mbedtls/include", "third_party/mbedtls/library",
)
SEQUENCE_REQUIRED_FILES = (
    "src/CMakeLists.txt", "tests/boot/CMakeLists.txt", "cmake/boot_crypto.cmake",
    "tests/boot/sequence_host.c", "tests/boot/run_sequence.py", "tests/boot/arm_compiler.py",
    "tests/boot/sequence_manifest.json", "tests/boot/flash_model.c",
    "tests/boot/flash_model.h", "tests/boot/verify_fixtures.h",
    "docs/boot-startup.md", "scripts/boot_fingerprints.py",
)


def _public_path(path):
    """Reject known private paths before any content read, including aliases."""
    path = Path(path).absolute()
    for candidate in (path, path.resolve()):
        for part in candidate.parts:
            if (part in (".dev-vault", ".git", ".pi", "resume.tex", ".env",
                         "privatekeys", "private-keys", "id_rsa", "id_ed25519")
                    or part.startswith(".env.")):
                raise ValueError(f"private input is forbidden: {path}")
    return path


def sha(path):
    """Return a file's SHA256 hex digest, streaming bytes without executing it."""
    path = _public_path(path)
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _walk(directory):
    """Walk only an explicitly selected directory, never private subtrees."""
    directory = _public_path(directory)
    if not directory.is_dir():
        return
    def fail(error):
        raise error
    for base, dirs, files in os.walk(directory, followlinks=False, onerror=fail):
        for name in list(dirs):
            child = Path(base) / name
            _public_path(child)
            if child.is_symlink():
                raise ValueError(f"directory symlink is not traversed: {child}")
        for name in files:
            yield Path(base) / name


def _is_source_path(relative, profile):
    if profile == "physical-root":
        return (relative.suffix in SOURCE_SUFFIXES
                and any(relative.is_relative_to(Path(name)) for name in SOURCE_DIRS))
    return (relative.suffix in (".c", ".h")
            and (any(relative.is_relative_to(Path(name)) for name in SEQUENCE_SOURCE_DIRS)
                 or (relative.parent == Path("src") and relative.name.startswith("boot_"))))


def _source_files(root, profile="physical-root"):
    root = _public_path(root).resolve()
    if profile == "physical-root":
        required, directories = REQUIRED_FILES, SOURCE_DIRS
    elif profile == "sequence":
        required, directories = SEQUENCE_REQUIRED_FILES, (*SEQUENCE_SOURCE_DIRS, "src")
    else:
        raise ValueError(f"unknown source profile: {profile}")
    files = {root / name for name in required}
    for name in directories:
        directory = root / name
        # Reject directory aliases rather than following them outside the allowlist.
        for parent in (directory, *directory.parents):
            if parent == root:
                break
            if parent.is_symlink():
                raise ValueError(f"source directory symlink: {parent}")
        if profile == "sequence" and name == "src":
            candidates = (*directory.glob("boot_*.c"), *directory.glob("boot_*.h"))
        else:
            candidates = _walk(directory)
        files.update(path for path in candidates
                     if _is_source_path(path.relative_to(root), profile) and path.is_file())
    for path in files:
        _public_path(path)
        if path.resolve() != path:
            raise ValueError(f"source symlink is not allowed: {path}")
        if not path.is_file():
            raise ValueError(f"required source file missing: {path.relative_to(root)}")
    return root, sorted(files)


def source_fingerprints(root, profile="physical-root"):
    """Return sorted {repository-relative POSIX path: SHA256} public sources.

    Profile is 'physical-root' (default) or 'sequence'; unknown profiles fail.
    Missing explicit required files fail closed. Absent glob directories retain
    the original runner's empty-match behavior. Source symlinks fail closed.
    No compiler, Git command, vault document or environment snapshot is used.
    """
    root, files = _source_files(root, profile)
    return {path.relative_to(root).as_posix(): sha(path) for path in files}


def tool_fingerprints(tools=None, bundles=()):
    """Hash explicit {label: command-or-path} and complete bundle directories.

    Commands resolve using PATH but are never executed. Bundle file symlinks
    must resolve inside that bundle; directory symlinks fail closed. Missing
    requests and empty bundles are errors, never silently omitted.
    """
    result = {"tools": {}, "tool_bundles": {}}
    for label, command in sorted((tools or {}).items()):
        if not label:
            raise ValueError("tool label must not be empty")
        found = shutil.which(os.fspath(command))
        if not found:
            raise ValueError(f"tool not found or not executable: {label}={command}")
        path = _public_path(found).resolve()
        result["tools"][label] = {"path": str(path), "sha256": sha(path)}
    for directory in sorted({_public_path(path).resolve() for path in bundles}):
        # Do not turn an explicit bundle request into a whole-repository scan.
        if ROOT == directory or directory in ROOT.parents:
            raise ValueError(f"bundle must not contain the repository: {directory}")
        if not directory.is_dir():
            raise ValueError(f"tool bundle missing: {directory}")
        files = sorted(_walk(directory))
        if not files:
            raise ValueError(f"tool bundle is empty: {directory}")
        hashes = {}
        for path in files:
            if not path.resolve().is_relative_to(directory):
                raise ValueError(f"bundle symlink escapes bundle: {path}")
            if not path.is_file():
                raise ValueError(f"bundle entry is not a regular file: {path}")
            hashes[path.relative_to(directory).as_posix()] = sha(path)
        result["tool_bundles"][str(directory)] = hashes
    return result


def _protect_output(output, root, sources, identities, bundles, profile="physical-root"):
    output = _public_path(output).resolve()
    inputs = [root / name for name in sources]
    inputs += [Path(item["path"]) for item in identities["tools"].values()]
    inputs += [Path(bundle) / name
               for bundle, files in identities["tool_bundles"].items()
               for name in files]
    for bundle in bundles:
        directory = Path(bundle).resolve()
        if output.is_relative_to(directory):
            raise ValueError("output must be outside requested tool bundles")
    for path in inputs:
        if output == path.resolve() or (output.exists() and output.samefile(path)):
            raise ValueError(f"output would overwrite fingerprinted input: {path}")
    relative = output.relative_to(root) if output.is_relative_to(root) else None
    if relative and _is_source_path(relative, profile):
        raise ValueError("output would become an allowlisted source input")
    return output


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT,
                        help="repository root (default: relative to this script)")
    parser.add_argument("--profile", choices=("physical-root", "sequence"),
                        default="physical-root", help="public source scope (default: physical-root)")
    parser.add_argument("--output", type=Path,
                        help="write deterministic JSON here (default: stdout)")
    parser.add_argument("--tool", action="append", default=[], metavar="NAME=PATH_OR_COMMAND",
                        help="explicit executable identity; repeatable, never executed")
    parser.add_argument("--tool-bundle", action="append", type=Path, default=[], metavar="PATH",
                        help="hash every file in an explicit public tool bundle; repeatable")
    args = parser.parse_args(argv)
    try:
        tools = {}
        for spec in args.tool:
            name, separator, command = spec.partition("=")
            if not separator or not name or not command or name in tools:
                raise ValueError("--tool needs a unique nonempty NAME=PATH_OR_COMMAND")
            tools[name] = command
        root = _public_path(args.root).resolve()
        sources = source_fingerprints(root, profile=args.profile)
        identities = tool_fingerprints(tools, args.tool_bundle)
        manifest = {"source_fingerprints": sources}
        if tools or args.tool_bundle:
            manifest.update(identities)
        text = json.dumps(manifest, indent=2, sort_keys=True) + "\n"
        if args.output:
            output = _protect_output(args.output, root, sources, identities,
                                     args.tool_bundle, profile=args.profile)
            output.write_text(text, encoding="utf-8")
        else:
            sys.stdout.write(text)
    except (OSError, ValueError) as error:
        print(f"Fingerprint generation failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
