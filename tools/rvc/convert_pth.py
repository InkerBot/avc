#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import struct
import sys
from pathlib import Path
from typing import Any

def progress(value: float, message: str) -> None:
    print(json.dumps({"progress": value, "message": message}), flush=True)

def fail(message: str) -> "NoReturn":  # type: ignore[name-defined]
    raise SystemExit(message)

def digest(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            value.update(chunk)
    return value.hexdigest()

def sample_rate_of(value: Any) -> int:
    if isinstance(value, str):
        rates = {"32k": 32000, "40k": 40000, "48k": 48000}
        if value.lower() in rates:
            return rates[value.lower()]
    try:
        rate = int(value)
    except (TypeError, ValueError):
        fail(f"unsupported checkpoint sample rate: {value!r}")
    if rate not in (32000, 40000, 48000):
        fail(f"unsupported checkpoint sample rate: {rate}")
    return rate

def link_shared(source: Path, destination: Path) -> None:
    if not source.is_file():
        fail(f"shared model is missing: {source}")
    try:
        destination.symlink_to(source.resolve())
    except OSError:
        # Some packaged or restricted filesystems do not permit symlinks. A
        # hardlink avoids duplicating hundreds of MiB when both live together;
        # copying is the portable last resort.
        try:
            os.link(source, destination)
        except OSError:
            shutil.copy2(source, destination)

def convert_feature_index(source: Path, destination: Path, expected_dimension: int) -> dict[str, int]:
    import faiss
    import numpy as np

    try:
        original = faiss.read_index(str(source))
        index = faiss.downcast_index(original)
        ivf = faiss.extract_index_ivf(index)
    except Exception as exc:
        fail(f"cannot read FAISS feature index: {exc}")

    dimension = int(index.d)
    nlist = int(ivf.nlist)
    ntotal = int(ivf.ntotal)
    code_size = int(ivf.code_size)
    if int(index.metric_type) != int(faiss.METRIC_L2):
        fail("feature index must use squared L2 distance")
    if dimension != expected_dimension:
        fail(f"feature index dimension {dimension} does not match RVC {expected_dimension}")
    if code_size != dimension * 4:
        fail("only an uncompressed FAISS IndexIVFFlat feature index is supported")
    if nlist < 1 or nlist > 1_000_000 or ntotal < 1:
        fail("feature index has invalid list or vector counts")

    try:
        centroids = np.asarray(
            ivf.quantizer.reconstruct_n(0, nlist), dtype="<f4", order="C"
        ).reshape(nlist, dimension)
    except Exception as exc:
        fail(f"cannot reconstruct feature-index centroids: {exc}")

    list_sizes = [int(ivf.invlists.list_size(number)) for number in range(nlist)]
    if sum(list_sizes) != ntotal:
        fail("feature index inverted-list sizes do not match ntotal")
    offsets = np.empty(nlist + 1, dtype="<u8")
    offsets[0] = 0
    np.cumsum(np.asarray(list_sizes, dtype=np.uint64), out=offsets[1:])

    header_size = 72
    centroids_offset = header_size
    offsets_offset = centroids_offset + centroids.nbytes
    vectors_offset = offsets_offset + offsets.nbytes
    file_size = vectors_offset + ntotal * dimension * 4
    nprobe = max(1, min(int(ivf.nprobe), 32, nlist))
    header = struct.pack(
        "<8sIIIIIIQQQQQ",
        b"AVCRIDX\0",
        1,  # format version
        1,  # squared L2
        dimension,
        nlist,
        nprobe,
        0,
        ntotal,
        centroids_offset,
        offsets_offset,
        vectors_offset,
        file_size,
    )

    try:
        with destination.open("wb") as output:
            output.write(header)
            output.write(centroids.tobytes(order="C"))
            output.write(offsets.tobytes(order="C"))
            for number, size in enumerate(list_sizes):
                if size == 0:
                    continue
                codes = ivf.invlists.get_codes(number)
                try:
                    raw = faiss.rev_swig_ptr(codes, size * code_size)
                    output.write(memoryview(raw))
                finally:
                    ivf.invlists.release_codes(number, codes)
        if destination.stat().st_size != file_size:
            fail("converted feature index has an unexpected size")
    except SystemExit:
        raise
    except Exception as exc:
        fail(f"cannot write converted feature index: {exc}")
    return {"dimension": dimension, "nlist": nlist, "nprobe": nprobe, "vectors": ntotal}

def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--name", required=True)
    parser.add_argument("--contentvec-v1", type=Path, required=True)
    parser.add_argument("--contentvec-v2", type=Path, required=True)
    parser.add_argument("--rmvpe", type=Path, required=True)
    parser.add_argument("--index", type=Path)
    args = parser.parse_args()

    # Imports happen after argument parsing so a missing bundled dependency is
    # reported as a converter failure rather than looking like an AVC crash.
    progress(0.10, "loading conversion runtime")
    import onnx
    import torch
    from rvc.lib.infer_pack.models_onnx import SynthesizerTrnMsNSFsidM

    # CVE-2026-24747 affects the weights-only unpickler through 2.9.1.
    # Refuse a developer override that silently restores a known-vulnerable
    # loader; the bundled runtime is pinned above this floor.
    try:
        torch_version = tuple(int(part) for part in torch.__version__.split("+", 1)[0].split(".")[:2])
    except ValueError:
        fail(f"cannot validate PyTorch version: {torch.__version__}")
    if torch_version < (2, 10):
        fail("PyTorch 2.10 or newer is required for secure weights-only checkpoint loading")

    torch.set_num_threads(1)
    torch.manual_seed(114514)

    progress(0.18, "reading checkpoint safely")
    try:
        checkpoint = torch.load(args.input, map_location="cpu", weights_only=True)
    except Exception as exc:
        fail(
            "checkpoint cannot be read with PyTorch's safe weights-only loader; "
            f"use an RVC inference .pth rather than a training checkpoint ({exc})"
        )

    if not isinstance(checkpoint, dict):
        fail("checkpoint root must be a dictionary")
    config = checkpoint.get("config")
    weights = checkpoint.get("weight")
    if not isinstance(config, (list, tuple)) or len(config) < 18:
        fail("checkpoint does not contain a valid RVC inference config")
    if not isinstance(weights, dict) or "emb_g.weight" not in weights:
        fail("checkpoint does not contain RVC inference weights")

    version = str(checkpoint.get("version", "v1")).lower()
    if version not in ("v1", "v2"):
        fail(f"unsupported RVC version: {version}")
    uses_f0 = bool(checkpoint.get("f0", 1))
    if not uses_f0:
        fail("non-F0 RVC checkpoints are not supported by the current native runtime")

    config = list(config)
    speakers = int(weights["emb_g.weight"].shape[0])
    if speakers < 1 or speakers > 4096:
        fail(f"invalid speaker count: {speakers}")
    config[-3] = speakers
    sample_rate = sample_rate_of(config[-1])
    content_dim = 256 if version == "v1" else 768

    progress(0.28, f"building RVC {version} network")
    model = SynthesizerTrnMsNSFsidM(
        *config, is_half=False, version=version
    )
    incompatible = model.load_state_dict(weights, strict=False)
    # Deployment checkpoints intentionally omit enc_q, the training-only
    # posterior encoder. Every part reached by forward() must still be present.
    missing_runtime = [key for key in incompatible.missing_keys if not key.startswith("enc_q.")]
    if missing_runtime:
        fail("checkpoint is missing network weights: " + ", ".join(missing_runtime[:8]))
    model.eval()

    args.output.mkdir(parents=True, exist_ok=False)
    synthesizer = args.output / "synthesizer.onnx"
    # The legacy RVC ONNX attention implementation converts a few shape values
    # to Python integers while tracing, so advertising a dynamic time axis
    # produces a graph that passes onnx.checker but fails for other lengths.
    # The legacy ONNX attention code cannot honestly export a dynamic time
    # axis. A roughly 512 ms rolling input window gives ContentVec 25 semantic
    # frames, repeated to 50 RVC phone frames. The native runtime keeps this
    # window across graph blocks and emits only the newest output segment.
    content_samples_16k = 8192
    frames = 50
    inputs = (
        torch.rand(1, frames, content_dim),
        torch.tensor([frames], dtype=torch.long),
        torch.randint(5, 255, (1, frames), dtype=torch.long),
        torch.rand(1, frames),
        torch.tensor([0], dtype=torch.long),
        torch.rand(1, 192, frames),
    )

    progress(0.42, "exporting synthesizer to ONNX")
    with torch.inference_mode():
        torch.onnx.export(
            model,
            inputs,
            synthesizer,
            # RVC's ONNX-specific model was written for the established JIT
            # exporter. The newer dynamo exporter requires onnxscript and does
            # not improve this intentionally fixed input signature.
            dynamo=False,
            do_constant_folding=False,
            opset_version=13,
            input_names=["phone", "phone_lengths", "pitch", "pitchf", "ds", "rnd"],
            output_names=["audio"],
        )

    progress(0.78, "validating exported ONNX graph")
    graph = onnx.load(synthesizer, load_external_data=True)
    onnx.checker.check_model(graph, full_check=True)
    input_names = [item.name for item in graph.graph.input]
    expected = ["phone", "phone_lengths", "pitch", "pitchf", "ds", "rnd"]
    if input_names != expected:
        fail(f"exported synthesizer input signature is incompatible: {input_names}")

    contentvec = args.contentvec_v1 if version == "v1" else args.contentvec_v2
    link_shared(contentvec, args.output / "contentvec.onnx")
    link_shared(args.rmvpe, args.output / "rmvpe.onnx")

    feature_index = None
    if args.index is not None:
        progress(0.84, "converting FAISS feature index")
        feature_index = convert_feature_index(
            args.index, args.output / "feature_index.avcidx", content_dim
        )

    progress(0.92, "writing AVC model manifest")
    manifest = {
        "format_version": 1,
        "name": args.name,
        "rvc_version": version,
        "content_dim": content_dim,
        "model_sample_rate": sample_rate,
        "synthesizer_frames": frames,
        "content_samples_16k": content_samples_16k,
        "uses_f0": True,
        "speakers": speakers,
        "source": {
            "format": "rvc-pth",
            "sha256": digest(args.input),
            "safe_weights_only": True,
        },
        "files": {
            "synthesizer.onnx": f"sha256:{digest(synthesizer)}",
            "contentvec.onnx": "shared",
            "rmvpe.onnx": "shared",
        },
    }
    if feature_index is not None:
        manifest["feature_index"] = "feature_index.avcidx"
        manifest["retrieval"] = feature_index
        manifest["source"]["index_sha256"] = digest(args.index)
        manifest["files"]["feature_index.avcidx"] = (
            f"sha256:{digest(args.output / 'feature_index.avcidx')}"
        )
    (args.output / "manifest.json").write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    progress(0.95, "conversion complete")

if __name__ == "__main__":
    main()
