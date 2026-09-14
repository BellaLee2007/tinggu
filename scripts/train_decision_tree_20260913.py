"""Train and export the 2026-09-13 Tinggu decision-tree candidates.

The input files are feature tables, not raw waveforms. Evaluation is grouped by
``measurement_id`` so strikes from one measurement never cross train/test folds.
Raw data and generated cache files remain local; only reproducible model and
evaluation artifacts are written under ``models/20260913``.
"""

from __future__ import annotations

import argparse
import json
import os
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(os.environ.get("TINGGU_ROOT", Path(__file__).resolve().parents[1]))
DEFAULT_OUTPUT = ROOT / "models" / "20260913"
LABELS = ["tight", "medium", "loose"]
FEATURES = [
    "peak_abs",
    "peak_time_ms",
    "rms_0_100ms",
    "rms_100_300ms",
    "dominant_freq_hz",
    "spectral_centroid_hz",
    "low_high_energy_ratio",
    "decay_tau_ms",
    "piezo_std",
    "accel_mag_mean",
    "accel_mag_std",
    "gyro_mag_mean",
    "gyro_mag_std",
    "mpu_valid_ratio",
]
MAX_DEPTH = 4
MIN_LEAF = 3
CV_FOLDS = 5
RANDOM_STATE = 20260913


@dataclass(frozen=True)
class DatasetSpec:
    name: str
    path: Path
    suffix: str


def load_dependencies() -> None:
    global np, pd, plt
    global accuracy_score, confusion_matrix, f1_score, precision_recall_fscore_support
    global StratifiedGroupKFold, DecisionTreeClassifier
    try:
        import matplotlib.pyplot as plt_module
        import numpy as np_module
        import pandas as pd_module
        from sklearn.metrics import (
            accuracy_score as accuracy_score_function,
            confusion_matrix as confusion_matrix_function,
            f1_score as f1_score_function,
            precision_recall_fscore_support as precision_recall_fscore_support_function,
        )
        from sklearn.model_selection import StratifiedGroupKFold as stratified_group_k_fold
        from sklearn.tree import DecisionTreeClassifier as decision_tree_classifier
    except ImportError as error:
        raise RuntimeError(
            "Training dependencies are missing. Install numpy, pandas, "
            "scikit-learn, and matplotlib in the project environment."
        ) from error
    np = np_module
    pd = pd_module
    plt = plt_module
    accuracy_score = accuracy_score_function
    confusion_matrix = confusion_matrix_function
    f1_score = f1_score_function
    precision_recall_fscore_support = precision_recall_fscore_support_function
    StratifiedGroupKFold = stratified_group_k_fold
    DecisionTreeClassifier = decision_tree_classifier


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--full-csv",
        type=Path,
        default=ROOT / "data" / "processed" / "training_dataset_20260911.csv",
        help="Cleaned full feature table.",
    )
    parser.add_argument(
        "--perfect-strict-csv",
        type=Path,
        default=ROOT / "data" / "processed" / "training_dataset_20260911_perfect_strict.csv",
        help="Zero-anomaly, zero-drop feature table.",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=DEFAULT_OUTPUT,
        help="Directory for model JSON, metrics JSON, plots, and summary.",
    )
    return parser.parse_args()


def load_dataset(path: Path) -> pd.DataFrame:
    if not path.is_file():
        raise FileNotFoundError(f"Feature table not found: {path}")
    frame = pd.read_csv(path)
    required = ["measurement_id", "label", *FEATURES]
    missing = [column for column in required if column not in frame.columns]
    if missing:
        raise ValueError(f"{path} is missing required columns: {missing}")

    frame = frame[required].copy()
    unknown = sorted(set(frame["label"].dropna().astype(str)) - set(LABELS))
    if unknown:
        raise ValueError(f"{path} contains unsupported labels: {unknown}")
    frame = frame[frame["label"].isin(LABELS)].reset_index(drop=True)
    if frame.empty:
        raise ValueError(f"{path} contains no supported rows")
    if frame["measurement_id"].isna().any():
        raise ValueError(f"{path} contains missing measurement_id values")

    frame[FEATURES] = frame[FEATURES].apply(pd.to_numeric, errors="coerce")
    invalid = frame[FEATURES].isna().sum()
    invalid = invalid[invalid > 0].to_dict()
    if invalid:
        raise ValueError(f"{path} contains missing or non-numeric features: {invalid}")
    frame["label_id"] = frame["label"].map({label: index for index, label in enumerate(LABELS)})

    labels_per_group = frame.groupby("measurement_id")["label"].nunique()
    if (labels_per_group > 1).any():
        bad = labels_per_group[labels_per_group > 1].index.tolist()[:5]
        raise ValueError(f"measurement_id values span multiple labels: {bad}")
    return frame


def new_tree() -> DecisionTreeClassifier:
    return DecisionTreeClassifier(
        max_depth=MAX_DEPTH,
        min_samples_leaf=MIN_LEAF,
        random_state=RANDOM_STATE,
    )


def export_node(model: DecisionTreeClassifier, node_id: int) -> dict:
    tree = model.tree_
    prediction = int(model.classes_[int(np.argmax(tree.value[node_id][0]))])
    node = {"pred": prediction, "n": int(tree.n_node_samples[node_id])}
    left = int(tree.children_left[node_id])
    right = int(tree.children_right[node_id])
    if left != right:
        node.update(
            {
                "j": int(tree.feature[node_id]),
                "thr": float(tree.threshold[node_id]),
                "left": export_node(model, left),
                "right": export_node(model, right),
            }
        )
    return node


