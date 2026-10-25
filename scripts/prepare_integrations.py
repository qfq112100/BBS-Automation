"""Pack pinned third-party directories into native Cabinets; never execute applications.

Normal BBS builds use the already generated resources/integrations files. This script
is for maintainers refreshing those binary inputs after license and digest review.
"""
from __future__ import annotations
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

DEFINITIONS = [
    dict(id="rose", name="Rose", version="1.4.9", entry="Rose.exe", cab_resource=3101,
         elevated=True, source="https://github.com/Alban1911/Rose/releases/tag/1.4.9",
         bindings=[dict(tool="ltk", **{"from": name}, to="_internal/injection/tools/" + name)
                   for name in ("ltk_patcher_host.exe", "ltk_patcher_dll.dll", "LTK-PATCHER-LICENSE.md")]),
    dict(id="akari", name="LeagueAkari", version="1.5.1", entry="LeagueAkari.exe", cab_resource=3102,
         elevated=False, source="https://github.com/LeagueAkari/LeagueAkari/releases/tag/v1.5.1"),
    dict(id="ltk", name="LTK Patcher", version="1.30.4", entry="", cab_resource=3103,
         elevated=False, source="https://github.com/LeagueToolkit/ltk-manager/tree/v1.30.4"),
]


def digest(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def add_file(files: dict[str, Path], name: str, path: Path) -> None:
    if not path.is_file():
        raise ValueError(f"Required payload/license absent: {path}")
    name = name.replace("\\", "/")
    if name.startswith("/") or ":" in name or any(p in ("", ".", "..") for p in name.split("/")):
        raise ValueError(f"Invalid relative payload path: {name}")
    if any(old.casefold() == name.casefold() for old in files):
        raise ValueError(f"Case-insensitive payload path collision: {name}")
    files[name] = path


def pack(definition: dict, inputs: Path, work: Path, output: Path) -> dict:
    key = definition["id"]
    base = inputs / key / ("runtime" if key == "ltk" else "payload")
    if not base.is_dir():
        raise ValueError(f"Prepared official payload required: {base}")
    files: dict[str, Path] = {}
    for path in sorted(base.rglob("*")):
        if path.is_symlink():
            raise ValueError(f"Refuse linked payload entry: {path}")
        if path.is_file():
            add_file(files, path.relative_to(base).as_posix(), path)
    if key == "rose":
        # Include the approved unsigned LTK pair directly inside the Rose cabinet.
        for name in ("ltk_patcher_host.exe", "ltk_patcher_dll.dll", "LTK-PATCHER-LICENSE.md"):
            add_file(files, "_internal/injection/tools/" + name, inputs / "ltk" / "runtime" / name)
        for name in ("Rose-MIT-LICENSE.txt", "PenguLoader-MIT-LICENSE.txt", "CPython-3.13.14-LICENSE.txt"):
            add_file(files, "BBS-THIRD-PARTY/" + name, inputs / key / "licenses" / name)
        add_file(files, "BBS-THIRD-PARTY/THIRD_PARTY_NOTICES.txt", inputs / key / "THIRD_PARTY_NOTICES.txt")
    if key == "akari" and "LeagueAkari-LICENSE.txt" not in files:
        raise ValueError("Akari copyright/permission notice must be retained")
    if key == "ltk" and "LTK-PATCHER-LICENSE.md" not in files:
        raise ValueError("Separate LTK binary redistribution license required")
    stage = work / key
    stage.mkdir(parents=True, exist_ok=True)
    archive_dir = stage / "cab"
    archive_dir.mkdir(exist_ok=True)
    if not str(stage).isascii():
        raise ValueError("Use an ASCII temporary staging directory for makecab")
    manifest = dict(definition)
    records = []
    ddf = [".OPTION EXPLICIT", ".Set Cabinet=ON", ".Set Compress=ON", ".Set CompressionType=LZX",
           ".Set CompressionMemory=21", ".Set MaxDiskSize=0", ".Set MaxCabinetSize=0",
           f'.Set CabinetNameTemplate={key}.cab', f'.Set DiskDirectoryTemplate="{archive_dir}"']
    for index, (relative, original) in enumerate(sorted(files.items())):
        token = f"f{index:06d}"
        staged = stage / token
        if staged.exists():
            staged.unlink()
        try:
            os.link(original, staged)
        except OSError:
            shutil.copy2(original, staged)
        records.append(dict(cab=token, path=relative, size=original.stat().st_size, sha256=digest(original)))
        ddf.append(f'"{staged}" "{token}"')
    ddf_path = stage / "payload.ddf"
    ddf_path.write_text("\n".join(ddf) + "\n", encoding="ascii")
    cabinet = archive_dir / (key + ".cab")
    if cabinet.exists():
        cabinet.unlink()
    with (stage / "makecab.log").open("wb") as log:
        subprocess.run(["makecab.exe", "/F", str(ddf_path)], check=True, stdout=log, stderr=subprocess.STDOUT)
    if not cabinet.is_file() or cabinet.stat().st_size < 36:
        raise RuntimeError(f"Cabinet missing after makecab: {cabinet}")
    shutil.copyfile(cabinet, output / cabinet.name)
    manifest["files"] = records
    manifest["total_bytes"] = sum(item["size"] for item in records)
    manifest["cab_sha256"] = digest(cabinet)
    manifest["cab_size"] = cabinet.stat().st_size
    print(json.dumps(dict(id=key, files=len(records), uncompressed=manifest["total_bytes"],
                          cabinet=manifest["cab_size"], sha256=manifest["cab_sha256"])), flush=True)
    return manifest


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--inputs", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--only", choices=[item["id"] for item in DEFINITIONS])
    parser.add_argument("--output", type=Path, default=Path(__file__).resolve().parents[1] / "resources/integrations")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    args.work.mkdir(parents=True, exist_ok=True)
    previous = {}
    manifest_path = args.output / "manifest.json"
    if args.only and manifest_path.is_file():
        previous = {item["id"]: item for item in json.loads(manifest_path.read_text(encoding="utf-8"))["tools"]}
    selected = [item for item in DEFINITIONS if not args.only or item["id"] == args.only]
    with ThreadPoolExecutor(max_workers=3) as executor:
        futures = [executor.submit(pack, spec, args.inputs, args.work, args.output) for spec in selected]
        for future in futures:
            item = future.result()
            previous[item["id"]] = item
    tools = [previous[item["id"]] for item in DEFINITIONS]
    document = dict(format=1, tools=tools)
    (args.output / "manifest.json").write_text(json.dumps(document, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print("Native CAB resources ready; runtime apps were not executed.", flush=True)


if __name__ == "__main__":
    main()
