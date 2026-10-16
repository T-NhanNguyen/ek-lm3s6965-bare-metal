"""Scripted HOST transport checks. No listener or network connection."""
import hashlib
from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools" / "boot"))
import workflow as w


def artifact(slot):
    base = {"A": 0x8800, "B": 0x24000}[slot]
    payload = struct.pack("<II", 0x20010000, base | 1) + bytes(228)
    return (b"LM3S6965-FW-V1\0\0" + struct.pack("<8I", 1, 160, 1, "AB".index(slot), base, 236, 42, 0)
            + hashlib.sha256(payload).digest() + bytes(16) + bytes(64) + payload)


def status(**changes):
    values = dict.fromkeys(w.KEYS, "none")
    values.update(format="1", sequence="20", running_slot="A", hash="0"*64,
                  release="1", confirmed_slot="A", permitted_slot="B", trial_id="0",
                  health="00000007", stage="4", fault="0", detail="0", reset_flags="00000000")
    values.update(changes)
    return "".join(f"{key}={values[key]}\n" for key in w.KEYS).encode()


def trial(**changes):
    a = w.Artifact.from_bytes(artifact("B"))
    values = dict(sequence="22", running_slot="B", hash=a.digest, release="42", candidate_slot="B",
                  permitted_slot="none", trial_id="21", trial_state="trial")
    values.update(changes)
    return status(**values)


class Script:
    def __init__(self, steps):
        self.steps = list(steps)
        self.calls = []

    def call(self, *args):
        self.calls.append(args)
        expected, result = self.steps.pop(0)
        if expected != args:
            raise AssertionError((expected, args))
        if isinstance(result, Exception):
            raise result
        return result

    def connect(self):
        return self.call("connect")

    def binary(self):
        return self.call("TYPE I")

    def retr(self, name):
        return self.call("RETR", name)

    def stor(self, name, body):
        return self.call("STOR", name, body)


def read(body):
    return [(("connect",), None), (("TYPE I",), None), (("RETR", "boot-status"), body)]


