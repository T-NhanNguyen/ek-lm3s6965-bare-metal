"""Restricted-bench update workflow. Transport is injected; no sockets here."""
from dataclasses import dataclass
import hashlib
import re
import struct
import time

KEYS = ("format sequence running_slot hash release confirmed_slot candidate_slot "
        "permitted_slot trial_id trial_state slot_a_failures slot_a_reasons "
        "slot_a_rollback slot_b_failures slot_b_reasons slot_b_rollback health "
        "stage fault detail reset_flags").split()
REASONS = {"watchdog", "external", "brownout", "network_deadline", "combined"}


def status(body):
    """Reject malformed status. Keep known zero distinct from unavailable."""
    if not isinstance(body, bytes) or len(body) > 1024 or not body.endswith(b"\n"):
        raise ValueError("status framing")
    text = body.decode("ascii")
    lines = [line.split("=") for line in text[:-1].split("\n")]
    if any(len(line) != 2 for line in lines) or [line[0] for line in lines] != KEYS:
        raise ValueError("status keys")
    values = dict(lines)
    if values["format"] != "1":
        raise ValueError("status format")
    for key in KEYS[1:]:
        value = values[key]
        if value == "none":
            continue
        if key.endswith("slot"):
            valid = value in ("A", "B")
        elif key == "hash":
            valid = re.fullmatch("[0-9a-f]{64}", value)
        elif key in ("health", "reset_flags"):
            valid = re.fullmatch("[0-9a-f]{8}", value)
            if valid and key == "health":
                valid = int(value, 16) <= 7
        elif key == "trial_state":
            valid = value in ("ready", "trial", "rejected")
        elif key.endswith("reasons"):
            parts = value.split(",")
            valid = len(parts) <= 3 and all(part in REASONS for part in parts)
        else:
            limit = 3 if key.endswith("failures") else 1 if key.endswith("rollback") else 5 if key in ("stage", "fault") else 20 if key == "detail" else 0xffffffff
            valid = re.fullmatch("[0-9]+", value) and len(value) <= 1024 and int(value) <= limit
        if not valid:
            raise ValueError("status value: " + key)
    return values


@dataclass(frozen=True)
class Artifact:
    slot: str
    digest: str
    release: str
    body: bytes

    @classmethod
    def from_bytes(cls, body):
        """Check package shape and identity, not ECDSA authenticity.

        The device must authenticate with its real verifier before mutation.
        """
        if len(body) < 160 or body[:16] != b"LM3S6965-FW-V1\0\0":
            raise ValueError("manifest domain")
        version, size, product, slot, base, length, release, flags = struct.unpack_from("<8I", body, 16)
        if (version, size, product, flags) != (1, 160, 1, 0) or slot not in (0, 1):
            raise ValueError("manifest header")
        if base != (0x8800, 0x24000)[slot] or not 236 <= length <= 111616 or length % 4:
            raise ValueError("manifest bounds")
        if len(body) != 160 + length or body[80:96] != bytes(16):
            raise ValueError("package framing")
        if hashlib.sha256(body[160:]).digest() != body[48:80]:
            raise ValueError("package hash")
        msp, reset = struct.unpack_from("<II", body, 160)
        if msp != 0x20010000 or not reset & 1 or not base <= (reset & ~1) < base + length:
            raise ValueError("package vectors")
        return cls("AB"[slot], body[48:80].hex(), str(release), bytes(body))


