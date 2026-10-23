"""Offline FTP/data/control/time doubles only. Never open a socket."""
import contextlib
import ftplib
import io
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools" / "boot"))
import client
import ftp_transport as t
import workflow as w
from test_workflow import artifact, status, trial


class Clock:
    now = 0

    def __call__(self):
        return self.now


class Data:
    def __init__(self, body=b"", fragment=17, fail=None, clock=None):
        self.body, self.fragment, self.fail = body, fragment, fail
        self.sent = bytearray()
        self.closed = False
        self.timeouts = []
        self.clock = clock

    def settimeout(self, value):
        self.timeouts.append(value)

    def recv(self, count):
        if self.fail:
            raise self.fail
        if self.clock:
            self.clock.now += 1
        count = min(count, self.fragment)
        result, self.body = self.body[:count], self.body[count:]
        return result

    def send(self, body):
        if self.fail:
            raise self.fail
        count = min(len(body), self.fragment)
        self.sent.extend(body[:count])
        return count

    def close(self):
        self.closed = True


class FTP:
    def __init__(self, data=None, fail=None):
        self.sock = Data()
        self.data = data or Data()
        self.fail = fail or {}
        self.calls = []
        self.closed = False

    def arm(self, remaining):
        self.remaining = remaining

    def call(self, name, *args):
        self.calls.append((name, *args))
        if name in self.fail:
            raise self.fail[name]

    def connect(self, *args):
        self.call("connect", *args)
        return "220 ready"

    def login(self, *args):
        self.call("login", *args)
        return "230 logged in"

    def set_pasv(self, value):
        self.call("passive", value)

    def voidcmd(self, command):
        self.call(command)
        return "200 OK"

    def transfercmd(self, command):
        self.call(command)
        return self.data

    def voidresp(self):
        self.call("final")
        return "226 OK"

    def close(self):
        self.closed = True
        self.call("close")


class Factory:
    def __init__(self, *objects):
        self.objects = list(objects)
        self.calls = 0

    def __call__(self):
        self.calls += 1
        return self.objects.pop(0)


def transport(factory, **kwargs):
    return t.FTPTransport("127.0.0.1", 21, "anonymous", "SECRET",
                          ftp_factory=factory, **kwargs)


