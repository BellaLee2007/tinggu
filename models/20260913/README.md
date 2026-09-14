# Tinggu Decision Tree Models 20260913

This directory contains two three-class decision tree models trained from the
2026-09-11 Tinggu impact-response datasets.

## Models

| File | Dataset | Events | Grouped OOF accuracy | Macro F1 |
|---|---|---:|---:|---:|
| `decision_tree_full.json` | Cleaned full dataset | 289 | 87.89% | 86.83% |
| `decision_tree_perfect_strict.json` | Zero-anomaly, zero-drop subset | 93 | 82.80% | 83.04% |

Both models use a CART-style decision tree with maximum depth 4 and minimum
leaf size 3. Evaluation uses five-fold grouped out-of-fold prediction, keeping
all strikes from the same `measurement_id` in the same fold.

The feature order is embedded in each model JSON. The exported trees were
trained on all events in their respective datasets after grouped evaluation.

## Files

- `../../scripts/extract_features_20260913.py`: deterministic conversion from
  event waveform CSV files to the fixed 14-feature contract.
- `../../scripts/train_decision_tree_20260913.py`: reproducible grouped OOF
  evaluation, final fitting, JSON export, and confusion-matrix generation.
- `decision_tree_full.json` and `decision_tree_perfect_strict.json`: exported
  model structure and feature order.
- `results_full.json` and `results_perfect_strict.json`: detailed metrics,
  fold results, per-class metrics, and feature-importance surrogate.
- `confusion_matrix_full.png` and
  `confusion_matrix_perfect_strict.png`: grouped OOF confusion matrices.
- `summary.json`: combined machine-readable summary.

The local reproduction command and input feature-table contract are documented
in `../../docs/model_training_20260913.md`. Raw CSV files are intentionally not
committed.

The full cleaned dataset model is the current recommended candidate because it
meets the project target of at least 85% three-class accuracy under the grouped
OOF evaluation protocol. The main remaining errors are between `medium` and
`loose`.
