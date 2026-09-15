> **2026-09-15 正式接入更正：** 当前 `decision_tree_full.json` 的特征与树实现以 `models/20260913/train_waveform_14features.py` 为准（完整事件、100Hz分频、自定义 double Tree）。旧 `scripts/extract_features_20260913.py` 和 `scripts/train_decision_tree_20260913.py` 与该模型不匹配，不可用于生成部署特征或复现该模型。以下旧脚本说明仅保留作历史记录。部署验证见仓库 `docs/model_integration_20260915.md`；本次未重新训练或重做 OOF 评估。

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

## Waveform to 14 Features

`train_waveform_14features.py` is the reproducible training entry point. It
reads the event CSV waveforms, extracts the following 14 features, performs
five-fold grouped evaluation by `measurement_id`, and exports a decision-tree
model and evaluation artifacts:

```text
peak_abs, peak_time_ms, rms_0_100ms, rms_100_300ms,
dominant_freq_hz, spectral_centroid_hz, low_high_energy_ratio,
decay_tau_ms, piezo_std, accel_mag_mean, accel_mag_std,
gyro_mag_mean, gyro_mag_std, mpu_valid_ratio
```

Run it from the repository root with:

```bash
python models/20260913/train_waveform_14features.py \
  --dataset /path/to/training_dataset_20260911 \
  --outdir ./model_output
```

The output directory contains `features_14.csv`, `tree_model.json`,
`results.json`, `fold_metrics.csv`, `oof_predictions.csv`, and
`confusion_matrix.png`. Dependencies are NumPy, Pandas, and Pillow.

The full cleaned dataset model is the current recommended candidate because it
meets the project target of at least 85% three-class accuracy under the grouped
OOF evaluation protocol. The main remaining errors are between `medium` and
`loose`.
