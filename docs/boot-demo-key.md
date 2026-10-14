# Boot demo signing key (DEMO ONLY)

This key is an authorized local demonstration key, **not a production trust
root**. Only its public key is in this repository. No hardware was contacted;
this does not implement or validate a bootloader, provisioning, or target fit.
The existing design contract specifies ECDSA P-256 and SEC1 uncompressed public
keys: `0x04 || x || y`, with each coordinate 32 bytes unsigned big-endian.

## Local identity and custody

- Private key: `/Users/nhan/.local/share/lm3s6965-boot-demo/LM3S6965-IQC50-A2-arm-cortex-m3/signing-key.pem`
- Private directory mode: `0700`; private key mode: `0600`.
- Public include: `config/boot_demo_public_key.h`, symbol
  `lm3s6965_boot_demo_public_key` (65 bytes).
- Public fingerprint: **SHA-256 of the exact 65 SEC1 bytes**, not PEM or SPKI DER:
  `f3ab1a07197e14750365367b1c09a2b0848c7062a419f82d7f1b087d03e7518d`.

The PEM is an unencrypted OpenSSL private key, protected by local filesystem
permissions; filesystem permissions do not protect against the account owner,
administrators, malware, or insecure backups. Never copy it into the repository,
vault, firmware, logs, or chat. Preserve it in appropriately protected local
storage if demo artifacts must remain verifiable. Key generation used exclusive
file creation and did not overwrite an existing key. Losing or rotating it
requires updating the public include/fingerprint and re-signing demo artifacts;
old artifacts do not verify under a new key.

Tools verified locally: OpenSSL **3.6.3** (`/opt/homebrew/bin/openssl`) and Python 3
standard library. No dependencies installed. `.gitignore` already excludes
`*.pem`, `*.key`, and common secret formats; the private path is outside the Git
worktree and vault altogether. Ignore rules are defense in depth, not a security
boundary, and do not untrack existing files.

## Verify the current key and include

Run from the repository root; this prints validity and the **public** fingerprint,
never private values. Do not add `-text` to a private-key inspection command.

```sh
openssl pkey -in "$HOME/.local/share/lm3s6965-boot-demo/LM3S6965-IQC50-A2-arm-cortex-m3/signing-key.pem" -check -noout
python3 - <<'PY'
import hashlib, pathlib, re, stat, subprocess
repo = pathlib.Path.cwd().resolve()
key = pathlib.Path.home() / '.local/share/lm3s6965-boot-demo' / repo.name / 'signing-key.pem'
assert not key.resolve().is_relative_to(repo)
assert stat.S_IMODE(key.parent.stat().st_mode) == 0o700
assert stat.S_IMODE(key.stat().st_mode) == 0o600
der = subprocess.check_output(['openssl', 'pkey', '-in', str(key),
    '-pubout', '-outform', 'DER', '-ec_conv_form', 'uncompressed'])
# Exact SPKI prefix asserts id-ecPublicKey and prime256v1 (P-256) OIDs.
prefix = bytes.fromhex('3059301306072a8648ce3d020106082a8648ce3d030107034200')
assert len(der) == 91 and der[:26] == prefix
public = der[26:]
assert len(public) == 65 and public[0] == 4
header = (repo / 'config/boot_demo_public_key.h').read_text()
initializer = header.split('= {', 1)[1].split('};', 1)[0]
embedded = bytes(int(x, 16) for x in re.findall(r'0x([0-9a-fA-F]{2})', initializer))
assert embedded == public, 'Header differs from private-derived public key'
fingerprint = hashlib.sha256(public).hexdigest()
assert fingerprint == 'f3ab1a07197e14750365367b1c09a2b0848c7062a419f82d7f1b087d03e7518d'
print('P-256, SEC1/header derivation and permissions verified; SHA-256:', fingerprint)
PY
```

Generation validation also ran `openssl pkey -pubin -pubcheck -noout` on the
exported public key and an ECDSA SHA-256 digest signing/verification round trip.
No private text dump was used. Git checks: `git ls-files '*.pem' '*.key'` must be
empty; `git check-ignore --no-index demo-signing-key.pem demo-signing-key.key`
confirms the existing secret patterns. Outside-worktree files are not trackable
by this repository.

