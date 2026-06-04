"""
TensorRT engine deploy-smoke for ai_worker_v2.

Walks a models/ directory and, for each YOLO-shape `*.engine`, loads it
via Ultralytics + runs one forward pass on a synthetic 640x640x3 zero
tensor. Catches the deploy-time failure modes that the standalone PPE
smoke (tools/ppe_standalone_test.py) covered for ONE engine and one
real snapshot — generalized for multiple engines and CI-friendly
synthetic input.

Failure modes the smoke catches:
  - Engine binary corrupt or truncated (copy fail mid-deploy)
  - TensorRT API version mismatch (engine built for TRT 8.x, runtime 10.x)
  - GPU compute capability mismatch (engine built for sm_75, GPU is sm_86)
  - Wrong precision flag (FP16 engine on GPU without FP16 cores)
  - YAML/metadata.json sibling file missing — Ultralytics needs it to
    reconstruct the preprocessing pipeline

NOT covered (intentional — those need real data + larger deps):
  - Inference correctness / class-label drift (see ppe_standalone_test.py)
  - SCRFD / ArcFace / OSNet engines — they're not YOLO-shape, need raw
    TensorRT Python bindings. Defer; YOLO engines are the bulk of
    re-training churn anyway.

Usage:
  python test_engines_smoke.py                     # ./models in repo
  python test_engines_smoke.py path/to/models      # explicit dir

Exit code:
  0 — all discovered engines passed, OR no engines found (CI / fresh box)
  1 — at least one engine failed to load or forward
"""
from __future__ import annotations

import sys
import time
from pathlib import Path


# YOLO-shape engines we know how to validate via Ultralytics. Add new
# entries here as future ai_worker_v2 model bundles roll out. Names
# match the ai_worker_v2 resolveModelPath() defaults.
YOLO_ENGINES = ("yolo11m.engine", "PPE-YOLOv8m.engine")

# Engines we KNOW are not YOLO-shape — skip them with a clean message
# rather than failing on Ultralytics' confusing "unrecognized model"
# error. Adjust if a future bundle ships a YOLO-shape variant.
NON_YOLO_ENGINES = ("scrfd", "arcface", "osnet")


def discover_engines(models_dir: Path) -> list[Path]:
    if not models_dir.is_dir():
        return []
    return sorted(p for p in models_dir.iterdir() if p.suffix == ".engine")


def classify(engine_path: Path) -> str:
    name = engine_path.name
    if name in YOLO_ENGINES:
        return "yolo"
    if any(tag in name.lower() for tag in NON_YOLO_ENGINES):
        return "non_yolo_skip"
    # Unknown — try YOLO load anyway; if it fails we surface the error
    # so operators add the engine to the skip list intentionally.
    return "yolo_guess"


def smoke_yolo_engine(engine_path: Path) -> tuple[bool, str]:
    try:
        import numpy as np
        from ultralytics import YOLO
    except ImportError as e:
        return False, f"missing python deps: {e.name} (pip install ultralytics numpy)"

    try:
        model = YOLO(str(engine_path), task="detect")
    except Exception as e:
        return False, f"load failed: {type(e).__name__}: {e}"

    # Synthetic 640x640x3 zero frame. Ultralytics handles letterbox/
    # resize internally per the engine's metadata.json — we just need
    # SOMETHING shaped like a camera frame to exercise the forward path.
    frame = np.zeros((640, 640, 3), dtype=np.uint8)

    try:
        t0 = time.perf_counter()
        results = model.predict(source=frame, conf=0.10, iou=0.45,
                                verbose=False, save=False, stream=False)
        elapsed_ms = (time.perf_counter() - t0) * 1000
    except Exception as e:
        return False, f"forward failed: {type(e).__name__}: {e}"

    # We don't assert any detection count — synthetic zero frame may
    # produce zero or random low-conf boxes depending on engine quirks.
    # The smoke is "did the forward path execute without throwing".
    if results is None or len(results) == 0:
        return False, "forward returned empty result list"

    r = results[0]
    n = len(r.boxes) if hasattr(r, "boxes") and r.boxes is not None else 0
    return True, f"loaded + 1 forward pass OK ({elapsed_ms:.0f} ms, {n} detections on zero frame)"


def main() -> int:
    if len(sys.argv) > 1:
        models_dir = Path(sys.argv[1]).resolve()
    else:
        # Default: cpp-backend/build/Release/models — the canonical deploy
        # target the install_service.ps1 script populates.
        repo_root = Path(__file__).resolve().parent.parent
        models_dir = repo_root / "build" / "Release" / "models"

    print(f"engine smoke — scanning {models_dir}")
    engines = discover_engines(models_dir)
    if not engines:
        print(f"  no *.engine files found — exiting OK (fresh box / CI shape)")
        return 0

    print(f"  found {len(engines)} engine(s)")
    print()

    any_failed = False
    for engine in engines:
        kind = classify(engine)
        size_mb = engine.stat().st_size / (1024 * 1024)
        prefix = f"[{engine.name}] ({size_mb:.1f} MB)"

        if kind == "non_yolo_skip":
            print(f"  SKIP  {prefix}: non-YOLO shape (SCRFD/ArcFace/OSNet) — smoke not implemented")
            continue

        ok, msg = smoke_yolo_engine(engine)
        if ok:
            print(f"  PASS  {prefix}: {msg}")
        else:
            print(f"  FAIL  {prefix}: {msg}")
            any_failed = True

    print()
    if any_failed:
        print("VERDICT: at least one engine failed deploy-smoke. Investigate.")
        return 1
    print("VERDICT: all engines pass deploy-smoke.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
