"""
Live webcam multi-frame waste classifier.
Captures 5 frames from camera, classifies each, aggregates with majority voting.
Press SPACE to capture & classify. Press Q to quit.
"""
import cv2
import numpy as np
import time
from tensorflow.keras.models import load_model
from tensorflow.keras.applications.mobilenet_v2 import preprocess_input

MODEL_PATH  = "mobilenetv2_waste_final.h5"
CLASS_NAMES = ["biodegradable", "recyclable", "residual"]
N_FRAMES    = 5
DELAY_SEC   = 0.5

print("Loading model...")
model = load_model(MODEL_PATH)
print("Model loaded. Opening camera...")

cap = cv2.VideoCapture(0, cv2.CAP_DSHOW)
if not cap.isOpened():
    raise RuntimeError("Cannot open webcam. Try changing index 0 to 1 or 2.")

print("\n" + "="*50)
print("  SPACE = capture & classify")
print("  Q     = quit")
print("="*50 + "\n")

def classify_object(cap):
    preds, probs, captured = [], [], []
    print(f"\nCapturing {N_FRAMES} frames...")
    for i in range(N_FRAMES):
        ret, frame = cap.read()
        if not ret:
            continue
        resized = cv2.resize(frame, (224, 224))
        rgb = cv2.cvtColor(resized, cv2.COLOR_BGR2RGB)
        x = preprocess_input(np.expand_dims(rgb.astype(np.float32), 0))
        p = model.predict(x, verbose=0)[0]
        pred_idx = int(np.argmax(p))
        preds.append(pred_idx)
        probs.append(p)
        captured.append(frame.copy())
        print(f"  Frame {i+1}: {CLASS_NAMES[pred_idx]:<14s} (confidence {p[pred_idx]*100:.1f}%)")
        time.sleep(DELAY_SEC)

    counts = np.bincount(preds, minlength=3)
    avg_probs = np.mean(probs, axis=0)
    if counts.max() >= (N_FRAMES // 2 + 1):
        final = int(np.argmax(counts))
        method = "majority vote"
    else:
        final = int(np.argmax(avg_probs))
        method = "tiebreaker (avg confidence)"
    return final, preds, probs, captured, method, avg_probs

while True:
    ret, frame = cap.read()
    if not ret:
        break
    display = frame.copy()
    cv2.putText(display, "SPACE = classify  |  Q = quit",
                (10, 30), cv2.FONT_HERSHEY_SIMPLEX, 0.7, (0, 255, 0), 2)
    cv2.imshow("Waste Classifier", display)
    key = cv2.waitKey(1) & 0xFF

    if key == ord('q'):
        break
    elif key == ord(' '):
        final, preds, probs, captured, method, avg_probs = classify_object(cap)
        print(f"\n>>> FINAL: {CLASS_NAMES[final].upper()} <<<")
        print(f"    (decided by {method})")
        print(f"    average confidences: ", end="")
        for i, name in enumerate(CLASS_NAMES):
            print(f"{name}={avg_probs[i]*100:.1f}%  ", end="")
        print("\n")

        # Show result overlay on last frame
        result = captured[-1].copy()
        color = [(0, 200, 0), (200, 100, 0), (0, 0, 200)][final]
        cv2.rectangle(result, (0, 0), (result.shape[1], 80), color, -1)
        cv2.putText(result, CLASS_NAMES[final].upper(), (20, 55),
                    cv2.FONT_HERSHEY_SIMPLEX, 1.5, (255, 255, 255), 3)
        cv2.imshow("Waste Classifier", result)
        cv2.waitKey(3000)

cap.release()
cv2.destroyAllWindows()
print("Done.")