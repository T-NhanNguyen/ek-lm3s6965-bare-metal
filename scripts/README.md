# Public boot source fingerprints

Generate a public source-only SHA256 manifest with Python 3.9+ (standard library
only; no ARM compiler, build, Git, network or private design documents needed):

```sh
python3 scripts/boot_fingerprints.py --output /tmp/boot-fingerprints.json
```

From another working directory, use the script's absolute path; the default
repository root is anchored to the script, not the working directory. `--root
/path/to/checkout` selects another checkout. Omit `--output` for JSON on stdout.
Explicit relative paths supplied as arguments are relative to the working
directory. Output parent directories must already exist.

The default `--profile physical-root` preserves the physical-root runner's public
selection. Select the sequence runner's public scope explicitly:

```sh
python3 scripts/boot_fingerprints.py --profile sequence \
  --output /tmp/sequence-fingerprints.json
```

Both profiles are source-only by default and need no ARM compiler or other tool
installation. Their source scopes are:

- `physical-root` (default): recursively select `.c`, `.h`, `.S`, `.cmake`, `.txt`
  and `.ld` files under `src`, `include/lm3s6965`, `config`, `cmake`,
  `third_party/mbedtls/include`, `third_party/mbedtls/library`,
  `tests/boot/physical_root`, `freertos` and `examples`. Retain the existing
  explicit required files, remove the two private design inputs, and add the
  generator itself; the exact required list is `REQUIRED_FILES` in the script.
- `sequence`: recursively select only `.c` and `.h` files under
  `include/lm3s6965`, `config`, `third_party/mbedtls/include` and
  `third_party/mbedtls/library`; select only direct `src/boot_*.c` and
  `src/boot_*.h` matches. Explicitly require `src/CMakeLists.txt`,
  `tests/boot/CMakeLists.txt`, `cmake/boot_crypto.cmake`,
  `tests/boot/sequence_host.c`, `tests/boot/run_sequence.py`,
  `tests/boot/sequence_manifest.json`, `tests/boot/flash_model.c`,
  `tests/boot/flash_model.h`, `tests/boot/verify_fixtures.h`,
  `docs/boot-startup.md` and `scripts/boot_fingerprints.py`. Relative to the
  existing sequence identity, only its private shared-design input is removed
  and the generator is added; no physical-root-only scope is inherited.

The `source_fingerprints` object maps sorted repository-relative POSIX paths to
SHA256 hex digests; the profile does not add a JSON field or change the schema.
Explicit required files must exist; absent
glob directories retain the runner's empty-match behavior. No whole-repository
scan is performed. Source symlinks and known private paths are rejected rather
than followed. Generated JSON outside explicit required inputs is not selected.
Do not publish private designs or key material alongside the manifest.

## Optional explicit tools

```sh
python3 scripts/boot_fingerprints.py \
  --tool cc=arm-none-eabi-gcc --tool cmake=cmake \
  --tool-bundle /opt/arm-gnu-toolchain \
  --output /tmp/boot-fingerprints-with-tools.json
```

Tool scope is independent of `--profile`: neither profile discovers or hashes
any tools unless `--tool` or `--tool-bundle` is explicitly supplied.

`--tool NAME=PATH_OR_COMMAND` is repeatable with unique labels. Commands are
resolved through PATH, never executed. The `tools` object records each label's
resolved absolute `path` and `sha256`. There is no default compiler location or
version. A missing or nonexecutable requested tool is an error.

`--tool-bundle PATH` is repeatable and hashes **every file**, not just compiler
executables. `tool_bundles` maps resolved absolute bundle paths to relative file
hashes. Supply only public tool installations, never a home directory, secret
store or repository. Repository-containing bundles, missing/empty bundles,
directory symlinks, escaping file symlinks and known private paths fail closed.
In-bundle file symlinks hash their resolved target bytes. No requested input is
silently omitted. Output cannot overwrite any hashed input (including hardlink
aliases), become an allowlisted source, or reside inside a requested bundle.
Tool paths in optional manifests disclose local installation locations; review
those paths before publication.

## Import API and runner integration

Import without side effects (no manifest generation or tool discovery at import):

```python
from scripts.boot_fingerprints import source_fingerprints, tool_fingerprints

sources = source_fingerprints(root)  # root: str or pathlib.Path; physical-root
sequence_sources = source_fingerprints(root, profile="sequence")
# Optional, explicitly selected; no implicit environment/tool coverage:
identities = tool_fingerprints(
    {"cc": "/opt/arm/bin/arm-none-eabi-gcc", "cmake": "cmake"},
    bundles=["/opt/arm"],
)
```

`source_fingerprints(root, profile="physical-root")` returns the source mapping
directly, not the CLI's outer JSON object. The optional profile accepts only
`"physical-root"` and `"sequence"`; unknown values raise `ValueError`. Existing
one-argument calls retain the physical-root scope and output shape.
`tool_fingerprints(tools=None, bundles=())` always returns
`{"tools": {...}, "tool_bundles": {...}}`. `sha(path)` is also available for
streaming file hashes. APIs raise `OSError`/`ValueError` on invalid or unreadable
inputs; the CLI reports errors on stderr and exits 1. Successful generation
exits 0. JSON is sorted, indented and newline-terminated, with no timestamp.

Runner integration is separate from this generator: use
`source_fingerprints(ROOT)` for physical-root and
`source_fingerprints(ROOT, profile="sequence")` for sequence. Runner before/after
calls and baseline equality checks remain mandatory. The physical-root
selection delta is removal of `.dev-vault/design/physical-boot-io.md` and
`.dev-vault/design/shared-real-boot-sequence.md`, plus addition of
`scripts/boot_fingerprints.py`; all other required paths, directory globs and
suffixes are retained. Existing pre-migration manifests are not equivalent:
generate a fresh bring-up baseline before continuing verification.

**Evidence limit:** a source manifest identifies selected bytes, not successful
tests, runtime behavior, hardware acceptance, or a full environment proof.
Optional tool hashes prove only requested file identities. They do not assert
compiler versions, resolve runtime dependencies, or establish tool trust. Keep
the existing runner's official-bundle availability/version enforcement, complete
transitive bundle hashing, required host tools and before/after continuity
checks in the integrated runner; an optional CLI scope must not replace those
mandatory checks. This utility provides generation, not a `--check` mode;
compare mappings using the owner's existing baseline logic.
