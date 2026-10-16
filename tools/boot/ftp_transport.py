"""Bounded stdlib FTP transport for an unsigned restricted bench."""
import ftplib
import ipaddress
import math
import re
import socket
import time


class DeadlineFTP(ftplib.FTP):
    """Restricted reply states, direct numeric sockets, per-byte deadlines."""
    max_reply_bytes = 65536  # Wire bytes, including every CR/LF and code.
    max_reply_lines = 256
    _reply_bytes = None

    def arm(self, remaining):
        self.remaining = remaining

    def getline(self):
        line = bytearray()
        while True:
            self.sock.settimeout(self.remaining())
            byte = self.sock.recv(1)
            if not byte:
                raise EOFError
            if self._reply_bytes is not None:
                self._reply_bytes += len(byte)
                if self._reply_bytes > self.max_reply_bytes:
                    self.close()
                    raise ftplib.error_proto("control reply too long")
            if len(line) + len(byte) > self.maxline:
                self.close()
                raise ftplib.error_proto("control line too long")
            line.extend(byte)
            if byte == b"\n":
                self.remaining()
                return line.decode(self.encoding).rstrip("\r\n")

    def getmultiline(self):
        # ftplib's reader accepts code-X and bare-code terminators. RFC 959
        # requires the opening code plus SPACE; foreign codes remain content.
        self._reply_bytes = 0
        try:
            line = self.getline()
            if (len(line) < 4 or not re.fullmatch(r'[0-9]{3}', line[:3])
                    or line[3] not in (' ', '-')):
                raise ftplib.error_proto("invalid reply framing")
            lines = [line]
            if line[3] == '-':
                code = line[:3]
                while True:
                    if len(lines) >= self.max_reply_lines:
                        raise ftplib.error_proto("too many control reply lines")
                    line = self.getline()
                    lines.append(line)
                    if line[:3] == code:
                        separator = line[3:4]
                        if separator == ' ':
                            break
                        if separator != '-':
                            raise ftplib.error_proto("invalid reply terminator")
            return '\n'.join(lines)
        except (ftplib.Error, OSError, EOFError, UnicodeError):
            self.close()
            raise
        finally:
            self._reply_bytes = None

    def putline(self, line):
        if '\r' in line or '\n' in line:
            raise ValueError("invalid control line")
        data = (line + '\r\n').encode(self.encoding)
        while data:
            self.sock.settimeout(self.remaining())
            sent = self.sock.send(data)
            if sent <= 0:
                raise EOFError
            data = data[sent:]
        self.remaining()

    def close(self):
        # Independently attempt both resources, suppressing cleanup failures.
        file, sock = self.file, self.sock
        self.file = self.sock = None
        for obj in (file, sock):
            if obj is not None:
                try:
                    obj.close()
                except Exception:
                    pass

    @staticmethod
    def expect(reply, code):
        if reply[:3] != code or reply[3:4] not in (' ', '-'):
            raise ftplib.error_reply("unexpected reply state")
        return reply

    def _connect_numeric(self, host, port):
        # Never enter create_connection/getaddrinfo, even for numeric input.
        ipaddress.IPv4Address(host)
        conn = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            conn.settimeout(self.remaining())
            conn.connect((host, port))
            self.remaining()
            return conn
        except BaseException:
            try:
                conn.close()
            except Exception:
                pass
            raise

    def connect(self, host='', port=21, timeout=None):
        self.host, self.port, self.timeout = host, port, timeout
        self.sock = self._connect_numeric(host, port)
        self.af = socket.AF_INET
        # getline uses recv directly: no buffered makefile is needed.
        self.welcome = self.expect(self.getresp(), '220')
        return self.welcome

    def login(self, user='', passwd='', acct=''):
        # No anonymous '-' rewrite and no implicit ACCT or early USER success.
        self.expect(self.sendcmd('USER ' + user), '331')
        return self.expect(self.sendcmd('PASS ' + passwd), '230')

    def makepasv(self):
        reply = self.expect(self.sendcmd('PASV'), '227')
        fields = re.search(r'(\d+),(\d+),(\d+),(\d+),(\d+),(\d+)',
                           reply, re.ASCII)
        # Validate before stdlib's unbounded integer conversion.
        if fields is None or any(len(x) > 3 or int(x) > 255
                                 for x in fields.groups()):
            raise ftplib.error_proto('invalid passive endpoint')
        try:
            _, port = ftplib.parse227(reply)
        except (ValueError, OverflowError):
            raise ftplib.error_proto('invalid passive endpoint') from None
        if not 1 <= port <= 65535:
            raise ftplib.error_proto('invalid passive endpoint')
        # Always use control peer, never advertised foreign PASV address.
        return self.sock.getpeername()[0], port

    def ntransfercmd(self, cmd, rest=None):
        if not self.passiveserver or rest is not None:
            raise ftplib.error_proto('unsupported transfer mode')
        host, port = self.makepasv()
        conn = self._connect_numeric(host, port)
        try:
            reply = self.expect(self.sendcmd(cmd), '150')
            try:
                size = ftplib.parse150(reply)
            except (ValueError, OverflowError):
                raise ftplib.error_proto('invalid transfer size') from None
            self.remaining()
            return conn, size
        except BaseException:
            # Explicit ownership includes preliminary numeric parsing.
            try:
                conn.close()
            except Exception:
                pass
            raise


