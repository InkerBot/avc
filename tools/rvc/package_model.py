#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
import shutil
from pathlib import Path

def digest(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            value.update(chunk)
    return value.hexdigest()

def copy_model(source: Path, destination: Path) -> str:
    if not source.is_file():
        raise SystemExit(f"model does not exist: {source}")
    shutil.copy2(source, destination)
    return f"sha256:{digest(destination)}"

def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--contentvec", type=Path, required=True)
    parser.add_argument("--synthesizer", type=Path, required=True)
    parser.add_argument("--rmvpe", type=Path)
    parser.add_argument("--name", default="")
    parser.add_argument("--rvc-version", choices=("v1", "v2"), required=True)
    parser.add_argument("--sample-rate", type=int, required=True)
    args = parser.parse_args()

    uses_f0 = args.rmvpe is not None
    args.output.mkdir(parents=True, exist_ok=True)
    files = {
        "contentvec.onnx": copy_model(args.contentvec, args.output / "contentvec.onnx"),
        "synthesizer.onnx": copy_model(args.synthesizer, args.output / "synthesizer.onnx"),
    }
    if args.rmvpe is not None:
        files["rmvpe.onnx"] = copy_model(args.rmvpe, args.output / "rmvpe.onnx")

    manifest = {
        "format_version": 1,
        "name": args.name or args.output.stem,
        "rvc_version": args.rvc_version,
        "content_dim": 256 if args.rvc_version == "v1" else 768,
        "model_sample_rate": args.sample_rate,
        "uses_f0": uses_f0,
        "files": files,
    }
    (args.output / "manifest.json").write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )

if __name__ == "__main__":
    main()
