#!/usr/bin/env python3
"""Package the complete BBS development tree without local runtime evidence/data.
No vendor program is executed, no user data is imported, no app is restarted.
"""
from __future__ import annotations
import argparse
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import zipfile

SKIP_DIRS = {'.git', '.venv', 'venv', '__pycache__', 'node_modules', 'build', '.build', 'dist', '.cache', '.idea', '.vscode'}
SKIP_SUFFIXES = {'.log', '.dmp', '.tmp', '.bak', '.pyc', '.pyo', '.db', '.sqlite', '.sqlite3', '.db-wal', '.db-shm', '.obj', '.o', '.pdb'}
SKIP_ROOT = {'setup.inf', 'setup.rpt', 'config.json', 'config.json.lock', 'tool-center.json', 'source_package_manifest.json'}
ALLOWED_TOP_DIRS = {'src', 'tests', 'scripts', 'cmake', 'third_party', 'resources', 'licenses', 'docs'}
STORED_SUFFIXES = {'.cab', '.png', '.jpg', '.jpeg', '.lbap', '.zip'}
MANIFEST_NAME = 'SOURCE_PACKAGE_MANIFEST.json'

def digest(path: Path) -> str:
    h = hashlib.sha256()
    with path.open('rb') as f:
        for data in iter(lambda: f.read(1024 * 1024), b''):
            h.update(data)
    return h.hexdigest()

def blocked_dir(rel: Path) -> bool:
    parts = [x.casefold() for x in rel.parts]
    if any(x in SKIP_DIRS or x.startswith(('build-', 'cmake-build-')) for x in parts):
        return True
    return len(parts) > 1 and parts[0] == 'docs' and parts[1].startswith('verification')

def reparse(path: Path) -> bool:
    return path.is_symlink() or bool(getattr(path.lstat(), 'st_file_attributes', 0) & getattr(stat, 'FILE_ATTRIBUTE_REPARSE_POINT', 0x400))

