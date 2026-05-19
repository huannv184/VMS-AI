"""
PPE engine standalone smoke test.

Loads PPE-YOLOv8m.engine via the Ultralytics runtime (which auto-handles
the model's preprocessing per its export metadata), runs prediction on a
known-person snapshot, and prints raw detections.

Purpose: triangulate the 2026-05-19 PPE phantom-detection bug
(BUG-PPE-PHANTOM-FEATURE followup). If THIS standalone test produces
detections, the engine + Ultralytics' preprocessing combo is healthy and
the bug is in cpp-backend's AdvancedInfer preprocessing pipeline. If
this ALSO produces zero, the engine itself is the issue (retraining or
model swap is the operator's path forward).
"""
from pathlib import Path
import sys

PROJECT_ROOT = Path("D:/buidC/ai2.1/AI-Camera-System")
ENGINE = PROJECT_ROOT / "cpp-backend/build/Release/models/PPE-YOLOv8m.engine"
SNAPSHOT_DIR = PROJECT_ROOT / "cpp-backend/build/Release/data/snapshots"


def pick_test_image() -> Path:
    # Prefer cam2/cam3 — those had real Person events in the 2026-05-19
    # ai_event_processor log around 19:08-19:11 and 19:22.
    candidates = sorted(SNAPSHOT_DIR.glob("cam2_*.jpg")) + \
                 sorted(SNAPSHOT_DIR.glob("cam3_*.jpg"))
    if not candidates:
        candidates = sorted(SNAPSHOT_DIR.glob("*.jpg"))
    if not candidates:
        sys.exit(f"No snapshots in {SNAPSHOT_DIR}")
    # Most recent — most likely to be a test-walk frame
    return candidates[-1]


def main() -> int:
    if not ENGINE.exists():
        sys.exit(f"Engine not found: {ENGINE}")

    img = pick_test_image()
    print(f"engine:   {ENGINE} ({ENGINE.stat().st_size // (1024*1024)} MB)")
    print(f"test img: {img}")

    # Ultralytics YOLO auto-detects engine format and applies the
    # preprocessing baked into the engine's metadata.json (sibling file
    # next to the .engine if exported via ultralytics export).
    from ultralytics import YOLO
    model = YOLO(str(ENGINE), task="detect")

    # conf=0.10 = permissive, surface borderline detections.
    # verbose=True prints per-class hits to stdout from Ultralytics' side.
    results = model.predict(source=str(img), conf=0.10, iou=0.45,
                            verbose=True, save=False, stream=False)

    r = results[0]
    boxes = r.boxes
    print()
    print("=" * 60)
    print(f"RAW DETECTIONS: {len(boxes)} boxes")
    print("=" * 60)
    if hasattr(model, "names"):
        print(f"engine class names: {model.names}")
    for i, box in enumerate(boxes):
        cls = int(box.cls.item())
        conf = float(box.conf.item())
        xyxy = [round(float(v), 1) for v in box.xyxy[0].tolist()]
        label = model.names.get(cls, f"class_{cls}") if hasattr(model, "names") else str(cls)
        print(f"  [{i:>2}] cls={cls} ({label}) conf={conf:.3f} bbox={xyxy}")

    if len(boxes) == 0:
        print()
        print("VERDICT: engine produced ZERO detections on this image even at "
              "conf=0.10. Engine itself likely miscalibrated for this scene "
              "OR the .engine was built without the right precision (fp16/fp32) "
              "for the deployment GPU.")
    else:
        print()
        print("VERDICT: engine IS producing detections — cpp-backend's "
              "AdvancedInfer preprocessing path is the next suspect.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
