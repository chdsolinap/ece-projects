"""
Trains MobileNetV2 on 3-class waste dataset with two-phase transfer learning.
Uses class weights to handle imbalance.
"""
import tensorflow as tf
from tensorflow.keras.applications import MobileNetV2
from tensorflow.keras.applications.mobilenet_v2 import preprocess_input
from tensorflow.keras.layers import GlobalAveragePooling2D, Dense, Dropout
from tensorflow.keras.models import Model
from tensorflow.keras.optimizers import Adam
from tensorflow.keras.callbacks import EarlyStopping, ModelCheckpoint
from tensorflow.keras.preprocessing.image import ImageDataGenerator
from sklearn.utils.class_weight import compute_class_weight
import numpy as np
import matplotlib.pyplot as plt

DATA_DIR    = "waste_data"
IMG_SIZE    = (224, 224)
BATCH_SIZE  = 32
EPOCHS_P1   = 15
EPOCHS_P2   = 25
SEED        = 42

# --- 1. Data loaders ---
train_gen = ImageDataGenerator(
    preprocessing_function=preprocess_input,
    validation_split=0.30,
    horizontal_flip=True,
    vertical_flip=True,
    rotation_range=30,
    brightness_range=[0.7, 1.3],
    zoom_range=0.2,
    shear_range=0.15,
    fill_mode="nearest",
)
eval_gen = ImageDataGenerator(
    preprocessing_function=preprocess_input,
    validation_split=0.30,
)

train_ds = train_gen.flow_from_directory(
    DATA_DIR, target_size=IMG_SIZE, batch_size=BATCH_SIZE,
    class_mode="categorical", subset="training", seed=SEED, shuffle=True)

val_ds = eval_gen.flow_from_directory(
    DATA_DIR, target_size=IMG_SIZE, batch_size=BATCH_SIZE,
    class_mode="categorical", subset="validation", seed=SEED, shuffle=False)

print("Classes:", train_ds.class_indices)

# --- 2. Compute class weights to handle imbalance ---
class_weights = compute_class_weight(
    class_weight="balanced",
    classes=np.arange(3),
    y=train_ds.classes
)
class_weight_dict = dict(enumerate(class_weights))
print("Class weights:", class_weight_dict)

# --- 3. Build model ---
base = MobileNetV2(input_shape=(*IMG_SIZE, 3), include_top=False, weights="imagenet")
base.trainable = False

x = base.output
x = GlobalAveragePooling2D()(x)
x = Dense(128, activation="relu")(x)
x = Dropout(0.3)(x)
out = Dense(3, activation="softmax")(x)

model = Model(base.input, out)

# --- 4. Phase 1: train head only ---
model.compile(optimizer=Adam(1e-3), loss="categorical_crossentropy", metrics=["accuracy"])
print("\n=== Phase 1: training head ===")
h1 = model.fit(
    train_ds, validation_data=val_ds, epochs=EPOCHS_P1,
    class_weight=class_weight_dict,
    callbacks=[EarlyStopping(patience=5, restore_best_weights=True)]
)

# --- 5. Phase 2: fine-tune top layers ---
base.trainable = True
for layer in base.layers[:-30]:
    layer.trainable = False

model.compile(optimizer=Adam(1e-4), loss="categorical_crossentropy", metrics=["accuracy"])
print("\n=== Phase 2: fine-tuning ===")
h2 = model.fit(
    train_ds, validation_data=val_ds, epochs=EPOCHS_P2,
    class_weight=class_weight_dict,
    callbacks=[
        EarlyStopping(patience=8, restore_best_weights=True),
        ModelCheckpoint("mobilenetv2_waste.h5", save_best_only=True),
    ]
)

# --- 6. Save final model + plot training curves ---
model.save("mobilenetv2_waste_final.h5")

fig, axes = plt.subplots(1, 2, figsize=(12, 4))
axes[0].plot(h1.history["accuracy"] + h2.history["accuracy"], label="train")
axes[0].plot(h1.history["val_accuracy"] + h2.history["val_accuracy"], label="val")
axes[0].axvline(len(h1.history["accuracy"]) - 0.5, color="gray", linestyle="--", label="phase 2 start")
axes[0].set_title("Accuracy"); axes[0].set_xlabel("Epoch"); axes[0].legend()
axes[1].plot(h1.history["loss"] + h2.history["loss"], label="train")
axes[1].plot(h1.history["val_loss"] + h2.history["val_loss"], label="val")
axes[1].axvline(len(h1.history["loss"]) - 0.5, color="gray", linestyle="--", label="phase 2 start")
axes[1].set_title("Loss"); axes[1].set_xlabel("Epoch"); axes[1].legend()
plt.tight_layout()
plt.savefig("training_curves.png", dpi=150)
print("\nSaved: mobilenetv2_waste_final.h5, training_curves.png")