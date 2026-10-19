"""Execute prebuilt focused cases only, fresh process each, no retries/builds.
Raw evidence lives under /tmp; vault receives logs/JSONL only. Normal stdout is compact JSONL.
"""
import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
import signal
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[3]
CASES = {
    'owner': ['owner_completed_turn_sequence', 'owner_timer_reentry_quarantines',
              'owner_partial_report_failure_no_retry', 'owner_ambiguous_feed_no_retry',
              'owner_watchdog_read_reentry_stops_before_icr'],
    'clock': ['c_ready_validated_before_app', 'systick_admission_rejects_unsafe_core',
              'systick_poststart_pending_fails_closed', 'systick_offset_no_retained_io',
              'gptm_explicit_timer_claim_configuration', 'gptm_unknown_or_enabled_never_stops_timer',
              'gptm_claim_io_failure_consumed', 'gptm_expiry_before_tar', 'gptm_expiry_after_tar',
              'gptm_drift_rejects_unpublished_sample', 'gptm_claim_reentry_terminal',
              'gptm_read_reentry_terminal', 'gptm_release_consumed_no_doublewrite'],
    'netif': ['netif_completed_work_returns_true', 'netif_uninitialized_returns_false',
              'netif_phy_timeout_returns_false', 'netif_rx_error_returns_false'],
    'tcp': ['ftp_one_step_then_reentry_sticky', 'ftp_missing_core_sticky'],
    'target': ['target_authenticated_foreground_continuity'],
}

def expected_nested():
    return [{'configuration': c, 'status':'pass', 'scenarios':
             [{'case':s,'status':'pass'} for s in
              ('msp-clean','psp-clean','msp-masks','psp-masks','pre-c-clock-reject')]}
            for c in ('bootOs/appOs-A','bootOs/appOg-B')]

NESTED = {
 'c_ready_validated_before_app': ['readiness-0','readiness-1','readiness-2','readiness-3'],
 'systick_admission_rejects_unsafe_core': ['mask','unknown-source','shcsr'],
 'gptm_explicit_timer_claim_configuration': ['timer-0','timer-1','timer-2','timer-3'],
 'gptm_release_consumed_no_doublewrite': ['normal','ris-reentry','ctl-reentry'],
 'target_authenticated_foreground_continuity': [c+'/'+s for c in ('bootOs/appOs-A','bootOs/appOg-B') for s in
    ('msp-clean','psp-clean','msp-masks','psp-masks','pre-c-clock-reject')]}

