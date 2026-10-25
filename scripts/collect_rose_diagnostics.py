"""Read-only maintainer diagnostics; never launch apps or declare skins successful.

Run with --not-before 2026-10-24T20:30:00+08:00 to exclude pre-candidate logs.
Filename local times are interpreted in the explicit cutoff's offset, if supplied.
"""
from __future__ import annotations
import argparse
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import re

LIMIT = 512 * 1024
EXPECTED = {
    'ltk_patcher_host.exe': '23fa1aaeda1a0c743da44227f179084e1fb81c7cd262144a3c8604ffc49d3bc1',
    'ltk_patcher_dll.dll': '6d419057e6667994ba752ad0fb089b363db98267618644d7f7b6632441a21d74',
}

def filename_time(path: Path):
    try:
        return dt.datetime.strptime(path.stem.removeprefix('rose_runoverlay_'), '%d-%m-%Y_%H-%M-%S')
    except ValueError:
        return None

def classify(text: str, truncated: bool = False):
    lines = text.splitlines()
    starts = [i for i, s in enumerate(lines) if re.search(r'game found.*hook installed|hook install failed', s, re.I)]
    if starts:
        lines = lines[starts[-1]:]
    segment = '\n'.join(lines)
    rejected = bool(re.search(r'host verification failed|refusing overlay|disabling overlay', segment, re.I))
    hook_failed = bool(re.search(r'SetWindowsHookEx failed|hook install failed', segment, re.I))
    attached = bool(re.search(r'\bdll attached\b', segment, re.I))
    verified = bool(re.search(r'\boverlay verified\b', segment, re.I))
    redirected = bool(re.search(r'\bredirect(?:ed|ing|s)?\b[^\r\n]*\.wad(?:\.client)?\b', segment, re.I))
    other_error = bool(re.search(r'\bERROR\b', segment))
    if rejected:
        state = 'overlay_rejected'
    elif hook_failed:
        state = 'hook_failed'
    elif other_error:
        state = 'session_error'
    elif truncated:
        state = 'partial_log_manual_review'
    elif attached and verified and redirected:
        state = 'overlay_evidence_present'
    elif verified:
        state = 'verified_without_redirect_evidence'
    elif redirected:
        state = 'redirects_without_complete_verification'
    elif attached:
        state = 'attached_only'
    else:
        state = 'no_attachment_evidence'
    interesting = [s for s in lines if re.search(r'hook installed|dll attached|overlay verified|redirect|refusing overlay|verification failed|SetWindowsHookEx failed|\bERROR\b', s, re.I)]
    return dict(state=state, hook_failed=hook_failed, dll_attached=attached,
                overlay_rejected=rejected, overlay_verified_marker=verified,
                wad_redirect_marker=redirected, log_truncated=truncated,
                session_ended=bool(re.search(r'dll detached|host stopped', segment, re.I)),
                evidence_lines=interesting[-12:], human_visual_confirmed=False,
                full_goal_verified=False)

def runtime_status(folder: Path, expected=None):
    expected = EXPECTED if expected is None else expected
    result = {}
    for name, pin in expected.items():
        path = folder / name
        try:
            h = hashlib.sha256()
            with path.open('rb') as source:
                for block in iter(lambda: source.read(1024 * 1024), b''):
                    h.update(block)
            result[name] = dict(sha256=h.hexdigest(), matches=h.hexdigest() == pin)
        except OSError as exc:
            result[name] = dict(matches=False, error=type(exc).__name__)
    return dict(files=result, all_match=all(x['matches'] for x in result.values()))

def collect(logs: Path, runtime: Path, not_before=None):
    report = dict(read_only=True, human_visual_confirmed=False, full_goal_verified=False,
                  runtime=runtime_status(runtime), session_origin_verified=False)
    dated = [(filename_time(p), p) for p in logs.glob('rose_runoverlay_*.log')]
    dated = [(when, path) for when, path in dated if when is not None]
    if not dated:
        return dict(report, state='no_session', reason='No runoverlay log exists')
    when, path = max(dated, key=lambda pair: pair[0])
    report.update(log=str(path), filename_time=when.isoformat(), not_before=str(not_before) if not_before else None)
    if not_before is not None:
        when = when.replace(tzinfo=not_before.tzinfo)
        if when < not_before:
            return dict(report, state='historical_only', reason='Newest log predates explicit cutoff')
    size = path.stat().st_size
    with path.open('rb') as source:
        source.seek(max(0, size - LIMIT))
        text = source.read(LIMIT).decode('utf-8-sig', errors='replace')
    report.update(classify(text, size > LIMIT))
    report['session_origin_note'] = 'Current file hashes do not prove which runtime generated a past log; correlate Rose startup path and user session.'
    return report

def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    local = Path(os.environ.get('LOCALAPPDATA', '.'))
    parser.add_argument('--logs-dir', type=Path, default=local / 'Rose' / 'logs')
    parser.add_argument('--runtime-tools', type=Path, default=local / 'LOLBackupV3' / 'components' / 'rose' / '1.4.9-ltk1.29.0' / '_internal' / 'injection' / 'tools')
    parser.add_argument('--not-before', type=dt.datetime.fromisoformat)
    args = parser.parse_args(argv)
    print(json.dumps(collect(args.logs_dir, args.runtime_tools, args.not_before), ensure_ascii=False, indent=2))

if __name__ == '__main__':
    main()