class WorkflowTests(unittest.TestCase):
    def start(self, tail, reply=None, baseline=None):
        s = Script(read(status() if baseline is None else baseline)
                   + [(("STOR", "firmware", artifact("B")), reply)] + tail)
        sleeps = []
        f = w.Workflow(s, sleeps.append)
        self.assertEqual(f.upload({"A": artifact("A"), "B": artifact("B")}), "UNKNOWN")
        return f, s, sleeps

    def test_slot_selection_and_lost_upload_reply(self):
        f, s, sleeps = self.start(read(status()) + read(trial()), OSError("lost reply"))
        self.assertEqual(f.poll(2), "TRIAL")
        self.assertEqual(s.steps, [])
        self.assertEqual(sleeps, [1])
        with self.assertRaises(ValueError):
            f.upload({"B": artifact("B")})
        self.assertEqual(sum(c[:2] == ("STOR", "firmware") for c in s.calls), 1)

    def test_old_running_invalidation_ready_then_trial(self):
        pending = [status(sequence="21"),
                   status(sequence="22", candidate_slot="B", permitted_slot="none",
                          trial_state="ready", trial_id="22")]
        f, s, sleeps = self.start(sum((read(body) for body in pending), [])
                                  + read(trial(sequence="23", trial_id="22")),
                                  OSError("lost firmware reply"))
        self.assertEqual(f.poll(3), "TRIAL")
        self.assertEqual(s.steps, [])
        self.assertEqual(sleeps, [1, 1])
        with self.assertRaises(ValueError):
            f.upload({"B": artifact("B")})
        self.assertEqual(sum(c[:2] == ("STOR", "firmware") for c in s.calls), 1)
        self.assertFalse(any(c[:2] == ("STOR", "confirm") for c in s.calls))

    def test_pending_poll_exhaustion_remains_unknown(self):
        for body in (status(sequence="21"),
                     status(sequence="22", candidate_slot="B", permitted_slot="none",
                            trial_state="ready", trial_id="22")):
            with self.subTest(body=body):
                f, s, sleeps = self.start(read(body) * 3, OSError("lost reply"))
                self.assertEqual(f.poll(3), "UNKNOWN")
                self.assertEqual(s.steps, [])
                self.assertEqual(sleeps, [1, 1])
                self.assertIsNone(f.trial_id)
                self.assertEqual(sum(c[:2] == ("STOR", "firmware") for c in s.calls), 1)
                self.assertFalse(any(c[:2] == ("STOR", "confirm") for c in s.calls))

    def test_missing_running_identity_is_unknown_and_blocks_confirmation(self):
        for key in ("running_slot", "hash", "release"):
            with self.subTest(key=key):
                missing = trial(**{key: "none"})
                f, s, sleeps = self.start(read(missing) + read(missing) + read(trial()))
                with self.assertRaises(ValueError):
                    f.confirm()
                self.assertIsNone(f.trial_id)
                self.assertEqual(f.poll(2), "TRIAL")
                self.assertEqual(s.steps, [])
                self.assertEqual(sleeps, [1])
                self.assertFalse(any(c[:2] == ("STOR", "confirm") for c in s.calls))

    def test_known_wrong_identity_after_transition_stops_polling(self):
        for changed in ({"hash": "1"*64}, {"release": "43"}, {"running_slot": "A"}):
            with self.subTest(changed=changed):
                f, s, sleeps = self.start(read(trial()) + read(trial(**changed)))
                self.assertEqual(f.poll(1), "TRIAL")
                self.assertEqual(f.poll(3), "MISMATCH")
                self.assertEqual(s.steps, [])
                self.assertEqual(sleeps, [])
                self.assertFalse(any(c[:2] == ("STOR", "confirm") for c in s.calls))
        # Even the complete old identity must not become pending after a trial was pinned.
        f, s, _ = self.start(read(trial()) + read(status()))
        self.assertEqual(f.poll(1), "TRIAL")
        self.assertEqual(f.poll(1), "MISMATCH")
        self.assertEqual(s.steps, [])

    def test_stale_or_unavailable_authority_cannot_bind_or_confirm(self):
        cases = [dict(sequence="19", trial_id="18"),  # Review reproduction.
                 dict(sequence="20", trial_id="20"),  # Equal is not fresh.
                 dict(sequence="22", trial_id="20"),  # New status, old acceptance.
                 dict(sequence="22", trial_id="19"),
                 dict(sequence="20", trial_id="21"),  # Acceptance ahead of authority.
                 dict(sequence="none"), dict(trial_id="none"),
                 dict(trial_id="0"), dict(trial_id="0000")]
        for changes in cases:
            for confirmed in (False, True):
                with self.subTest(changes=changes, confirmed=confirmed):
                    roles = (dict(confirmed_slot="B", candidate_slot="none", trial_state="none")
                             if confirmed else {})
                    body = trial(**roles, **changes)
                    f, s, _ = self.start(read(body) + read(body))
                    self.assertEqual(f.poll(1), "UNKNOWN")
                    self.assertIsNone(f.trial_id)
                    with self.assertRaises(ValueError):
                        f.confirm()
                    self.assertIsNone(f.trial_id)
                    self.assertEqual(s.steps, [])
                    self.assertFalse(any(c[:2] == ("STOR", "confirm") for c in s.calls))

    def test_fresh_upload_zero_is_known_but_unresolved(self):
        for spelling in ("0", "0000"):
            for confirmed in (False, True):
                with self.subTest(spelling=spelling, confirmed=confirmed):
                    roles = (dict(confirmed_slot="B", candidate_slot="none", trial_state="none")
                             if confirmed else {})
                    body = trial(trial_id=spelling, **roles)
                    self.assertEqual(int(w.status(body)["trial_id"]), 0)
                    f, s, _ = self.start(read(body) + read(body))
                    self.assertEqual(f.poll(1), "UNKNOWN")
                    with self.assertRaises(ValueError):
                        f.confirm()
                    self.assertIsNone(f.trial_id)
                    self.assertEqual(s.steps, [])
                    self.assertEqual(sum(c[:2] == ("STOR", "firmware") for c in s.calls), 1)
                    self.assertFalse(any(c[:2] == ("STOR", "confirm") for c in s.calls))

    def test_existing_confirmed_zero_resolution_and_retry_without_health(self):
        a = w.Artifact.from_bytes(artifact("B"))
        for spelling in ("0", "0000"):
            with self.subTest(spelling=spelling):
                confirmed = trial(trial_id=spelling, confirmed_slot="B", candidate_slot="none",
                                  trial_state="none", health="none")
                s = Script(read(OSError("lost status reply")) + read(confirmed) + read(confirmed))
                sleeps = []
                f = w.ConfirmedIdentity(s, a, 0, sleeps.append)
                self.assertEqual(f.poll(2), "CONFIRMED")
                self.assertEqual(sleeps, [1])
                self.assertEqual(f.trial_id, 0)
                self.assertEqual(f.confirm(), "CONFIRMED")
                with self.assertRaises(ValueError):
                    f.upload({"B": artifact("B")})
                self.assertEqual(s.steps, [])
                self.assertFalse(any(c[0] == "STOR" for c in s.calls))

    def test_existing_confirmed_zero_requires_exact_complete_identity(self):
        a = w.Artifact.from_bytes(artifact("B"))
        confirmed = dict(trial_id="0", confirmed_slot="B", candidate_slot="none",
                         trial_state="none", health="00000000")
        cases = [(dict(hash="1"*64), "MISMATCH"), (dict(running_slot="A"), "MISMATCH"),
                 (dict(release="43"), "MISMATCH"), (dict(trial_id="1"), "MISMATCH")]
        cases += [({key: "none"}, "UNKNOWN") for key in
                  ("running_slot", "hash", "release", "trial_id", "sequence", "confirmed_slot")]
        cases += [(dict(candidate_slot="B", trial_state="trial"), "UNKNOWN")]
        for changes, expected in cases:
            with self.subTest(changes=changes):
                values = dict(confirmed, **changes)
                body = trial(**values)
                s = Script(read(body) + read(body))
                f = w.ConfirmedIdentity(s, a, 0)
                self.assertEqual(f.poll(1), expected)
                with self.assertRaises(ValueError):
                    f.confirm()
                self.assertEqual(f.trial_id, 0)
                self.assertEqual(s.steps, [])
                self.assertFalse(any(c[0] == "STOR" for c in s.calls))
        for invalid in (None, "0", -1, 0x100000000, True):
            with self.subTest(invalid=invalid):
                with self.assertRaises(ValueError):
                    w.ConfirmedIdentity(Script([]), a, invalid)

    def test_missing_baseline_authority_cannot_prove_success(self):
        for confirmed in (False, True):
            with self.subTest(confirmed=confirmed):
                body = trial(**(dict(confirmed_slot="B", candidate_slot="none", trial_state="none")
                                if confirmed else {}))
                f, s, _ = self.start(read(body) + read(body), baseline=status(sequence="none"))
                self.assertEqual(f.poll(1), "UNKNOWN")
                with self.assertRaises(ValueError):
                    f.confirm()
                self.assertIsNone(f.trial_id)
                self.assertEqual(s.steps, [])
                self.assertFalse(any(c[:2] == ("STOR", "confirm") for c in s.calls))

    def test_fresh_authority_binds_after_replayed_trial(self):
        a = w.Artifact.from_bytes(artifact("B"))
        body = f"format=1\nslot=B\nhash={a.digest}\ntrial_id=21\n".encode()
        tail = read(trial(sequence="19", trial_id="18"))
        tail += read(trial(sequence="21", trial_id="00021"))
        tail += read(trial(sequence="22")) + [(("STOR", "confirm", body), None)]
        f, s, sleeps = self.start(tail, OSError("lost upload reply"))
        self.assertEqual(f.poll(2), "TRIAL")
        self.assertEqual(f.trial_id, 21)
        self.assertEqual(sleeps, [1])
        self.assertEqual(f.confirm(), "UNKNOWN")
        self.assertEqual(s.steps, [])
        self.assertEqual(sum(c[:2] == ("STOR", "firmware") for c in s.calls), 1)

    def test_acceptance_sequence_boundaries_do_not_wrap(self):
        for baseline, acceptance in (("0", "1"), ("4294967294", "4294967295")):
            with self.subTest(baseline=baseline, acceptance=acceptance):
                f, s, _ = self.start(read(trial(sequence=acceptance, trial_id=acceptance)),
                                      baseline=status(sequence=baseline))
                self.assertEqual(f.poll(1), "TRIAL")
                self.assertEqual(f.trial_id, int(acceptance))
                self.assertEqual(s.steps, [])
        for acceptance in ("0", "4294967295"):
            with self.subTest(exhausted=acceptance):
                body = trial(sequence="4294967295", trial_id=acceptance)
                f, s, _ = self.start(read(body) + read(body),
                                      baseline=status(sequence="4294967295"))
                self.assertEqual(f.poll(1), "UNKNOWN")
                with self.assertRaises(ValueError):
                    f.confirm()
                self.assertIsNone(f.trial_id)
                self.assertEqual(s.steps, [])

    def test_confirmed_shortcut_requires_fresh_authority(self):
        confirmed = dict(confirmed_slot="B", candidate_slot="none", trial_state="none",
                         health="00000000")
        # Direct success needs fresh authority even without a prior pinned TRIAL.
        for resolve in ("poll", "confirm"):
            with self.subTest(resolve=resolve):
                f, s, _ = self.start(read(trial(sequence="23", **confirmed)))
                self.assertEqual(f.poll(1) if resolve == "poll" else f.confirm(), "CONFIRMED")
                self.assertEqual(f.trial_id, 21)
                self.assertEqual(s.steps, [])
                self.assertFalse(any(c[:2] == ("STOR", "confirm") for c in s.calls))
        # A pinned ID does not make missing or contradictory authority sufficient.
        for sequence in ("none", "19", "20"):
            with self.subTest(sequence=sequence):
                stale = trial(sequence=sequence, **confirmed)
                tail = read(trial()) + read(stale) + read(stale)
                tail += read(trial(sequence="23", **confirmed))
                f, s, _ = self.start(tail)
                self.assertEqual(f.poll(1), "TRIAL")
                self.assertEqual(f.poll(1), "UNKNOWN")
                with self.assertRaises(ValueError):
                    f.confirm()
                self.assertEqual(f.trial_id, 21)
                self.assertEqual(f.poll(1), "CONFIRMED")
                self.assertEqual(s.steps, [])
                self.assertFalse(any(c[:2] == ("STOR", "confirm") for c in s.calls))

    def test_silence_unknown_and_one_second_reconnect(self):
        f, s, sleeps = self.start([(("connect",), OSError("silent"))]*3)
        self.assertEqual(f.poll(3), "UNKNOWN")
        self.assertEqual(sleeps, [1, 1])
        self.assertEqual(s.steps, [])
        self.assertFalse(any(c[:2] == ("STOR", "confirm") for c in s.calls))

    def test_explicit_confirm_lost_reply_and_idempotent_retry(self):
        a = w.Artifact.from_bytes(artifact("B"))
        body = f"format=1\nslot=B\nhash={a.digest}\ntrial_id=21\n".encode()
        tail = read(trial()) + read(trial()) + [(("STOR", "confirm", body), OSError("lost"))]
        tail += read(trial()) + [(("STOR", "confirm", body), None)]
        tail += read(trial(confirmed_slot="B", candidate_slot="none", trial_state="none", health="00000000"))
        f, s, _ = self.start(tail)
        self.assertEqual(f.poll(1), "TRIAL")
        self.assertEqual(f.confirm(), "UNKNOWN")
        self.assertEqual(f.confirm(), "UNKNOWN")
        self.assertEqual(f.poll(1), "CONFIRMED")
        self.assertEqual(s.steps, [])

    def test_identity_and_trial_mismatch(self):
        for changed in ({"hash": "1"*64}, {"release": "43"}, {"running_slot": "A"}):
            with self.subTest(changed=changed):
                f, s, _ = self.start(read(trial(**changed)))
                self.assertEqual(f.poll(1), "MISMATCH")
                self.assertEqual(s.steps, [])
        f, s, _ = self.start(read(trial()) + read(trial(trial_id="22")))
        self.assertEqual(f.poll(1), "TRIAL")
        with self.assertRaises(ValueError):
            f.confirm()
        self.assertEqual(s.steps, [])

    def test_unhealthy_trial_cannot_confirm(self):
        for health in ("none", "00000000", "00000003"):
            with self.subTest(health=health):
                f, s, _ = self.start(read(trial(health=health)))
                with self.assertRaises(ValueError):
                    f.confirm()
                self.assertEqual(s.steps, [])

    def test_no_permission_no_upload(self):
        for changed in ({"permitted_slot": "none"}, {"candidate_slot": "B"},
                        {"confirmed_slot": "B"}, {"permitted_slot": "A"}):
            with self.subTest(changed=changed):
                s = Script(read(status(**changed)))
                with self.assertRaises(ValueError):
                    w.Workflow(s).upload({"A": artifact("A"), "B": artifact("B")})
                self.assertEqual(s.steps, [])

    def test_wrong_artifact_slot_rejected(self):
        s = Script(read(status()))
        with self.assertRaises(ValueError):
            w.Workflow(s).upload({"B": artifact("A")})
        self.assertEqual(s.steps, [])

    def test_artifact_exact_framing_and_bounds(self):
        valid = artifact("B")
        bad = [valid[:-1], valid+b"\xff", valid[:159], valid[:160]+bytes(236)]
        for offset in (16, 20, 24, 28, 32, 36, 44, 80):
            body = bytearray(valid); body[offset] ^= 0x80; bad.append(bytes(body))
        for body in bad:
            with self.subTest(length=len(body)):
                with self.assertRaises(ValueError):
                    w.Artifact.from_bytes(body)

    def test_strict_status_parser(self):
        good = status()
        bad = [good[:-1], good.replace(b"\n", b"\r\n"), good+b"extra=1\n", good.replace(b"format=1", b"format=2"),
               good.replace(b"stage=4", b"stage=6"), good.replace(b"health=00000007", b"health=00000008"),
               good.replace(b"hash="+b"0"*64, b"hash="+b"A"*64), b"x"*1025,
               good.replace(b"sequence=20", b"sequence=4294967296")]
        for body in bad:
            with self.subTest(body=body[:20]):
                with self.assertRaises((ValueError, UnicodeError)):
                    w.status(body)
        parsed = w.status(status(sequence="4294967295", trial_id="4294967295"))
        self.assertEqual(parsed["sequence"], "4294967295")
        self.assertEqual(parsed["reset_flags"], "00000000")
        self.assertEqual(w.status(status(trial_id="0000"))["trial_id"], "0000")

    def test_ready_is_not_trial_or_confirmed(self):
        f, s, _ = self.start(read(trial(trial_state="ready")))
        self.assertEqual(f.poll(1), "UNKNOWN")
        self.assertEqual(s.steps, [])

    def test_malformed_poll_is_unknown(self):
        f, s, _ = self.start(read(b"bad status\n"))
        self.assertEqual(f.poll(1), "UNKNOWN")
        self.assertEqual(s.steps, [])


if __name__ == "__main__":
    unittest.main()
