"""Disk-backed, bounded command evidence for the isolated resource fixture."""
import json
from pathlib import Path
import subprocess
import tempfile

OUTPUT_LIMIT = 1024 * 1024


class CommandEvidence:
    def __init__(self, directory):
        self.directory = Path(directory)
        self.directory.mkdir(parents=True, exist_ok=True)
        self.serial = 0

    def run(self, argv, label, *, timeout=60, check=True):
        self.serial += 1
        stem = self.directory / (f'{self.serial:03d}-' + label)
        argv = [str(x) for x in argv]
        record = dict(argv=argv, timeout_seconds=timeout, returncode=None,
                      status='started', timed_out=False)
        metadata = Path(str(stem) + '.json')
        metadata.write_text(json.dumps(record, indent=2) + '\n')
        error = None
        bodies = []
        with tempfile.TemporaryFile() as stdout, tempfile.TemporaryFile() as stderr:
            try:
                p = subprocess.run(argv, stdout=stdout, stderr=stderr, timeout=timeout)
                record.update(returncode=p.returncode, status='completed')
            except subprocess.TimeoutExpired as exc:
                error = exc
                record.update(status='timeout', timed_out=True)
            except OSError as exc:
                error = exc
                record.update(status='launch-error', error=str(exc))
            for name, stream in (('stdout', stdout), ('stderr', stderr)):
                size = stream.tell()
                stream.seek(0)
                body = stream.read(OUTPUT_LIMIT)
                Path(str(stem) + '.' + name).write_bytes(body)
                record[name + '_bytes'] = size
                record[name + '_truncated'] = size > OUTPUT_LIMIT
                bodies.append(body)
        metadata.write_text(json.dumps(record, indent=2) + '\n')
        if error is not None:
            reason = 'command timeout: ' if record['timed_out'] else 'command launch error: '
            raise ValueError(reason + str(metadata)) from error
        if record['stdout_truncated'] or record['stderr_truncated']:
            raise ValueError('incomplete command output: ' + str(metadata))
        # Decode only after preserving byte-exact streams; invalid encoding fails.
        stdout, stderr = (body.decode('utf-8') for body in bodies)
        if check and record['returncode'] != 0:
            raise ValueError('command failed: ' + str(metadata))
        return subprocess.CompletedProcess(argv, record['returncode'], stdout, stderr)
