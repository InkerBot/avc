# AVC native RVC extension (Linux and Windows)

This extension runs an RVC deployment package directly through ONNX Runtime's
C++ API. Inference never loads or spawns Python. A self-contained, daemon-side
importer is bundled by default so an end user can select an RVC inference
`.pth` checkpoint and its optional `.index` in the extension settings without installing Python,
PyTorch, ONNX tools, or shared base models on the machine. The x86-64 install
footprint is currently about 2 GiB, dominated by the bundled CPU PyTorch and
its export dependencies.

By default CMake downloads the pinned ONNX Runtime package, a standalone
CPython distribution, the conversion dependencies, the pinned RVC exporter
sources, ContentVec v1/v2 and RMVPE. Direct archives/models have fixed SHA-256
digests, Python packages have pinned versions, and the RVC source is fixed to a
commit. Downloads stay in the build cache and conversion itself is offline:

```sh
cmake -S . -B build-rvc -DAVC_BUILD_RVC_EXT=ON
cmake --build build-rvc --target avc_rvc
```

On Windows use the normal Visual Studio generator and configuration, for
example `cmake --build build-rvc --config RelWithDebInfo --target avc_rvc`.
The pinned archives include native x64 Windows builds of ONNX Runtime and the
self-contained CPython converter. The installed extension and ONNX Runtime
DLLs are placed together under `bin/extensions`.

The download is cached in the build directory. For an offline build or a
private ONNX Runtime build, add
`-DONNXRUNTIME_ROOT=/opt/onnxruntime-linux-x64` (or an extracted Windows x64
package). CUDA selects the official platform GPU archive automatically:

```sh
cmake -S . -B build-rvc -DAVC_BUILD_RVC_EXT=ON -DAVC_RVC_ENABLE_CUDA=ON
```

Add `-DAVC_RVC_ONNXRUNTIME_CUDA13=ON` to select the CUDA 13 archive; otherwise
the standard GPU archive is used.

`cmake --install build-rvc --component rvc` installs the extension, ONNX
Runtime libraries, converter runtime and shared models. Set
`AVC_RVC_BUNDLE_ONNXRUNTIME=OFF` only when ONNX Runtime is provided by the
system or another package. Set `AVC_RVC_BUNDLE_CONVERTER=OFF` for a deployment
that accepts prebuilt `.avcrvc` packages only.

## Importing a checkpoint

Open the RVC extension settings in the AVC editor, select an RVC inference
`.pth`, optionally select the voice's `.index`, enter a model name, and choose
**转换并导入**. The browser streams both files to the loopback-only
daemon endpoint; they are never loaded into browser JavaScript or daemon memory
as one large buffer. The settings UI shows conversion progress and supports
cancellation and model deletion.

The converter runs in a separate process. Linux applies no-new-privileges,
CPU/file limits and a best-effort network namespace; Windows uses a Job Object
with a CPU-time limit, memory limit and kill-on-close containment. Checkpoints are opened only with
PyTorch 2.12.1's restricted `weights_only=True` loader; the converter refuses
PyTorch versions below 2.10 because they include known weights-only loader
vulnerabilities. A checkpoint that needs arbitrary Python pickle objects is
rejected. Only RVC v1/v2 inference
checkpoints with F0 are currently accepted. Uploads are limited to 512 MiB and
optional indexes to 2 GiB; only one conversion may run at once.

Feature retrieval accepts the standard squared-L2 FAISS `IndexIVFFlat` used by
RVC. During import the bundled `faiss-cpu` reader converts it to an AVC-owned,
read-only IVF-flat store and discards FAISS IDs and metadata. At inference the
C++ node memory-maps that store, probes the index's configured coarse lists,
selects eight neighbours, applies RVC's inverse-square weighting, and blends
the result before the normal two-times feature expansion. Consequently neither
Python nor FAISS is loaded by audio inference. The node's `index_rate` parameter
controls the blend from `0` (off) to `1` (retrieved features only) and defaults
to `0.75`; packages without an index simply ignore it. Compressed, inner-product,
HNSW-only and other FAISS index variants are rejected during import.

The native editor module, HTTP import API and converter supervisor live in the
extension/daemon process. The large PyTorch runtime is used only while
converting; it is not part of audio inference.

The importer traces a fixed 50-frame synthesizer window and records
`content_samples_16k: 8192` in the package. Upstream RVC's legacy ONNX
attention exporter traces some time-shape calculations as constants, so the
runtime maintains a rolling 512 ms context and emits only the newest graph
quantum. Packages produced by the earlier 8-frame exporter are rejected with a
re-import instruction because that 80 ms context is not sufficient for stable
RVC inference.

For development, `AVC_RVC_CONVERTER_ROOT=/path/to/rvc-converter` overrides the
built-in converter location.

## Runtime package

The node must be placed in a cold domain. A package is a directory ending in
`.avcrvc` containing `manifest.json`, `contentvec.onnx`, `synthesizer.onnx`,
and, when `uses_f0` is true, `rmvpe.onnx`. Indexed packages additionally contain
`feature_index.avcidx`, referenced by the manifest's `feature_index` field.

The native pipeline implements ContentVec, RMVPE and synthesizer execution,
stream resampling, the 128-band RMVPE log-mel frontend and pitch decode, rolling
context/output, cross-block overlap, and optional native feature retrieval.
The synthesizer latent is deterministic standard-normal noise, matching the
RVC inference contract while avoiding stochastic seams between graph quanta.

CPU inference uses ONNX Runtime's normal intra-op worker pool. At 48 kHz, use a
cold RVC domain block of at least 24000 frames or use the CUDA build. A
4096-frame domain asks the 512 ms rolling model to run every
85 ms and will underrun on CPU; the node reports this configuration as degraded
instead of presenting it as a healthy model. The half-second CPU block adds
about one second of round-trip cold-domain latency at safety 1, but does not
require Python at runtime. CUDA users can lower the block after checking that
the domain remains free of underruns.

The older packaging helper remains available when all ONNX files have already
been exported. It requires Python only on that packaging machine:

```sh
python3 tools/rvc/package_model.py voice.avcrvc \
  --contentvec vec-768-layer-12.onnx \
  --rmvpe rmvpe.onnx \
  --synthesizer voice.onnx \
  --rvc-version v2 --sample-rate 40000
```