class TransportTests(unittest.TestCase):
    def test_fragmented_exact_1024_and_final(self):
        ftp = FTP(Data(bytes(range(256))*4, 7))
        tr = transport(Factory(ftp)); tr.connect()
        self.assertEqual(tr.retr("boot-status"), bytes(range(256))*4)
        self.assertTrue(ftp.data.closed)
        self.assertEqual(ftp.calls[-1], ("final",))

    def test_1025_rejected_without_drain_or_final(self):
        ftp = FTP(Data(b"x"*2000, 500))
        tr = transport(Factory(ftp)); tr.connect()
        with self.assertRaises(OSError): tr.retr("boot-status")
        self.assertEqual(len(ftp.data.body), 975)
        self.assertNotIn(("final",), ftp.calls)
        self.assertTrue(ftp.closed and ftp.data.closed)
        self.assertIsNone(tr.ftp)

    def test_partial_write_exact_firmware_and_confirm(self):
        for name, body in (("firmware", artifact("B")), ("confirm", b"format=1\n")):
            ftp = FTP(Data(fragment=3)); tr = transport(Factory(ftp)); tr.connect()
            tr.stor(name, body)
            self.assertEqual(ftp.data.sent, body)
            self.assertEqual(ftp.calls[-1], ("final",))
            self.assertGreaterEqual(ftp.calls.count(("TYPE I",)), 2)

    def test_transfer_faults_discard_and_redact(self):
        for step in ("TYPE I", "RETR boot-status", "final"):
            for error in (OSError("SECRET"), EOFError("SECRET"), ftplib.error_reply("SECRET")):
                ftp = FTP(); tr = transport(Factory(ftp)); tr.connect(); ftp.fail[step] = error
                with self.assertRaises(OSError) as caught: tr.retr("boot-status")
                self.assertNotIn("SECRET", str(caught.exception))
                self.assertIsNone(tr.ftp)
                self.assertTrue(ftp.closed)

    def test_data_send_and_receive_faults(self):
        for write in (False, True):
            ftp = FTP(Data(fail=OSError("SECRET"))); tr = transport(Factory(ftp)); tr.connect()
            with self.assertRaises(OSError):
                tr.stor("firmware", artifact("B")) if write else tr.retr("boot-status")
            self.assertTrue(ftp.closed and ftp.data.closed)

    def test_data_open_write_and_final_failures(self):
        for step in ("STOR firmware", "final"):
            ftp = FTP(); tr = transport(Factory(ftp)); tr.connect()
            ftp.fail[step] = ftplib.error_temp("SECRET")
            with self.assertRaises(OSError): tr.stor("firmware", artifact("B"))
            self.assertIsNone(tr.ftp)
            self.assertTrue(ftp.closed)
            if step == "final": self.assertTrue(ftp.data.closed)

    def test_maximum_write_bounds_are_exact(self):
        for name, size in (("firmware", 111776), ("confirm", 128)):
            ftp = FTP(Data(fragment=113)); tr = transport(Factory(ftp)); tr.connect()
            body = bytes(range(256)) * (size // 256) + bytes(size % 256)
            tr.stor(name, body)
            self.assertEqual(bytes(ftp.data.sent), body)
            self.assertEqual(ftp.calls[-1], ("final",))

    def test_timeout_parameters_rejected_before_factory(self):
        f = Factory()
        for invalid in (0, -1, float("inf"), float("nan"), 121, True):
            for key in ("call_timeout", "operation_timeout"):
                with self.assertRaises(ValueError): transport(f, **{key: invalid})
        self.assertEqual(f.calls, 0)

    def test_zero_send_is_eof(self):
        ftp = FTP(Data(fragment=0)); tr = transport(Factory(ftp)); tr.connect()
        with self.assertRaises(OSError): tr.stor("confirm", b"x")
        self.assertIsNone(tr.ftp)

    def test_cleanup_does_not_mask_final_reply_loss(self):
        ftp = FTP(fail={"final": EOFError("SECRET"), "close": RuntimeError("cleanup")})
        tr = transport(Factory(ftp)); tr.connect()
        with self.assertRaisesRegex(OSError, "outcome may be unknown"): tr.retr("boot-status")
        self.assertTrue(ftp.data.closed)

    def test_reconnect_discards_old_and_selects_type_i(self):
        a, b = FTP(), FTP(); tr = transport(Factory(a, b)); tr.connect(); tr.connect()
        self.assertTrue(a.closed)
        self.assertIn(("passive", True), b.calls)
        self.assertIn(("TYPE I",), b.calls)
        self.assertEqual(b.calls[0][0], "connect")
        self.assertEqual(b.calls[1][0], "login")

    def test_connect_login_and_binary_failures(self):
        for step in ("connect", "login", "TYPE I"):
            ftp = FTP(fail={step: ftplib.error_perm("SECRET")}); tr = transport(Factory(ftp))
            with self.assertRaises(OSError): tr.connect()
            self.assertTrue(ftp.closed)
        tr = transport(Factory())
        with self.assertRaises(OSError): tr.binary()

    def test_validation_before_factory_and_command_injection(self):
        f = Factory()
        for host in (1, b"\x7f\x00\x00\x01", "host.example", "127.0.0.1\r\nUSER x"):
            with self.assertRaises(ValueError): t.FTPTransport(host, 21, "a", "b", ftp_factory=f)
        for secret in ("x\r\nSTOR firmware", "\x00", "", "é"):
            with self.assertRaises(ValueError): t.FTPTransport("127.0.0.1", 21, "a", secret, ftp_factory=f)
        tr = transport(f)
        for name in ("firmware\r\nQUIT", "other"):
            with self.assertRaises(ValueError): tr.stor(name, b"x")
            with self.assertRaises(ValueError): tr.retr(name)
        for name, body in (("firmware", b"x"*111777), ("confirm", b"x"*129)):
            with self.assertRaises(ValueError): tr.stor(name, body)
        self.assertEqual(f.calls, 0)

    def test_total_deadline_across_fragments(self):
        clock = Clock(); ftp = FTP(Data(b"x"*100, 1, clock=clock))
        tr = transport(Factory(ftp), monotonic=clock, call_timeout=2, operation_timeout=3)
        tr.connect()
        with self.assertRaises(OSError): tr.retr("boot-status")
        self.assertEqual(len(ftp.data.body), 97)
        self.assertEqual(ftp.data.timeouts, [2, 2, 1])
        self.assertIsNone(tr.ftp)

    def test_connect_budget_is_shared_across_control_steps(self):
        clock = Clock(); ftp = FTP()
        original = ftp.call
        def call(name, *args):
            original(name, *args)
            if name in ("connect", "login"): clock.now += 2
        ftp.call = call
        tr = transport(Factory(ftp), monotonic=clock, operation_timeout=3)
        with self.assertRaises(OSError): tr.connect()
        self.assertNotIn(("TYPE I",), ftp.calls)
        self.assertIsNone(tr.ftp)

    def test_control_deadline_and_partial_control_write(self):
        clock = Clock(); ftp = t.DeadlineFTP(); ftp.sock = Data(b"200 OK\r\n", 1, clock=clock)
        tr = transport(Factory(), monotonic=clock, operation_timeout=3)
        tr._start(); ftp.arm(tr._remaining)
        with self.assertRaises(TimeoutError): ftp.getline()
        ftp.sock = Data(fragment=2); clock.now = 0; tr._start()
        ftp.putline("TYPE I")
        self.assertEqual(ftp.sock.sent, b"TYPE I\r\n")

    def test_lost_firmware_reply_keeps_provenance_no_second_stor(self):
        a = FTP(Data(status()))
        b = FTP(Data(trial()))
        factory = Factory(a, b); flow = w.Workflow(transport(factory), lambda _: None)
        # Use a fresh data double when the first connection starts firmware.
        original = a.transfercmd
        def transfer(command):
            if command == "STOR firmware":
                a.data = Data(fragment=5); a.fail["final"] = EOFError("lost")
            return original(command)
        a.transfercmd = transfer
        self.assertEqual(flow.upload({"A": artifact("A"), "B": artifact("B")}), "UNKNOWN")
        self.assertEqual(str(flow.upload_diagnostic), "phase=final category=disconnect reply=none")
        diagnostic = flow.upload_diagnostic
        self.assertEqual(flow.poll(1), "TRIAL")
        self.assertIs(flow.upload_diagnostic, diagnostic)
        self.assertEqual(flow.trial_id, 21)
        with self.assertRaises(ValueError): flow.upload({"B": artifact("B")})
        self.assertEqual(a.calls.count(("STOR firmware",)), 1)
        self.assertNotIn(("STOR firmware",), b.calls)


class WireSocket(Data):
    """Only socket primitives are doubled; ftplib reply/command paths are real."""
    def __init__(self, body=b"", *, clock=None, tick=0):
        super().__init__(body, fragment=65536)
        self.clock, self.tick = clock, tick
        self.endpoint = None

    def recv(self, count):
        if self.clock:
            self.clock.now += self.tick
        result, self.body = self.body[:count], self.body[count:]
        return result

    def connect(self, endpoint):
        self.endpoint = endpoint

    def getpeername(self):
        return ("127.0.0.1", 21)


class StdlibPathTests(unittest.TestCase):
    setup_replies = [b"220 ready", b"331 password", b"230 logged in", b"200 binary"]
    transfer_replies = [b"200 binary", b"227 passive (203,0,113,9,4,1)",
                        b"150 opening (2 bytes)", b"226 complete"]

    @contextlib.contextmanager
    def wire(self, replies, *, password="SECRET", clock=None, tick=0,
             operation_timeout=75):
        control = WireSocket(b"\r\n".join(replies) + b"\r\n", clock=clock, tick=tick)
        data = WireSocket(b"ok")
        # A missed patch is a hard failure, never a live resolver or connect.
        with mock.patch.object(t.socket, 'getaddrinfo',
                               side_effect=AssertionError("resolver entered")) as resolver, \
             mock.patch.object(t.socket, 'create_connection',
                               side_effect=AssertionError("create_connection entered")), \
             mock.patch.object(t.socket, 'socket', side_effect=[control, data]) as constructor:
            tr = t.FTPTransport("127.0.0.1", 21, "anonymous", password,
                                monotonic=clock or Clock(),
                                operation_timeout=operation_timeout)
            try:
                yield tr, control, data, constructor
            finally:
                tr.close()
                resolver.assert_not_called()

    def assert_failed(self, tr, control, action):
        with self.assertRaisesRegex(OSError, '^FTP operation failed; outcome may be unknown$'):
            action()
        self.assertIsNone(tr.ftp)
        self.assertTrue(control.closed)
        self.assertNotIn(b"ABOR", control.sent)

    def test_stor_diagnostics_cover_pre_send_send_and_final_without_retry(self):
        cases = (
            # Wrong positive replies must retain their numeric state too.
            ("type", 0, b"225 SECRET", "reply-state", 225, 0),
            ("passive", 1, b"425 SECRET", "reply-temporary", 425, 0),
            ("passive", 1, b"227 SECRET (127,0,0,1,999,1)", "protocol", 227, 0),
            ("data-connect", None, None, "disconnect", None, 0),
            ("STOR-preliminary", 2, b"550 SECRET", "reply-permanent", 550, 1),
            ("STOR-preliminary", 2, b"226 SECRET", "reply-state", 226, 1),
            ("send", None, None, "timeout", None, 1),
            ("final", 3, b"550 SECRET", "reply-permanent", 550, 1),
            ("final", 3, b"250 SECRET", "reply-state", 250, 1),
            ("final", 3, None, "disconnect", None, 1),
            ("final", 3, b"226 complete", "timeout", None, 1),
        )
        body = artifact("B")
        for phase, index, reply, category, code, stor_count in cases:
            with self.subTest(phase=phase, reply=reply):
                replies = self.transfer_replies.copy()
                if index is not None:
                    replies[index:index + 1] = [] if reply is None else [reply]
                with self.wire(self.setup_replies + replies) as (tr, c, d, ctor):
                    tr.connect()
                    if phase == "data-connect":
                        d.connect = mock.Mock(side_effect=ConnectionRefusedError("SECRET"))
                    if phase == "send":
                        d.send = mock.Mock(side_effect=TimeoutError("SECRET"))
                    if phase == "final" and category == "timeout":
                        original_recv = c.recv
                        def final_timeout(count):
                            if c.body.startswith(b"226 complete\r\n"):
                                # Timeout only after the full send and data close.
                                self.assertEqual(bytes(d.sent), body)
                                self.assertTrue(d.closed)
                                self.assertEqual(c.timeouts[-1], 5)
                                raise TimeoutError("SECRET")
                            return original_recv(count)
                        c.recv = final_timeout
                    with self.assertRaises(OSError) as caught:
                        tr.stor("firmware", body)
                    failure = caught.exception
                    self.assertEqual(failure.diagnostic, t.FTPDiagnostic(phase, category, code))
                    self.assertNotIn("SECRET", str(failure) + repr(failure) + str(failure.diagnostic))
                    self.assertTrue(failure.__suppress_context__)
                    self.assertIsInstance(failure, t.ProtocolError if category.startswith("reply-") else OSError)
                    self.assertEqual(c.sent.count(b"STOR firmware\r\n"), stor_count)
                    self.assertNotIn(b"ABOR", c.sent)
                    self.assertNotIn(b"QUIT", c.sent)
                    self.assertIsNone(tr.ftp)
                    self.assertTrue(c.closed)
                    if ctor.call_count == 2:
                        self.assertTrue(d.closed)
                    self.assertEqual(bytes(d.sent), body if phase == "final" else b"")
                    if phase == "final":
                        self.assertEqual(ctor.call_count, 2)  # No reconnect/retry.

    def test_setup_diagnostics_are_allowlisted_and_redacted(self):
        for phase, index, reply, category, code in (
            ("connect", 0, b"421 SECRET", "reply-temporary", 421),
            ("login", 2, b"530 SECRET", "reply-permanent", 530),
            ("type", 3, b"226 SECRET", "reply-state", 226),
            ("connect", 0, b"bad SECRET 530", "protocol", None),
        ):
            with self.subTest(phase=phase):
                replies = self.setup_replies.copy()
                replies[index] = reply
                with self.wire(replies) as (tr, c, _, _):
                    with self.assertRaises(t.ProtocolError) as caught:
                        tr.connect()
                    self.assertEqual(caught.exception.diagnostic, t.FTPDiagnostic(phase, category, code))
                    self.assertNotIn("SECRET", str(caught.exception.diagnostic))
                    self.assertNotIn(b"STOR", c.sent)
        for args in (("SECRET", "io"), ("final", "SECRET"), ("final", "io", "530 SECRET")):
            with self.subTest(args=args), self.assertRaises(ValueError):
                t.FTPDiagnostic(*args)

    def test_real_parser_success_numeric_connects_no_resolver(self):
        with self.wire(self.setup_replies + self.transfer_replies) as (tr, c, d, ctor):
            tr.connect()
            self.assertEqual(tr.retr("boot-status"), b"ok")
            self.assertEqual(c.endpoint, ("127.0.0.1", 21))
            self.assertEqual(d.endpoint, ("127.0.0.1", 1025))
            self.assertEqual(ctor.call_args_list,
                             [mock.call(t.socket.AF_INET, t.socket.SOCK_STREAM)] * 2)
            self.assertTrue(d.closed)
            self.assertIsNotNone(tr.ftp)

    def test_anonymous_dash_password_sent_literally(self):
        with self.wire(self.setup_replies, password="-") as (tr, c, _, _):
            tr.connect()
            self.assertEqual(c.sent, b"USER anonymous\r\nPASS -\r\nTYPE I\r\n")

    def test_wrong_positive_setup_states_discard(self):
        cases = ((0, b"120 wait"), (0, b"221 bye"), (0, b"230 logged in"),
                 (1, b"230 early success"), (1, b"332 account"), (1, b"200 wrong"),
                 (2, b"200 wrong"), (2, b"331 again"), (2, b"332 account"),
                 (2, b"221 bye"), (3, b"226 wrong"), (3, b"225 abort"))
        for index, reply in cases:
            with self.subTest(index=index, reply=reply):
                replies = self.setup_replies.copy(); replies[index] = reply
                with self.wire(replies) as (tr, c, _, _):
                    self.assert_failed(tr, c, tr.connect)
                    self.assertNotIn(b"ACCT", c.sent)
                    if index == 0:
                        self.assertEqual(c.sent, b"")

    def test_preliminary_greeting_not_consumed_as_login_reply(self):
        for tail in ([], [b"220 delayed ready", *self.setup_replies[1:]]):
            with self.wire([b"120 wait", *tail]) as (tr, c, _, _):
                self.assert_failed(tr, c, tr.connect)
                self.assertEqual(c.sent, b"")

    def test_wrong_positive_transfer_states_discard_and_close(self):
        for index, codes in ((0, (221, 225, 226)), (1, (200, 229)),
                             (2, (110, 125, 200, 221, 225, 226)),
                             (3, (200, 221, 225, 250))):
            for code in codes:
                with self.subTest(index=index, code=code):
                    replies = self.transfer_replies.copy()
                    replies[index] = f"{code} wrong state".encode()
                    with self.wire(self.setup_replies + replies) as (tr, c, d, ctor):
                        tr.connect()
                        self.assert_failed(tr, c, lambda: tr.retr("boot-status"))
                        self.assertEqual(ctor.call_count, 1 if index < 2 else 2)
                        if index >= 2:
                            self.assertTrue(d.closed)

    def test_malformed_passive_numbers_discard_before_data_socket(self):
        for fields in ("127,0,0,1,999999999999999999999999,1",
                       "127,0,0,1," + "9" * 5000 + ",1",
                       "256,0,0,1,4,1", "127,0,0,1,1,256", "127,0,0,1,0,0"):
            with self.subTest(fields=fields[:50]):
                replies = self.transfer_replies.copy()
                replies[1] = ("227 passive (" + fields + ")").encode()
                with self.wire(self.setup_replies + replies) as (tr, c, _, ctor):
                    tr.connect()
                    self.assert_failed(tr, c, lambda: tr.retr("boot-status"))
                    self.assertEqual(ctor.call_count, 1)

    def test_over_digit_limit_150_closes_owned_data_socket(self):
        # Run the actual stdlib int conversion with a deterministic digit limit.
        old = sys.get_int_max_str_digits()
        try:
            sys.set_int_max_str_digits(4300)
            replies = self.transfer_replies.copy()
            replies[2] = b"150 opening (" + b"9" * 5000 + b" bytes)"
            with self.wire(self.setup_replies + replies) as (tr, c, d, _):
                tr.connect()
                self.assert_failed(tr, c, lambda: tr.retr("boot-status"))
                self.assertTrue(d.closed)
                self.assertEqual(d.body, b"ok")
        finally:
            sys.set_int_max_str_digits(old)

    def test_numeric_parser_conversion_errors_normalized_at_boundary(self):
        for parser in ('parse227', 'parse150'):
            for error in (ValueError("SECRET"), OverflowError("SECRET")):
                with self.subTest(parser=parser, error=type(error).__name__):
                    with self.wire(self.setup_replies + self.transfer_replies) as (tr, c, d, ctor):
                        tr.connect()
                        with mock.patch.object(ftplib, parser, side_effect=error):
                            self.assert_failed(tr, c, lambda: tr.retr("boot-status"))
                        self.assertEqual(ctor.call_count, 1 if parser == 'parse227' else 2)
                        if parser == 'parse150':
                            self.assertTrue(d.closed)

    def test_pasv_delay_reduces_direct_data_connect_timeout(self):
        clock = Clock()
        with self.wire(self.setup_replies + self.transfer_replies,
                       clock=clock, operation_timeout=4) as (tr, c, d, _):
            tr.connect()
            original = c.recv
            def recv(count):
                byte = original(count)
                if c.body.startswith(b"150"):
                    clock.now = 3  # PASV consumed, before data socket creation.
                return byte
            c.recv = recv
            self.assertEqual(tr.retr("boot-status"), b"ok")
            self.assertEqual(d.timeouts[0], 1)

    def test_exact_lf_crlf_control_line_boundaries(self):
        for ending in (b"\n", b"\r\n"):
            for excess in (0, 1):
                ftp = t.DeadlineFTP(); ftp.maxline = 12; ftp.arm(lambda: 5)
                ftp.sock = WireSocket(b"x" * (12 - len(ending) + excess) + ending)
                try:
                    if excess:
                        with self.assertRaises(ftplib.error_proto): ftp.getline()
                    else:
                        self.assertEqual(ftp.getline(), "x" * (12 - len(ending)))
                finally:
                    ftp.close()

    @staticmethod
    def bounded_reply(code, excess=0):
        # Sixteen 4096-byte wire lines: CRLF, codes and UTF-8 all count.
        return ([code + b'-' + b'\xc3\xa9' * 2045] +
                [b'x' * 4094] * 14 +
                [code + b' ' + b'x' * (4090 + excess)])

    def test_valid_multiline_setup_and_transfer_real_parser(self):
        replies = []
        for line in self.setup_replies + self.transfer_replies:
            code = line[:3]
            replies.extend([code + b'-' + line[4:], b'free form content',
                            b'250 foreign code is content', code + b'-continued',
                            code + b' done'])
        with self.wire(replies) as (tr, c, d, _):
            tr.connect()
            self.assertEqual(tr.retr('boot-status'), b'ok')
            self.assertEqual(c.body, b'')
            self.assertTrue(d.closed)
            self.assertIsNotNone(tr.ftp)
            self.assertIn(b'PASS SECRET\r\n', c.sent)

    def test_aggregate_exact_boundary_and_one_byte_over(self):
        self.assertEqual(t.DeadlineFTP.max_reply_bytes, 65536)
        for stage, code in ((0, b'220'), (6, b'150'), (7, b'226')):
            for excess in (0, 1):
                with self.subTest(stage=stage, excess=excess):
                    replies = self.setup_replies + self.transfer_replies
                    bounded = self.bounded_reply(code, excess)
                    self.assertEqual(sum(len(x) + 2 for x in bounded), 65536 + excess)
                    replies[stage:stage + 1] = bounded
                    with self.wire(replies) as (tr, c, d, _):
                        if stage:
                            tr.connect()
                        action = tr.connect if stage == 0 else lambda: tr.retr('boot-status')
                        if excess:
                            self.assert_failed(tr, c, action)
                            # The overflow byte is LF: fail immediately, do not
                            # consume the next reply or wait for another read.
                            tail = self.setup_replies[1:] + self.transfer_replies if stage == 0 else []
                            if stage == 6:
                                tail = [self.transfer_replies[3]]
                            self.assertEqual(c.body, b'\r\n'.join(tail) + (b'\r\n' if tail else b''))
                            if stage:
                                self.assertTrue(d.closed)
                                self.assertEqual(d.body, b'ok' if stage == 6 else b'')
                        else:
                            result = action()
                            if stage:
                                self.assertEqual(result, b'ok')
                            self.assertIsNotNone(tr.ftp)

    def test_error_reply_aggregate_overflow_is_bounded_and_redacted(self):
        replies = self.bounded_reply(b'530', 1) + [b'220 unread SECRET']
        with self.wire(replies) as (tr, c, _, ctor):
            self.assert_failed(tr, c, tr.connect)
            self.assertEqual(c.body, b'220 unread SECRET\r\n')
            self.assertEqual(ctor.call_count, 1)
            self.assertEqual(c.sent, b'')

    def test_malformed_matching_code_terminators_fail_immediately(self):
        for stage in (0, 1, 2, 6, 7):
            for suffix in (b'', b'XSECRET', b'\tSECRET', b'!SECRET'):
                with self.subTest(stage=stage, suffix=suffix):
                    replies = self.setup_replies + self.transfer_replies
                    code = replies[stage][:3]
                    replies[stage:stage + 1] = [code + b'-start', code + suffix,
                                               code + b' valid but unread']
                    with self.wire(replies) as (tr, c, d, _):
                        if stage >= 6:
                            tr.connect()
                        self.assert_failed(tr, c, tr.connect if stage < 6 else
                                           lambda: tr.retr('boot-status'))
                        self.assertTrue(c.body.startswith(code + b' valid but unread\r\n'))
                        if stage >= 6:
                            self.assertTrue(d.closed)

    def test_foreign_code_is_content_not_terminator(self):
        for tail in ([], [b'226 complete']):
            replies = self.setup_replies + self.transfer_replies[:3]
            replies += [b'226-start', b'250 done', b'200 wrong', b'foreign', *tail]
            with self.wire(replies) as (tr, c, d, _):
                tr.connect()
                if tail:
                    self.assertEqual(tr.retr('boot-status'), b'ok')
                else:
                    self.assert_failed(tr, c, lambda: tr.retr('boot-status'))
                self.assertEqual(c.body, b'')
                self.assertTrue(d.closed)

    def test_reply_line_count_exact_and_over_without_clock_progress(self):
        self.assertEqual(t.DeadlineFTP.max_reply_lines, 256)
        for count in (256, 257):
            replies = [b'220-start'] + [b'content'] * (count - 2) + [b'220 done']
            with self.wire(replies + self.setup_replies[1:]) as (tr, c, _, _):
                if count == 256:
                    tr.connect()
                    self.assertIsNotNone(tr.ftp)
                else:
                    self.assert_failed(tr, c, tr.connect)
                    self.assertTrue(c.body.startswith(b'220 done\r\n'))
                    self.assertEqual(c.sent, b'')

    def test_endless_multiline_progress_does_not_reset_deadline(self):
        for stage in (0, 6, 7):
            clock = Clock()
            replies = self.setup_replies + self.transfer_replies
            code = replies[stage][:3]
            replies[stage:stage + 1] = [code + b'-start'] + [b'250 content'] * 100
            with self.wire(replies, clock=clock, operation_timeout=60) as (tr, c, d, _):
                if stage:
                    tr.connect()
                original = c.recv
                def recv(count):
                    if c.body.startswith(code + b'-start'):
                        c.tick = 1
                    return original(count)
                c.recv = recv
                self.assert_failed(tr, c, tr.connect if stage == 0 else
                                   lambda: tr.retr('boot-status'))
                self.assertEqual(clock.now, 60)
                self.assertTrue(c.body)
                if stage:
                    self.assertTrue(d.closed)

    def test_multiline_failure_has_no_stale_session_and_can_reconnect(self):
        with self.wire([b'220-start', b'220XSECRET']) as (tr, c, _, ctor):
            self.assert_failed(tr, c, tr.connect)
            sent = bytes(c.sent)
            self.assert_failed(tr, c, tr.binary)
            self.assert_failed(tr, c, lambda: tr.retr('boot-status'))
            self.assertEqual(c.sent, sent)
            self.assertEqual(ctor.call_count, 1)
            fresh = WireSocket(b'\r\n'.join(self.setup_replies) + b'\r\n')
            ctor.side_effect = [fresh]
            tr.connect()
            self.assertIs(tr.ftp.sock, fresh)
            self.assertEqual(fresh.body, b'')
            self.assertTrue(c.closed)

    def test_real_control_trickle_expires_even_on_final_newline(self):
        for budget in (3, len(b"220 ready\r\n")):
            clock = Clock()
            with self.wire(self.setup_replies, clock=clock, tick=1,
                           operation_timeout=budget) as (tr, c, _, _):
                self.assert_failed(tr, c, tr.connect)
                self.assertEqual(clock.now, budget)
                self.assertEqual(c.sent, b"")


class ClientTests(unittest.TestCase):
    def run_client(self, factory, args, **kwargs):
        out, err = io.StringIO(), io.StringIO()
        result = client.main(["--host", "127.0.0.1", *args], ftp_factory=factory,
                             environ={"BOOT_FTP_PASSWORD": "SECRET"}, stdout=out, stderr=err,
                             sleep=lambda _: None, **kwargs)
        return result, out.getvalue(), err.getvalue()

    def test_status_binary_and_no_stor(self):
        ftp = FTP(Data(status()))
        code, out, err = self.run_client(Factory(ftp), ["status"])
        self.assertEqual(code, 0); self.assertEqual(out, status().decode()); self.assertEqual(err, "")
        self.assertFalse(any(call[0].startswith("STOR") for call in ftp.calls))

    def test_invalid_arguments_zero_factory(self):
        f = Factory()
        for args in (["update"], ["resolve", "--trial-id", "-1", "--package", "x"],
                     ["status", "--confirm"], ["--port", "0", "status"]):
            with contextlib.redirect_stderr(io.StringIO()):
                try: result = self.run_client(f, args)[0]
                except SystemExit as exc: result = exc.code
            self.assertEqual(result, 2)
        self.assertEqual(f.calls, 0)

    def test_package_limit_before_factory(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "large"; path.write_bytes(b"x"*111777)
            f = Factory()
            code, _, _ = self.run_client(f, ["resolve", "--package", str(path), "--trial-id", "0"])
            self.assertEqual(code, 2); self.assertEqual(f.calls, 0)

    def test_safe_errors_redact_secrets(self):
        ftp = FTP(fail={"login": ftplib.error_perm("SECRET")})
        code, out, err = self.run_client(Factory(ftp), ["status"])
        self.assertEqual(code, 4); self.assertNotIn("SECRET", out + err)
        self.assertNotIn("CONFIRMED", out)

    def test_resolve_read_only_exact_retained_identity(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "b"; path.write_bytes(artifact("B"))
            ftp = FTP(Data(trial(confirmed_slot="B", candidate_slot="none", trial_state="none", health="none")))
            code, out, _ = self.run_client(Factory(ftp), ["resolve", "--package", str(path), "--trial-id", "21", "--polls", "1"])
            self.assertEqual((code, out), (0, "CONFIRMED\n"))
            self.assertFalse(any(c[0].startswith("STOR") for c in ftp.calls))

    def test_update_confirm_lost_reply_same_session(self):
        with tempfile.TemporaryDirectory() as directory:
            a, b = Path(directory) / "a", Path(directory) / "b"
            a.write_bytes(artifact("A")); b.write_bytes(artifact("B"))
            first, running, confirming = FTP(Data(status())), FTP(Data(trial())), FTP(Data(trial()))
            confirmed = FTP(Data(trial(confirmed_slot="B", candidate_slot="none", trial_state="none")))
            for ftp, name in ((first, "firmware"), (confirming, "confirm")):
                original = ftp.transfercmd
                def transfer(command, ftp=ftp, name=name, original=original):
                    if command == "STOR " + name:
                        ftp.data = Data(fragment=2)
                        ftp.fail["final"] = EOFError("SECRET lost reply")
                    return original(command)
                ftp.transfercmd = transfer
            code, out, err = self.run_client(Factory(first, running, confirming, confirmed),
                                             ["update", "--slot-a", str(a), "--slot-b", str(b),
                                              "--polls", "1", "--confirm"])
            self.assertEqual((code, out, err), (0, "CONFIRMED\n", ""))
            self.assertEqual(first.calls.count(("STOR firmware",)), 1)
            self.assertEqual(confirming.calls.count(("STOR confirm",)), 1)
            digest = w.Artifact.from_bytes(artifact("B")).digest
            self.assertEqual(confirming.data.sent, f"format=1\nslot=B\nhash={digest}\ntrial_id=21\n".encode())

    def test_update_without_explicit_confirm_stops_at_trial(self):
        with tempfile.TemporaryDirectory() as directory:
            a, b = Path(directory) / "a", Path(directory) / "b"
            a.write_bytes(artifact("A")); b.write_bytes(artifact("B"))
            first, running = FTP(Data(status())), FTP(Data(trial()))
            original = first.transfercmd
            def transfer(command):
                if command == "STOR firmware": first.data = Data()
                return original(command)
            first.transfercmd = transfer
            code, out, _ = self.run_client(Factory(first, running),
                                           ["update", "--slot-a", str(a), "--slot-b", str(b), "--polls", "1"])
            self.assertEqual((code, out), (3, "TRIAL\n"))
            self.assertFalse(any(c[0] == "STOR confirm" for ftp in (first, running) for c in ftp.calls))

    def test_poll_interval_and_stale_provenance(self):
        first, stale, healthy = FTP(Data(status())), FTP(Data(trial(trial_id="20"))), FTP(Data(trial()))
        original = first.transfercmd
        def transfer(command):
            if command == "STOR firmware": first.data = Data()
            return original(command)
        first.transfercmd = transfer
        sleeps = []
        flow = w.Workflow(transport(Factory(first, stale, healthy)), sleeps.append)
        flow.upload({"A": artifact("A"), "B": artifact("B")})
        self.assertEqual(flow.poll(2), "TRIAL")
        self.assertEqual(sleeps, [1]); self.assertEqual(flow.trial_id, 21)
        self.assertTrue(first.closed and stale.closed)

    def test_exit_mapping_does_not_label_uncertain_success(self):
        self.assertEqual(client.EXIT, {"CONFIRMED": 0, "STATUS": 0, "TRIAL": 3,
                                      "UNKNOWN": 4, "MISMATCH": 5, "UNHEALTHY": 6})


if __name__ == "__main__":
    unittest.main()
