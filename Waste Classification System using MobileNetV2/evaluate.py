"""
Evaluates the trained model on a held-out test set.
Compares single-frame vs multi-frame (5 augmented views) classification.
Generates classification report + confusion matrices for Chapter 4.
"""
import numpy as np
import tensorflow as tf
from tensorflow.keras.models import load_model
from tensorflow.keras.applications.mobilenet_v2 import preprocess_input
from tensorflow.keras.preprocessing.image import ImageDataGenerator, load_img, img_to_array
from sklearn.metrics import classification_report, confusion_matrix
import matplotlib.pyplot as plt
import seaborn as sns
from pathlib import Path
import random

MODEL_PATH  = "mobilenetv2_waste_final.h5"
DATA_DIR    = "waste_data"
IMG_SIZE    = (224, 224)
N_FRAMES    = 5
SEED        = 42

random.seed(SEED); np.random.seed(SEED)

print("Loading model...")
model = load_model(MODEL_PATH)
class_names = sorted([d.name for d in Path(DATA_DIR).iterdir() if d.is_dir()])
print("Classes:", class_names)

# Test set = last 15% of each class (alphabetical, deterministic)
test_images = []
for cls_idx, cls in enumerate(class_names):
    imgs = sorted((Path(DATA_DIR) / cls).glob("*.*"))
    split_start = int(len(imgs) * 0.85)
    for img_path in imgs[split_start:]:
        test_images.append((img_path, cls_idx))

print(f"Test images: {len(test_images)}")

# Augmenter for multi-frame simulation
aug = ImageDataGenerator(
    horizontal_flip=True, rotation_range=30,
    brightness_range=[0.7, 1.3], zoom_range=0.2,
)

def predict_single(img_path):
    img = img_to_array(load_img(img_path, target_size=IMG_SIZE))
    x = preprocess_input(np.expand_dims(img, 0))
    return int(np.argmax(model.predict(x, verbose=0)[0]))

def predict_multiframe(img_path, n=N_FRAMES):
    img = img_to_array(load_img(img_path, target_size=IMG_SIZE))
    preds, probs = [], []
    for batch in aug.flow(np.expand_dims(img, 0), batch_size=1, seed=random.randint(0, 9999)):
        x = preprocess_input(batch)
        p = model.predict(x, verbose=0)[0]
        preds.append(int(np.argmax(p)))
        probs.append(p)
        if len(preds) >= n:
            break
    counts = np.bincount(preds, minlength=3)
    if counts.max() >= (n // 2 + 1):
        return int(np.argmax(counts))
    return int(np.argmax(np.sum(probs, axis=0)))

print("\nEvaluating... (this takes a few minutes)")
y_true = [c for _, c in test_images]
y_single, y_multi = [], []
for i, (p, _) in enumerate(test_images):
    y_single.append(predict_single(p))
    y_multi.append(predict_multiframe(p))
    if (i + 1) % 100 == 0:
        print(f"  {i+1}/{len(test_images)}")

print("\n" + "="*60)
print("SINGLE-FRAME RESULTS")
print("="*60)
print(classification_report(y_true, y_single, target_names=class_names, digits=4))

print("\n" + "="*60)
print("MULTI-FRAME RESULTS (5 frames, majority vote)")
print("="*60)
print(classification_report(y_true, y_multi, target_names=class_names, digits=4))

# Confusion matrices
fig, axes = plt.subplots(1, 2, figsize=(13, 5))
for ax, y_pred, title in zip(axes, [y_single, y_multi], ["Single-Frame", "Multi-Frame (5 frames)"]):
    cm = confusion_matrix(y_true, y_pred)
    sns.heatmap(cm, annot=True, fmt="d", cmap="Blues",
                xticklabels=class_names, yticklabels=class_names, ax=ax,
                cbar=False)
    ax.set_title(title); ax.set_xlabel("Predicted"); ax.set_ylabel("True")
plt.tight_layout()
plt.savefig("confusion_matrices.png", dpi=150)
print("\nSaved: confusion_matrices.png")