def per_class_metrics(y_true: np.ndarray, y_pred: np.ndarray) -> dict:
    precision, recall, f1, support = precision_recall_fscore_support(
        y_true,
        y_pred,
        labels=np.arange(len(LABELS)),
        zero_division=0,
    )
    return {
        label: {
            "precision": float(precision[index]),
            "recall": float(recall[index]),
            "f1": float(f1[index]),
            "support": int(support[index]),
        }
        for index, label in enumerate(LABELS)
    }


def plot_confusion(matrix: np.ndarray, path: Path, title: str) -> None:
    figure, axis = plt.subplots(figsize=(5.6, 4.8), constrained_layout=True)
    image = axis.imshow(matrix, cmap="Blues")
    axis.set(
        title=title,
        xlabel="Predicted label",
        ylabel="True label",
        xticks=np.arange(len(LABELS)),
        yticks=np.arange(len(LABELS)),
        xticklabels=LABELS,
        yticklabels=LABELS,
    )
    threshold = matrix.max() / 2 if matrix.size else 0
    for row in range(matrix.shape[0]):
        for column in range(matrix.shape[1]):
            axis.text(
                column,
                row,
                str(int(matrix[row, column])),
                ha="center",
                va="center",
                color="white" if matrix[row, column] > threshold else "black",
            )
    figure.colorbar(image, ax=axis, shrink=0.82)
    figure.savefig(path, dpi=160)
    plt.close(figure)


def train_dataset(spec: DatasetSpec, output_dir: Path) -> dict:
    frame = load_dataset(spec.path)
    x = frame[FEATURES].to_numpy(dtype=float)
    y = frame["label_id"].to_numpy(dtype=int)
    groups = frame["measurement_id"].to_numpy()
    splitter = StratifiedGroupKFold(
        n_splits=CV_FOLDS,
        shuffle=True,
        random_state=RANDOM_STATE,
    )

    predictions = np.full(len(frame), -1, dtype=int)
    folds = []
    for fold_index, (train_indices, test_indices) in enumerate(
        splitter.split(x, y, groups), start=1
    ):
        model = new_tree().fit(x[train_indices], y[train_indices])
        fold_prediction = model.predict(x[test_indices]).astype(int)
        predictions[test_indices] = fold_prediction
        folds.append(
            {
                "fold": fold_index,
                "train_events": int(len(train_indices)),
                "test_events": int(len(test_indices)),
                "test_groups": int(len(np.unique(groups[test_indices]))),
                "accuracy": float(accuracy_score(y[test_indices], fold_prediction)),
                "macro_f1": float(f1_score(y[test_indices], fold_prediction, average="macro")),
            }
        )
    if (predictions < 0).any():
        raise RuntimeError(f"OOF predictions are incomplete for {spec.name}")

    matrix = confusion_matrix(y, predictions, labels=np.arange(len(LABELS)))
    final_model = new_tree().fit(x, y)
    model_export = {
        "feature_names": FEATURES,
        "max_depth": MAX_DEPTH,
        "min_leaf": MIN_LEAF,
        "root": export_node(final_model, 0),
    }
    importance = [
        {"feature": feature, "importance": float(value)}
        for feature, value in sorted(
            zip(FEATURES, final_model.feature_importances_, strict=True),
            key=lambda item: item[1],
            reverse=True,
        )
        if value > 0
    ]
    result = {
        "dataset": spec.name,
        "events": int(len(frame)),
        "groups": int(frame["measurement_id"].nunique()),
        "class_counts": {
            label: int((frame["label"] == label).sum()) for label in reversed(LABELS)
        },
        "features": FEATURES,
        "tree": {"max_depth": MAX_DEPTH, "min_leaf": MIN_LEAF},
        "cv": {
            "scheme": "5-fold StratifiedGroupKFold by measurement_id",
            "random_state": RANDOM_STATE,
            "folds": folds,
            "oof_accuracy": float(accuracy_score(y, predictions)),
            "oof_macro_f1": float(f1_score(y, predictions, average="macro")),
            "confusion_matrix": matrix.tolist(),
            "per_class": per_class_metrics(y, predictions),
        },
        "feature_importance_surrogate": importance,
    }

    (output_dir / f"decision_tree_{spec.suffix}.json").write_text(
        json.dumps(model_export, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    (output_dir / f"results_{spec.suffix}.json").write_text(
        json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    plot_confusion(
        matrix,
        output_dir / f"confusion_matrix_{spec.suffix}.png",
        f"Grouped OOF confusion matrix: {spec.name}",
    )
    print(
        f"{spec.name}: events={len(frame)} groups={frame['measurement_id'].nunique()} "
        f"accuracy={result['cv']['oof_accuracy']:.4f} "
        f"macro_f1={result['cv']['oof_macro_f1']:.4f}",
        flush=True,
    )
    return result


def main() -> None:
    args = parse_args()
    load_dependencies()
    output_dir = args.output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    specs = [
        DatasetSpec("training_dataset_20260911", args.full_csv.resolve(), "full"),
        DatasetSpec(
            "training_dataset_20260911_perfect_strict",
            args.perfect_strict_csv.resolve(),
            "perfect_strict",
        ),
    ]
    results = [train_dataset(spec, output_dir) for spec in specs]
    (output_dir / "summary.json").write_text(
        json.dumps({"results": results}, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    print(f"Artifacts written to {output_dir}", flush=True)


if __name__ == "__main__":
    main()