class Workflow:
    """One upload attempt, then status resolution. Never upload on retry.

    Transport provides connect(), retr(name), stor(name, bytes), and binary().
    Each connection must select TYPE I through binary(). Operations raise
    OSError on silence, disconnect, or uncertain completion. Poll is bounded.
    Explicit confirm() is the only confirmation action. No self-confirmation.
    """
    def __init__(self, transport, sleep=time.sleep):
        self.transport = transport
        self.sleep = sleep
        self.artifact = None
        self.trial_id = None
        self.upload_attempted = False
        self.before_upload = None

    def read(self):
        self.transport.connect()
        self.transport.binary()
        return status(self.transport.retr("boot-status"))

    def upload(self, artifacts):
        if self.upload_attempted:
            raise ValueError("upload already attempted; resolve by status")
        current = self.read()
        destination = current["permitted_slot"]
        if (destination not in ("A", "B") or current["candidate_slot"] != "none" or
                current["running_slot"] not in ("A", "B") or
                current["running_slot"] != current["confirmed_slot"] or
                current["running_slot"] == destination):
            raise ValueError("upload not admitted")
        artifact = Artifact.from_bytes(artifacts[destination])
        if artifact.slot != destination:
            raise ValueError("artifact target")
        self.artifact = artifact
        self.before_upload = current
        self.upload_attempted = True  # A lost reply does not permit another upload.
        try:
            self.transport.stor("firmware", artifact.body)
        except OSError:
            pass
        return "UNKNOWN"  # READY, execution, and confirmation still need status.

    def classify(self, observed):
        a = self.artifact
        if a is None:
            raise ValueError("no selected artifact")
        if any(observed[key] == "none" for key in ("running_slot", "hash", "release")):
            return "UNKNOWN"  # Unavailable identity cannot prove a different image.
        release = observed["release"]
        if (observed["running_slot"] != a.slot or observed["hash"] != a.digest or
                release == "none" or int(release) != int(a.release)):
            before = self.before_upload
            if (self.trial_id is None and before is not None and
                    all(observed[key] == before[key] for key in
                        ("running_slot", "hash", "confirmed_slot")) and
                    before["release"] != "none" and int(release) == int(before["release"]) and
                    ((observed["candidate_slot"] == "none" and observed["trial_state"] == "none" and
                      observed["permitted_slot"] == a.slot) or
                     (observed["candidate_slot"] == a.slot and observed["trial_state"] == "ready" and
                      observed["permitted_slot"] == "none"))):
                # Invalidation and durable READY change metadata before reset.
                return "UNKNOWN"
            return "MISMATCH"
        trial = observed["trial_id"]
        if trial == "none":
            return "UNKNOWN"
        if self.trial_id is not None and int(trial) != self.trial_id:
            return "MISMATCH"
        before = self.before_upload
        if (before is None or before["sequence"] == "none" or
                observed["sequence"] == "none" or
                not int(before["sequence"]) < int(trial) <= int(observed["sequence"])):
            # Acceptance uses its metadata sequence. Missing or stale authority
            # cannot bind this upload, confirm it, or prove confirmed success.
            return "UNKNOWN"
        if observed["confirmed_slot"] == a.slot and observed["candidate_slot"] == "none" and observed["trial_state"] == "none":
            self.trial_id = int(trial)
            return "CONFIRMED"
        if (observed["candidate_slot"] != a.slot or observed["trial_state"] != "trial" or
                observed["permitted_slot"] != "none"):
            return "UNKNOWN"
        self.trial_id = int(trial)
        return "TRIAL" if observed["health"] == "00000007" else "UNHEALTHY"

    def poll(self, attempts):
        if attempts < 1:
            raise ValueError("positive poll bound required")
        for index in range(attempts):
            try:
                result = self.classify(self.read())
            except (OSError, ValueError, UnicodeError):
                result = "UNKNOWN"
            if result in ("TRIAL", "CONFIRMED", "MISMATCH"):
                return result
            if index + 1 < attempts:
                self.sleep(1)
        return result

    def confirm(self):
        try:
            observed = self.read()
        except (OSError, ValueError, UnicodeError):
            return "UNKNOWN"
        state = self.classify(observed)
        if state == "CONFIRMED":
            return state
        if state != "TRIAL":
            raise ValueError("healthy matching trial required")
        a = self.artifact
        body = (f"format=1\nslot={a.slot}\nhash={a.digest}\ntrial_id={self.trial_id}\n").encode("ascii")
        try:
            self.transport.stor("confirm", body)
        except OSError:
            pass
        # A caller may invoke confirm again. The exact retained trial ID stays fixed.
        return "UNKNOWN"


class ConfirmedIdentity(Workflow):
    """Read-only retry/resolution of an explicitly supplied confirmed identity.

    This is NOT upload acceptance or authorization of an unconfirmed trial.
    A retained ID of zero is valid (notably the factory image). Both poll()
    and explicit confirm() may resolve durable success without renewed health.
    No firmware or confirmation STOR is issued, even after a lost read reply.
    """
    def __init__(self, transport, artifact, trial_id, sleep=time.sleep):
        if not isinstance(artifact, Artifact):
            raise ValueError("selected artifact required")
        if type(trial_id) is not int or not 0 <= trial_id <= 0xffffffff:
            raise ValueError("exact uint32 retained trial ID required")
        super().__init__(transport, sleep)
        self.artifact = Artifact.from_bytes(artifact.body)
        if self.artifact != artifact:
            raise ValueError("artifact identity")
        self.trial_id = trial_id

    def upload(self, artifacts):
        raise ValueError("confirmed identity resolution cannot upload")

    def classify(self, observed):
        a = self.artifact
        if any(observed[key] == "none" for key in
               ("running_slot", "hash", "release", "trial_id", "sequence")):
            return "UNKNOWN"
        if (observed["running_slot"] != a.slot or observed["hash"] != a.digest or
                int(observed["release"]) != int(a.release) or
                int(observed["trial_id"]) != self.trial_id):
            return "MISMATCH"
        if int(observed["sequence"]) < self.trial_id:
            return "UNKNOWN"
        if (observed["confirmed_slot"] == a.slot and
                observed["candidate_slot"] == "none" and observed["trial_state"] == "none"):
            return "CONFIRMED"
        # Never let the inherited confirm() send a new trial authorization.
        return "UNKNOWN"