def validate(host, port, username, password, call_timeout, operation_timeout):
    try:
        if not isinstance(host, str):
            raise ValueError
        ipaddress.IPv4Address(host)
    except (ValueError, TypeError):
        raise ValueError("numeric IPv4 endpoint required") from None
    if type(port) is not int or not 1 <= port <= 65535:
        raise ValueError("port outside limits")
    for value in (username, password):
        if (not isinstance(value, str) or not value or len(value) > 256 or
                any(ord(c) < 32 or ord(c) > 126 for c in value)):
            raise ValueError("invalid FTP credentials")
    for value in (call_timeout, operation_timeout):
        if (type(value) not in (int, float) or not math.isfinite(value) or
                not 0 < value <= 120):
            raise ValueError("timeout outside limits")


class FTPTransport:
    def __init__(self, host, port, username, password, *, ftp_factory=DeadlineFTP,
                 call_timeout=5, operation_timeout=75, monotonic=time.monotonic):
        validate(host, port, username, password, call_timeout, operation_timeout)
        self.host, self.port = host, port
        self.username, self.password = username, password
        self.factory = ftp_factory
        self.call_timeout, self.operation_timeout = call_timeout, operation_timeout
        self.clock = monotonic
        self.ftp = None
        self.end = 0

    def _remaining(self):
        left = self.end - self.clock()
        if left <= 0:
            raise TimeoutError("FTP operation deadline")
        return min(self.call_timeout, left)

    def _start(self):
        self.end = self.clock() + self.operation_timeout

    @staticmethod
    def _close(obj):
        if obj is not None:
            try:
                obj.close()
            except Exception:
                pass  # Cleanup must not hide the operation result.

    def close(self):
        old, self.ftp = self.ftp, None
        self._close(old)  # No QUIT/ABOR on a possibly desynchronized stream.

    def _step(self, method, *args):
        self.ftp.timeout = self._remaining()
        if self.ftp.sock is not None:
            self.ftp.sock.settimeout(self._remaining())
        result = method(*args)
        self._remaining()
        return result

    def _binary(self):
        DeadlineFTP.expect(self._step(self.ftp.voidcmd, "TYPE I"), '200')

    def _fail(self):
        self.close()
        # Neither server reply text nor credentials are safe diagnostics.
        raise OSError("FTP operation failed; outcome may be unknown") from None

    def connect(self):
        self.close()
        self._start()
        try:
            self.ftp = self.factory()
            self.ftp.arm(self._remaining)
            DeadlineFTP.expect(self._step(self.ftp.connect, self.host, self.port,
                                          self._remaining()), '220')
            DeadlineFTP.expect(self._step(self.ftp.login, self.username,
                                          self.password), '230')
            self.ftp.set_pasv(True)
            self._binary()
        except (ftplib.Error, OSError, EOFError, UnicodeError):
            self._fail()

    def binary(self):
        self._start()
        try:
            if self.ftp is None:
                raise OSError
            self._binary()
        except (ftplib.Error, OSError, EOFError, UnicodeError):
            self._fail()

    def _transfer(self, command, body=None):
        self._start()
        data = None
        try:
            if self.ftp is None:
                raise OSError
            self._binary()
            self.ftp.timeout = self._remaining()
            data = self.ftp.transfercmd(command)
            self._remaining()
            if body is None:
                output = bytearray()
                while True:
                    data.settimeout(self._remaining())
                    chunk = data.recv(min(512, 1025 - len(output)))
                    self._remaining()
                    if not chunk:
                        break
                    output.extend(chunk)
                    if len(output) > 1024:
                        raise OSError("status exceeds limit")
                result = bytes(output)
            else:
                pos = 0
                while pos < len(body):
                    data.settimeout(self._remaining())
                    sent = data.send(body[pos:pos + 4096])
                    self._remaining()
                    if sent <= 0:
                        raise EOFError
                    pos += sent
                result = None
            data.close()
            data = None
            # Data EOF alone, 221, and ABOR's 225 are not completion.
            DeadlineFTP.expect(self._step(self.ftp.voidresp), '226')
            return result
        except (ftplib.Error, OSError, EOFError, UnicodeError):
            self._fail()
        finally:
            self._close(data)

    def retr(self, name):
        if name != "boot-status":
            raise ValueError("unsupported RETR name")
        return self._transfer("RETR boot-status")

    def stor(self, name, body):
        if name not in ("firmware", "confirm"):
            raise ValueError("unsupported STOR name")
        limit = 111776 if name == "firmware" else 128
        minimum = 396 if name == "firmware" else 1
        if not isinstance(body, bytes) or not minimum <= len(body) <= limit:
            raise ValueError("STOR body outside limits")
        return self._transfer("STOR " + name, body)