def emit(record, stream):
    stream.write(json.dumps(record, separators=(',', ':')) + '\n')
    stream.flush()

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bin-dir', type=Path, required=True)
    parser.add_argument('--group', choices=CASES)
    select = parser.add_mutually_exclusive_group(required=True)
    select.add_argument('--all', action='store_true')
    select.add_argument('--case', action='append', default=[])
    parser.add_argument('--human', action='store_true')
    parser.add_argument('--failures-only', action='store_true')
    parser.add_argument('--timeout', type=float, help='gate-supplied per-case seconds; no default')
    args = parser.parse_args()
    if args.timeout is not None and args.timeout <= 0:
        parser.error('--timeout must be positive')
    lookup = {case: group for group, cases in CASES.items() for case in cases}
    roster = ([case for group, cases in CASES.items() if not args.group or args.group == group
               for case in cases] if args.all else args.case)
    if len(set(roster)) != len(roster):
        parser.error('duplicate case selectors')
    if any(c not in lookup or (args.group and lookup[c] != args.group) for c in roster):
        parser.error('unknown case or case outside group')
    base = ROOT / '.dev-vault/tests/runs'
    base.mkdir(parents=True, exist_ok=True)
    label = args.group or 'all'
    stamp = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ')
    directory = Path(tempfile.mkdtemp(prefix=stamp + '-clock-switch-focused-' + label + '-', dir=base))
    raw_directory = Path(tempfile.mkdtemp(prefix=stamp + '-foreground-' + label + '-', dir='/tmp'))
    (directory / 'evidence.md').write_text('Raw artifacts: `' + str(raw_directory) + '`\n')
    (directory / 'owner.md').write_text(label + '\n')
    (raw_directory / 'owner.md').write_text(label + '\n')
    report = directory / 'report.jsonl'
    logpath = directory / 'run.log'
    observed = []
    stopped = False
    nested_complete = True
    nested_observed = {}
    counts = dict(pass_=0, fail=0, invalid=0)
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1',
               ASAN_OPTIONS='halt_on_error=1', UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
    with report.open('w') as digest, logpath.open('w') as log:
        for case in roster:
            group = lookup[case]
            if stopped:
                record = {'case': case, 'status': 'invalid', 'reason': 'not attempted after prior non-pass'}
            else:
                if group == 'target':
                    command = [sys.executable, str(ROOT / 'tests/boot/test_foreground_target.py'),
                               str(raw_directory / 'target')]
                else:
                    command = [str(args.bin_dir.resolve() / ('foreground_' + group)), case]
                log.write(json.dumps({'case': case, 'argv': command}) + '\n'); log.flush()
                stream_path = directory / (case + '.log')
                record = None
                try:
                    with stream_path.open('w') as stream:
                        process = subprocess.Popen(command, stdout=stream, stderr=subprocess.STDOUT,
                                                   env=env, start_new_session=True)
                        timed_out = False
                        try:
                            process.wait(timeout=args.timeout)
                        except subprocess.TimeoutExpired:
                            timed_out = True
                            os.killpg(process.pid, signal.SIGKILL)
                            process.wait()
                        result = process
                    # Full bytes remain on disk; inspect bounded output, mark overflow invalid.
                    with stream_path.open('rb') as stream:
                        raw = stream.read(1024 * 1024 + 1)
                    text = raw.decode('utf-8', errors='replace')
                    assertion = re.search(r'FOREGROUND_ASSERT (\S+:\d+) ([^\n]+)', text)
                    legacy = re.search(r'(?:Assertion[^\n]*failed[^\n]*|[^\n]*Assertion[^\n]*failed\.|STARTUP FAIL [^\n]+|AssertionError: [^\n]+)', text)
                    location = re.search(r'([^\s:]+\.c):(\d+):', text)
                    apple_location = re.search(r'file ([^,]+), line (\d+)', text)
                    if not location:
                        location = apple_location
                    fallback_at = str(stream_path) + ':' + str(text[:legacy.start()].count('\n') + 1) if legacy else str(stream_path) + ':1'
                    if assertion or legacy:
                        record = {'case': case, 'status': 'fail',
                                  'at': assertion[1] if assertion else (location[1] + ':' + location[2] if location else fallback_at),
                                  'assert': (assertion[2] if assertion else legacy[0])[:240]}
                    elif timed_out:
                        record = {'case': case, 'status': 'invalid', 'reason': 'case timeout; process group killed; full partial output retained'}
                    elif len(raw) > 1024 * 1024:
                        record = {'case': case, 'status': 'invalid', 'reason': 'digest inspection limit exceeded; full evidence retained'}
                    elif result.returncode != 0:
                        record = {'case': case, 'status': 'invalid', 'reason': 'exit ' + str(result.returncode) + ' without trustworthy assertion; see case log'}
                    elif text.splitlines().count('FOREGROUND_CASE_OK ' + case) != 1:
                        record = {'case': case, 'status': 'invalid', 'reason': 'missing/duplicate terminal case marker'}
                    else:
                        record = {'case': case, 'status': 'pass'}
                    leaves = re.findall(r'^FOREGROUND_CONFIG_OK ' + re.escape(case) + r' (\S+)$', text, re.M)
                    expected = NESTED.get(case, [])
                    nested_observed[case] = {'planned': expected, 'observed': leaves,
                        'unobserved': [x for x in expected if x not in leaves]}
                    if leaves != expected:
                        nested_complete = False
                        if record['status'] == 'pass':
                            record = {'case':case, 'status':'invalid', 'reason':'nested case/configuration roster mismatch'}
                    if group == 'target' and record['status'] == 'pass':
                        nested = json.loads((raw_directory / 'target/configurations.json').read_text())
                        if nested != expected_nested():
                            record = {'case': case, 'status': 'invalid', 'reason': 'incomplete/duplicate target configuration roster'}
                    log.write(json.dumps({'case': case, 'exit': result.returncode,
                                          'output': str(stream_path), 'status': record['status']}) + '\n')
                    if timed_out or len(raw) > 1024 * 1024:
                        stopped = True  # retain a genuine assertion failure, but stop infrastructure work
                    # Consolidate full verbose output without placing it on stdout.
                    with stream_path.open() as source:
                        shutil.copyfileobj(source, log)
                except (OSError, subprocess.TimeoutExpired, ValueError) as exc:
                    if record is None or record['status'] != 'fail':
                        record = {'case': case, 'status': 'invalid', 'reason': (type(exc).__name__ + ': ' + str(exc)).replace('\n', ' ')[:240]}
                    stopped = True
                if group == 'target':
                    nested_path = raw_directory / 'target/configurations.json'
                    try:
                        nested = json.loads(nested_path.read_text())
                        nested_complete = nested == expected_nested()
                    except (OSError, ValueError):
                        nested_complete = False
                stopped = stopped or record['status'] != 'pass'
            observed.append(record)
            counts['pass_' if record['status'] == 'pass' else record['status']] += 1
            emit(record, digest)
            if not args.human and (not args.failures_only or record['status'] != 'pass'):
                emit(record, sys.stdout)
        summary = {'summary': {'target': 'foreground-' + label, 'kind': 'python',
                   'pass': counts['pass_'], 'fail': counts['fail'], 'invalid': counts['invalid'],
                   'report': str(report), 'log': str(logpath)}}
        rollup = {'rollup': {'pass': counts['pass_'], 'fail': counts['fail'],
                            'invalid': counts['invalid'], 'targets': 1}}
        emit(summary, digest); emit(rollup, digest)
        if not args.human:
            emit(summary, sys.stdout); emit(rollup, sys.stdout)
    for case in roster:
        if case in NESTED and case not in nested_observed:
            nested_observed[case] = {'planned':NESTED[case], 'observed':[],
                                     'unobserved':NESTED[case]}
    (raw_directory / 'roster.json').write_text(json.dumps({'planned': roster,
        'observed': observed, 'nested_configurations': nested_observed, 'complete': len(observed) == len(roster) and not counts['invalid'] and nested_complete,
        'target_configurations': str(raw_directory / 'target/configurations.json') if 'target' in [lookup[c] for c in roster] else None}, indent=2) + '\n')
    # This invocation owns one fresh run for its target. Retain it and all
    # historical reports/invalids; do not scan or delete prior or unrelated runs.
    if args.human:
        with logpath.open() as source:
            shutil.copyfileobj(source, sys.stdout)
        print('Report: ' + str(report))
    return 1 if counts['fail'] or counts['invalid'] else 0

if __name__ == '__main__':
    sys.exit(main())
