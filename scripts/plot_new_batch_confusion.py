"""Plot confusion matrices for the 2026-09-03 grouped ensemble validation."""
from pathlib import Path
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd
from sklearn.metrics import confusion_matrix

ROOT = Path(__file__).resolve().parent
labels = ["tight", "medium", "loose"]
names = {0: "tight", 1: "medium", 2: "loose"}
pred = pd.read_csv(ROOT / "advanced_recrop_20260903" / "repeated_ensemble_predictions.csv")
pred = pred[pred.block_size == 4].copy()  # best measurement-level block size
pred["true"] = pred.true_id.map(names)
pred["pred"] = pred.predicted_id.map(names)
cm = confusion_matrix(pred.true, pred.pred, labels=labels)
row_norm = cm / np.maximum(cm.sum(axis=1, keepdims=True), 1)
pd.DataFrame(cm, index=labels, columns=labels).to_csv(ROOT / "advanced_recrop_20260903" / "confusion_matrix_block4_counts.csv")
pd.DataFrame(row_norm, index=labels, columns=labels).to_csv(ROOT / "advanced_recrop_20260903" / "confusion_matrix_block4_normalized.csv")

fig, ax = plt.subplots(figsize=(6.4, 5.4), dpi=180)
im = ax.imshow(row_norm, cmap="Blues", vmin=0, vmax=1)
for i in range(3):
    for j in range(3):
        ax.text(j, i, f"{cm[i,j]}\n{row_norm[i,j]:.1%}", ha="center", va="center",
                color="white" if row_norm[i,j] > 0.55 else "black", fontsize=11)
ax.set_xticks(range(3), labels)
ax.set_yticks(range(3), labels)
ax.set_xlabel("Predicted label")
ax.set_ylabel("True label")
ax.set_title("2026-09-03 ensemble confusion matrix\nmeasurement-level, block size 4; counts pooled over 5 seeds")
fig.colorbar(im, ax=ax, label="Row-normalized recall")
fig.tight_layout()
fig.savefig(ROOT / "advanced_recrop_20260903" / "confusion_matrix_block4_heatmap.png", bbox_inches="tight")
print(cm.tolist())
print(np.round(row_norm, 4).tolist())

# The complete-three-hit subset is a stricter estimate of the intended fusion mode.
triple = pred[pred.n_hits == 3]
cm3 = confusion_matrix(triple.true, triple.pred, labels=labels)
row3 = cm3 / np.maximum(cm3.sum(axis=1, keepdims=True), 1)
fig, ax = plt.subplots(figsize=(6.4, 5.4), dpi=180)
im = ax.imshow(row3, cmap="Blues", vmin=0, vmax=1)
for i in range(3):
    for j in range(3):
        ax.text(j, i, f"{cm3[i,j]}\n{row3[i,j]:.1%}", ha="center", va="center",
                color="white" if row3[i,j] > 0.55 else "black", fontsize=11)
ax.set_xticks(range(3), labels); ax.set_yticks(range(3), labels)
ax.set_xlabel("Predicted label"); ax.set_ylabel("True label")
ax.set_title("2026-09-03 ensemble confusion matrix\ncomplete three-hit measurements; block size 4")
fig.colorbar(im, ax=ax, label="Row-normalized recall")
fig.tight_layout()
fig.savefig(ROOT / "advanced_recrop_20260903" / "confusion_matrix_block4_triples_heatmap.png", bbox_inches="tight")
pd.DataFrame(cm3, index=labels, columns=labels).to_csv(ROOT / "advanced_recrop_20260903" / "confusion_matrix_block4_triples_counts.csv")
print("triples", cm3.tolist())
