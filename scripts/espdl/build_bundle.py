#!/usr/bin/env python3
"""Build a local model bundle from a reviewed manifest and local assets."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "examples/python"))
from model_bundle import MANIFEST_MAX, relative_file, validate_manifest


def build(manifest_path, assets, output):
    text = manifest_path.read_bytes()
    if len(text) > MANIFEST_MAX:
        raise ValueError("manifest exceeds 64 KiB")
    manifest = json.loads(text)
    validate_manifest(manifest, str(assets))
    files = dict(manifest.get("assets", {}))
    files[manifest["model"]["file"]] = manifest["model"]["sha256"]
    if "bundle.json" in files:
        raise ValueError("asset collides with bundle.json")
    for name, expected in files.items():
        path = Path(relative_file(str(assets), name))
        with path.open("rb") as source:
            digest = hashlib.file_digest(source, "sha256").hexdigest()
        if digest != expected:
            raise ValueError("SHA-256 mismatch: " + name)
    if output.exists():
        raise FileExistsError("output already exists: " + str(output))
    output.parent.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix=".model-bundle-", dir=output.parent))
    try:
        for name in files:
            target = staging / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(Path(relative_file(str(assets), name)), target)
        (staging / "bundle.json").write_text(json.dumps(manifest, indent=2) + "\n")
        staging.rename(output)
    finally:
        if staging.exists():
            shutil.rmtree(staging)
    return manifest["id"]


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("--assets", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    print(build(args.manifest, args.assets, args.output))