## Regeneration (explicit rotation only)

Do not run this for the existing key. The exclusive open below refuses to
replace any file or symlink at the key path. For intentional rotation, choose a
new external directory first; never delete/overwrite the current key casually.
Afterward derive the uncompressed public key with the verification procedure,
update the header/fingerprint deliberately, and re-sign the demo artifacts.

```sh
python3 - <<'PY'
import os, pathlib, subprocess
repo = pathlib.Path.cwd().resolve()
directory = pathlib.Path.home() / '.local/share/lm3s6965-boot-demo' / repo.name
assert not directory.resolve().is_relative_to(repo)
assert not directory.is_symlink()
directory.mkdir(parents=True, exist_ok=True, mode=0o700)
os.chmod(directory, 0o700)
key = directory / 'signing-key.pem'
fd = os.open(key, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
with os.fdopen(fd, 'wb') as output:
    subprocess.run(['openssl', 'genpkey', '-algorithm', 'EC',
        '-pkeyopt', 'ec_paramgen_curve:prime256v1'], stdout=output, check=True)
subprocess.run(['openssl', 'pkey', '-in', str(key), '-check', '-noout'], check=True)
PY
```

## Signing usage (host-only example, not a package generator)

Prepare `manifest-prefix.bin` as exactly the canonical 96-byte `manifest[0,96)`
with validated fields and the payload SHA-256 already at offset 48. Hash those
96 bytes **once**, then use the digest-input OpenSSL API without `-rawin` or
`-digest` (those options select message hashing). OpenSSL emits DER ECDSA, which
is **not** the wire format: convert to fixed 64-byte `r || s` before placing at
manifest offsets `[96,160)`. No private bytes are included in either signature.
Use a scratch directory outside the repository for these inputs/outputs:

```sh
KEY="$HOME/.local/share/lm3s6965-boot-demo/LM3S6965-IQC50-A2-arm-cortex-m3/signing-key.pem"
# Execute sequentially; stop on any failed command.
python3 - <<'PY'
import hashlib, pathlib
prefix = pathlib.Path('manifest-prefix.bin').read_bytes()
assert len(prefix) == 96
pathlib.Path('manifest.sha256').write_bytes(hashlib.sha256(prefix).digest())
PY
openssl pkeyutl -sign -inkey "$KEY" -pkeyopt digest:sha256 -in manifest.sha256 -out signature.der
openssl pkey -in "$KEY" -pubout -ec_conv_form uncompressed -out demo-public.pem
openssl pkey -pubin -in demo-public.pem -pubcheck -noout
openssl pkeyutl -verify -pubin -inkey demo-public.pem -pkeyopt digest:sha256 -in manifest.sha256 -sigfile signature.der
python3 - <<'PY'
import pathlib
# Strict decoder for OpenSSL's short P-256 DER signature, not ECC arithmetic.
der = pathlib.Path('signature.der').read_bytes()
assert 8 <= len(der) <= 72 and der[0] == 0x30 and der[1] == len(der) - 2
n = int('ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551', 16)
position, scalars = 2, []
for _ in range(2):
    assert position + 2 <= len(der) and der[position] == 0x02
    size = der[position + 1]
    position += 2
    encoded = der[position:position + size]
    assert 1 <= size <= 33 and len(encoded) == size and not (encoded[0] & 0x80)
    assert size == 1 or encoded[0] != 0 or (encoded[1] & 0x80)
    value = int.from_bytes(encoded, 'big')
    assert 1 <= value < n
    scalars.append(value.to_bytes(32, 'big'))
    position += size
assert position == len(der)
pathlib.Path('signature.raw').write_bytes(b''.join(scalars))
assert pathlib.Path('signature.raw').stat().st_size == 64
PY
```

This example validates the crypto operation and encoding only; it does not
validate domain/product/slot/base, payload bounds/hash/vectors, build a complete
FTP package, or authorize flashing. Future C verification must validate the
P-256 public point and scalar ranges and verify the same 32-byte digest using
the approved raw digest/MPI verifier contract. No C verifier integration or
production key provisioning is performed here.