def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--source', type=Path, default=Path.cwd())
    ap.add_argument('--output', type=Path, required=True, help='New .zip outside source root; existing output is not overwritten.')
    ap.add_argument('--reference-exe', type=Path, help='Optional reference hash only; executable is NOT added to source archive.')
    args = ap.parse_args()
    source = args.source.resolve(strict=True)
    output = args.output.resolve()
    checksum = Path(str(output) + '.sha256.txt')
    partial = Path(str(output) + '.partial')
    if output.is_relative_to(source) or output.suffix.casefold() != '.zip':
        raise SystemExit('Output must be a .zip outside the source directory.')
    if any(x.exists() for x in (output, checksum, partial)):
        raise SystemExit('Output/checksum/partial already exists; choose a new name.')
    required = ['src/app/main.cpp', 'src/core/tool_manager.cpp', 'CMakeLists.txt', 'resources/app.rc',
                'resources/integrations/manifest.json', 'resources/integrations/rose.cab',
                'resources/integrations/akari.cab', 'resources/integrations/ltk.cab',
                'resources/integrations/abc.cab', 'build.ps1', 'run_tests.ps1',
                'third_party/json/json.hpp', 'README.md', 'toolchain.lock.json']
    if not all((source / name).is_file() for name in required):
        raise SystemExit('Not a complete BBS source tree.')
    cmake = (source / 'CMakeLists.txt').read_text(encoding='utf-8-sig')
    match = re.search(r'project\s*\([^)]*?\bVERSION\s+([\d.]+)', cmake, re.I | re.S)
    if not match:
        raise SystemExit('CMake project version missing.')
    version = match.group(1)
    root_name = 'BBS自动化源码包_' + version
    paths: list[Path] = []
    for base, dirs, files in os.walk(source, followlinks=False):
        base_path = Path(base)
        kept = []
        for name in sorted(dirs):
            p = base_path / name
            rel = p.relative_to(source)
            if reparse(p):
                raise SystemExit('Refusing reparse point: ' + str(rel))
            if len(rel.parts) == 1 and name.casefold() not in ALLOWED_TOP_DIRS:
                continue
            if not blocked_dir(rel):
                kept.append(name)
        dirs[:] = kept
        for name in sorted(files):
            p = base_path / name
            rel = p.relative_to(source)
            lower = name.casefold()
            if reparse(p):
                raise SystemExit('Refusing reparse file: ' + str(rel))
            if lower in SKIP_ROOT or p.suffix.casefold() in SKIP_SUFFIXES:
                continue
            if lower == '.env' or lower.startswith('.env.') or 'credential' in lower:
                continue
            if p.suffix.casefold() in {'.exe', '.dll'}:
                raise SystemExit('Unexpected loose binary; review before distributing: ' + str(rel))
            paths.append(p)
    paths.sort(key=lambda p: p.relative_to(source).as_posix().casefold())
    rels = {p.relative_to(source).as_posix() for p in paths}
    if not set(required).issubset(rels):
        raise SystemExit('Packaging filters removed required source/resource files.')
    entries = [{'path': p.relative_to(source).as_posix(), 'bytes': p.stat().st_size, 'sha256': digest(p)} for p in paths]
    manifest = {
        'schema': 1, 'version': version, 'archiveRoot': root_name,
        'createdAtUTC': dt.datetime.now(dt.timezone.utc).isoformat(),
        'scope': 'BBS source, build/test scripts, visual assets, prebuilt third-party CABs and notices; not complete vendor source.',
        'excluded': ['real runtime data and credentials', 'desktop/game/translation caches', 'docs/verification* local runtime evidence',
                     'MakeCAB setup.inf/setup.rpt', 'compiled output', 'external LLVM toolchain', 'standalone release EXE'],
        'validationScope': 'Archive completeness, CRC and per-file SHA checks only; no new app/game/UI tests.',
        'files': entries,
    }
    if args.reference_exe:
        ref = args.reference_exe.resolve(strict=True)
        manifest['referenceExecutable'] = {'name': ref.name, 'bytes': ref.stat().st_size, 'sha256': digest(ref),
                                          'included': False, 'deterministicRebuildClaimed': False}
    mp = source / MANIFEST_NAME
    if mp.exists():
        mp.read_text(encoding='utf-8-sig')
    mp.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    with zipfile.ZipFile(partial, 'x', allowZip64=True) as z:
        for p in [*paths, mp]:
            method = zipfile.ZIP_STORED if p.suffix.casefold() in STORED_SUFFIXES else zipfile.ZIP_DEFLATED
            z.write(p, root_name + '/' + p.relative_to(source).as_posix(), compress_type=method, compresslevel=6)
    with zipfile.ZipFile(partial) as z:
        if z.testzip() is not None:
            raise SystemExit('ZIP CRC verification failed; partial left for investigation.')
        if len(z.namelist()) != len(paths) + 1 or len(set(z.namelist())) != len(z.namelist()):
            raise SystemExit('ZIP file count/duplicate check failed.')
        for entry in entries:
            h = hashlib.sha256()
            with z.open(root_name + '/' + entry['path']) as stream:
                for chunk in iter(lambda: stream.read(1024 * 1024), b''):
                    h.update(chunk)
            if h.hexdigest() != entry['sha256']:
                raise SystemExit('Archive/source SHA mismatch: ' + entry['path'])
        if json.loads(z.read(root_name + '/' + MANIFEST_NAME)) != manifest:
            raise SystemExit('Archive manifest mismatch.')
    partial.replace(output)
    sha = digest(output)
    checksum.write_text(sha.upper() + '  ' + output.name + '\n', encoding='utf-8-sig')
    print(json.dumps({'version': version, 'archive': str(output), 'bytes': output.stat().st_size,
                      'sha256': sha, 'files': len(paths) + 1, 'crcPassed': True, 'allFileHashesPassed': True,
                      'manifest': str(mp), 'checksum': str(checksum)}, ensure_ascii=False))

if __name__ == '__main__':
    main()
