"""
Reorganizes Kaggle Garbage Classification dataset into 3 RA 9003 categories:
biodegradable, recyclable, residual.
"""
import shutil
from pathlib import Path

SOURCE_ROOT = Path("garbage_classification/garbage_classification")
OUTPUT_ROOT = Path("waste_data")

MAPPING = {
    "biological":   "biodegradable",
    "brown-glass":  "recyclable",
    "cardboard":    "recyclable",
    "green-glass":  "recyclable",
    "metal":        "recyclable",
    "paper":        "recyclable",
    "plastic":      "recyclable",
    "white-glass":  "recyclable",
    "battery":      "residual",
    "trash":        "residual",
}

def main():
    for cat in ["biodegradable", "recyclable", "residual"]:
        (OUTPUT_ROOT / cat).mkdir(parents=True, exist_ok=True)

    count = {"biodegradable": 0, "recyclable": 0, "residual": 0}
    for src_class, tgt_class in MAPPING.items():
        src = SOURCE_ROOT / src_class
        if not src.exists():
            print(f"[skip] {src} not found")
            continue
        for img in src.glob("*.*"):
            if img.suffix.lower() in [".jpg", ".jpeg", ".png"]:
                dst = OUTPUT_ROOT / tgt_class / f"{src_class}_{img.name}"
                shutil.copy2(img, dst)
                count[tgt_class] += 1
        print(f"  copied {src_class} -> {tgt_class}")

    print("\nDone. Image counts:")
    for k, v in count.items():
        print(f"  {k}: {v}")

if __name__ == "__main__":
    main()