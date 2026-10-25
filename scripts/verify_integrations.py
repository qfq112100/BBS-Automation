"""Independently verify an isolated --verify-tools extraction, without executing any payload."""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import struct


def sha256(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def no_signature(path: Path) -> None:
    data = path.read_bytes()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    optional = pe + 24
    magic = struct.unpack_from("<H", data, optional)[0]
    directory = optional + (112 if magic == 0x20B else 96) + 4 * 8
    assert struct.unpack_from("<II", data, directory) == (0, 0), "LTK upstream signature not removed"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, default=Path(__file__).resolve().parents[1] / "resources/integrations/manifest.json")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    report = json.loads((args.profile / "verification.json").read_text(encoding="utf-8"))
    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    assert report["success"] and report["dry_run"] and report["applications_launched"] == 0
    states = {item["id"]: item for item in report["tools"]}
    totals = []
    for index, tool in enumerate(manifest["tools"]):
        state = states[index]
        assert state["ready"] and not state["running"] and not state["error"]
        directory = Path(state["directory"]).resolve()
        assert directory.is_relative_to(args.profile.resolve())
        paths = set()
        total = 0
        for item in tool["files"]:
            path = directory / item["path"]
            assert path.is_file() and not path.is_symlink(), f"Missing/linked file: {path}"
            assert path.stat().st_size == item["size"], f"Size mismatch: {path}"
            assert sha256(path) == item["sha256"].lower(), f"SHA256 mismatch: {path}"
            paths.add(path.relative_to(directory).as_posix())
            total += item["size"]
        physical = {path.relative_to(directory).as_posix() for path in directory.rglob("*") if path.is_file()}
        assert physical == paths | {".bbs-component.json"}, f"Unexpected extracted path set: {tool['id']}"
        cabinet = args.manifest.parent / (tool["id"] + ".cab")
        assert sha256(cabinet) == tool["cab_sha256"], "Source Cabinet digest mismatch"
        totals.append(dict(id=tool["id"], files=len(paths), bytes=total, all_sha256_match=True))
    rose = Path(states[0]["directory"]) / "_internal/injection/tools"
    ltk = Path(states[2]["directory"])
    expected = {"ltk_patcher_host.exe": "2d05f2a4224805e419fe6952214579d0fc469c52e718c7a6b1dd7d9729a98613",
                "ltk_patcher_dll.dll": "1fe703397b9222cdb41d3fe3198305d2236c97e4788b484544265c6af38ce55d"}
    for name, digest in expected.items():
        assert sha256(rose / name) == digest and sha256(ltk / name) == digest
        no_signature(rose / name)
    assert sha256(rose / "LTK-PATCHER-LICENSE.md") == sha256(ltk / "LTK-PATCHER-LICENSE.md")
    result = dict(success=True, applications_launched=0, tools=totals,
                  rose_contains_matching_ltk_pair=True, ltk_signatures_removed_per_license=True,
                  files_verified=sum(item["files"] for item in totals),
                  bytes_verified=sum(item["bytes"] for item in totals))
    text = json.dumps(result, ensure_ascii=False, indent=2)
    if args.output:
        args.output.write_text(text + chr(10), encoding="utf-8")
    print(text)


if __name__ == "__main__":
    main()
