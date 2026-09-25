"""Create and verify a Git-tree-bound source ZIP that needs no .git to build."""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
import pathlib
import re
import subprocess
import zipfile


ROOT = pathlib.Path(__file__).resolve().parents[1]
MANIFEST = "provenance/source-snapshot-manifest.json"
GENERATED = {"build", "dist", "bin", "obj", ".venv", "__pycache__", ".pytest_cache"}


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def git_object(kind: str, data: bytes) -> str:
    return hashlib.sha1(f"{kind} {len(data)}\0".encode() + data).hexdigest()


def tree_identity(files: list[dict[str, object]]) -> str:
    root: dict[str, object] = {}
    for item in files:
        parts = str(item["path"]).split("/")
        node = root
        for part in parts[:-1]:
            node = node.setdefault(part, {})  # type: ignore[assignment]
        if parts[-1] in node:
            raise ValueError("duplicate source path")
        node[parts[-1]] = (item["mode"], item["git_blob"])

    def hash_tree(node: dict[str, object]) -> str:
        entries = []
        for name, value in sorted(node.items(), key=lambda pair: pair[0] + ("/" if isinstance(pair[1], dict) else "")):
            if isinstance(value, dict):
                mode, child = "40000", hash_tree(value)
            else:
                mode, child = value
            entries.append(f"{mode} {name}".encode("utf-8") + b"\0" + bytes.fromhex(child))
        return git_object("tree", b"".join(entries))

    return hash_tree(root)


def verify(root: pathlib.Path) -> dict[str, object]:
    if (root / ".git").exists():
        raise ValueError("snapshot must not contain .git")
    manifest_path = root / MANIFEST
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if manifest.get("schema_version") != "xcp-source-snapshot-v1":
        raise ValueError("unknown source snapshot schema")
    if not isinstance(manifest.get("producer_claimed_commit"), str) or not re.fullmatch(
            r"[0-9a-f]{40}", manifest["producer_claimed_commit"]):
        raise ValueError("invalid producer commit assertion")
    files = manifest["files"]
    expected = {item["path"] for item in files}
    if len(expected) != len(files) or MANIFEST in expected or [item["path"] for item in files] != sorted(expected):
        raise ValueError("duplicate or self-referential source manifest")
    for item in files:
        path = pathlib.PurePosixPath(item["path"])
        if (path.is_absolute() or "\\" in item["path"] or ":" in item["path"] or
                any(part in ("", ".", "..") for part in item["path"].split("/")) or
                item["mode"] not in {"100644", "100755"}):
            raise ValueError("invalid source snapshot path or mode")
        source = root.joinpath(*path.parts)
        if source.is_symlink() or not source.is_file() or not source.resolve().is_relative_to(root.resolve()):
            raise ValueError("missing or linked source snapshot file")
        data = source.read_bytes()
        if (len(data) != item["bytes"] or sha256(data) != item["sha256"] or
                git_object("blob", data) != item["git_blob"]):
            raise ValueError(f"source snapshot file differs: {item['path']}")
    actual = set()
    for current, dirs, names in os.walk(root):
        dirs[:] = [name for name in dirs if name not in GENERATED and not name.endswith(".egg-info")]
        for name in names:
            actual.add((pathlib.Path(current) / name).relative_to(root).as_posix())
    if actual != expected | {MANIFEST}:
        raise ValueError("source snapshot file set differs")
    if tree_identity(files) != manifest["git_tree"]:
        raise ValueError("source snapshot Git tree differs")
    return manifest


def create(archive: pathlib.Path) -> dict[str, object]:
    if archive.exists() or not archive.parent.is_dir():
        raise ValueError("source archive must have a fresh path and existing parent")
    def git(*args: str) -> bytes:
        return subprocess.run(["git", *args], cwd=ROOT, check=True, capture_output=True).stdout
    if git("status", "--porcelain", "--untracked-files=all").strip():
        raise ValueError("source snapshot requires a clean checkout")
    commit = git("rev-parse", "HEAD").decode().strip()
    tree = git("rev-parse", "HEAD^{tree}").decode().strip()
    staged = {}
    for entry in git("ls-tree", "-r", "-z", "HEAD").split(b"\0"):
        if entry:
            metadata, raw_path = entry.split(b"\t", 1)
            mode, kind, blob = metadata.decode().split()
            if kind != "blob" or mode not in {"100644", "100755"}:
                raise ValueError("source snapshot contains a linked or unsupported Git entry")
            staged[raw_path.decode("utf-8")] = (mode, blob)
    requested = sorted(staged)
    stream = io.BytesIO(subprocess.run(["git", "cat-file", "--batch"], cwd=ROOT, check=True,
                                       input=("\n".join(staged[name][1] for name in requested) + "\n").encode(),
                                       capture_output=True).stdout)
    files = []
    content = {}
    for name in requested:
        mode, blob = staged[name]
        header = stream.readline().decode().strip().split()
        if len(header) != 3 or header[:2] != [blob, "blob"]:
            raise ValueError(f"Git object stream differs: {name}")
        data = stream.read(int(header[2]))
        if stream.read(1) != b"\n":
            raise ValueError("Git object stream separator differs")
        if git_object("blob", data) != blob:
            raise ValueError(f"Git blob differs: {name}")
        files.append({"path": name, "mode": mode, "bytes": len(data),
                      "sha256": sha256(data), "git_blob": blob})
        content[name] = data
    if tree_identity(files) != tree:
        raise ValueError("Git archive tree differs")
    manifest = {"schema_version": "xcp-source-snapshot-v1", "producer_claimed_commit": commit,
                "git_tree": tree, "files": files,
                "commit_identity_note": "Commit is producer asserted; Git tree is recomputed from snapshot bytes."}
    content[MANIFEST] = (json.dumps(manifest, sort_keys=True, separators=(",", ":")) + "\n").encode()
    with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=6) as zipped:
        for name in sorted(content):
            item = zipfile.ZipInfo(name, date_time=(2026, 1, 1, 0, 0, 0))
            item.compress_type = zipfile.ZIP_DEFLATED
            zipped.writestr(item, content[name])
    return {"archive_sha256": sha256(archive.read_bytes()), "git_tree": tree, "source_files": len(files)}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=pathlib.Path)
    parser.add_argument("--verify-root", type=pathlib.Path)
    args = parser.parse_args()
    if bool(args.archive) == bool(args.verify_root):
        parser.error("choose exactly one of --archive or --verify-root")
    try:
        if args.archive:
            result = create(args.archive)
        else:
            manifest = verify(args.verify_root)
            result = {"git_tree": manifest["git_tree"], "source_files": len(manifest["files"]),
                      "commit_origin": "producer_assertion"}
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError, zipfile.BadZipFile) as exc:
        print(json.dumps({"ok": False, "error": str(exc)}))
        return 2
    print(json.dumps({"ok": True, "result": result}, default=str))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
