"""Advanced raw-waveform recropping and leakage-resistant model search.

The script intentionally ignores filenames, timestamps, trigger delay, baseline,
noise/SNR and quality grades as classifier inputs.  Every event is recropped to
the same 240 ms interval after the detected physical onset; this is the longest
fixed interval available for every 2026-09-03 sample without zero padding.
"""

from __future__ import annotations

import json
import math
import warnings
from functools import partial
from pathlib import Path
import os

import joblib
import numpy as np
import pandas as pd
from scipy.fft import dct
from scipy.signal import coherence, find_peaks, stft
from sklearn.base import clone
from sklearn.discriminant_analysis import LinearDiscriminantAnalysis
from sklearn.ensemble import ExtraTreesClassifier, HistGradientBoostingClassifier, RandomForestClassifier
from sklearn.feature_selection import SelectKBest, f_classif, mutual_info_classif
from sklearn.impute import SimpleImputer
from sklearn.linear_model import LogisticRegression
from sklearn.metrics import accuracy_score, balanced_accuracy_score, confusion_matrix, f1_score
from sklearn.model_selection import StratifiedGroupKFold
from sklearn.pipeline import Pipeline
from sklearn.preprocessing import StandardScaler
from sklearn.svm import SVC

warnings.filterwarnings("ignore")

ROOT = Path(os.environ.get("TINGGU_ROOT", "."))
SOURCE = ROOT / "data" / "signal_validator"
FEATURE_SOURCE = ROOT / "outputs" / "model_recovery_20260903" / "features_single_hit.csv"
OUT = ROOT / "outputs" / "advanced_recrop_20260903"
OUT.mkdir(parents=True, exist_ok=True)

LABELS = ["tight", "medium", "loose"]
PIEZO_FS = 2000.0
MPU_FS = 1000.0
CROP_MS = 240.0
PIEZO_N = int(CROP_MS * PIEZO_FS / 1000.0)
MPU_N = int(CROP_MS * MPU_FS / 1000.0)


def robust_scale(x: np.ndarray) -> float:
    x = np.asarray(x, dtype=float)
    x = x[np.isfinite(x)]
    if not x.size:
        return 1e-9
    center = np.median(x)
    return max(float(np.median(np.abs(x - center)) * 1.4826), 1e-9)


def interp_fixed(t_ms: np.ndarray, x: np.ndarray, start_ms: float, fs: float, n: int) -> np.ndarray:
    t_ms = np.asarray(t_ms, dtype=float)
    x = np.asarray(x, dtype=float)
    keep = np.isfinite(t_ms) & np.isfinite(x)
    if keep.sum() < 2:
        return np.zeros(n)
    t, values = t_ms[keep], x[keep]
    order = np.argsort(t)
    grid = start_ms + np.arange(n) * 1000.0 / fs
    return np.interp(grid, t[order], values[order])


def rms(x: np.ndarray) -> float:
    x = np.asarray(x, dtype=float)
    return float(np.sqrt(np.mean(x * x))) if x.size else 0.0


def moving_rms(x: np.ndarray, width: int) -> np.ndarray:
    x = np.asarray(x, dtype=float)
    width = max(1, int(width))
    kernel = np.ones(width) / width
    return np.sqrt(np.maximum(np.convolve(x * x, kernel, mode="same"), 0.0))


def normalized(x: np.ndarray) -> np.ndarray:
    x = np.asarray(x, dtype=float) - np.mean(x)
    return x / max(rms(x), 1e-9)


def spectral_block(x: np.ndarray, fs: float, prefix: str, max_hz: float, bands: list[tuple[int, int]]) -> dict:
    x = np.asarray(x, dtype=float)
    tapered = (x - np.mean(x)) * np.hanning(len(x))
    power = np.abs(np.fft.rfft(tapered)) ** 2
    freq = np.fft.rfftfreq(len(x), 1.0 / fs)
    valid = (freq >= 5.0) & (freq <= max_hz)
    pv, fv = power[valid], freq[valid]
    total = float(pv.sum()) + 1e-12
    prob = pv / total
    peak_ids, properties = find_peaks(prob, prominence=max(float(prob.max()) * 0.02, 1e-10))
    if peak_ids.size:
        order = peak_ids[np.argsort(properties["prominences"])[::-1]]
    else:
        order = np.asarray([int(np.argmax(prob))]) if prob.size else np.asarray([], dtype=int)
    result = {
        f"{prefix}_centroid": float(np.sum(fv * prob)) if prob.size else 0.0,
        f"{prefix}_entropy": float(-np.sum(prob * np.log(prob + 1e-15)) / np.log(max(len(prob), 2))) if prob.size else 0.0,
        f"{prefix}_flatness": float(np.exp(np.mean(np.log(pv + 1e-15))) / (np.mean(pv) + 1e-15)) if pv.size else 0.0,
    }
    for rank in range(3):
        idx = int(order[rank]) if rank < len(order) else 0
        result[f"{prefix}_mode{rank + 1}_hz"] = float(fv[idx]) if fv.size else 0.0
        result[f"{prefix}_mode{rank + 1}_share"] = float(prob[idx]) if prob.size else 0.0
    for lo, hi in bands:
        result[f"{prefix}_band_{lo}_{hi}"] = float(power[(freq >= lo) & (freq < hi)].sum() / total)
    log_power = np.log1p(prob * 1e4)
    cep = dct(log_power, type=2, norm="ortho") if log_power.size else np.zeros(20)
    for i in range(min(20, len(cep))):
        result[f"{prefix}_cep_{i:02d}"] = float(cep[i])
    return result


def decay_block(x: np.ndarray, fs: float, prefix: str) -> dict:
    env = moving_rms(x, max(2, int(0.005 * fs)))
    peak = max(float(np.max(env)), 1e-9)
    result = {f"{prefix}_peak": float(np.max(np.abs(x))), f"{prefix}_rms": rms(x)}
    segment_edges = [0, 10, 20, 40, 80, 120, 180, 240]
    energies = []
    for lo, hi in zip(segment_edges[:-1], segment_edges[1:]):
        a, b = int(lo * fs / 1000.0), int(hi * fs / 1000.0)
        energies.append(float(np.sum(x[a:b] ** 2)))
    total = sum(energies) + 1e-12
    for (lo, hi), value in zip(zip(segment_edges[:-1], segment_edges[1:]), energies):
        result[f"{prefix}_energy_{lo}_{hi}"] = value / total
    for frac in [0.5, 0.25, 0.1]:
        below = env <= frac * peak
        hold = max(2, int(0.010 * fs))
        value = CROP_MS
        peak_i = int(np.argmax(env))
        for i in range(peak_i, len(env) - hold):
            if np.all(below[i : i + hold]):
                value = 1000.0 * i / fs
                break
        result[f"{prefix}_t{int(frac * 100)}"] = value
    # Robust envelope decay slopes for early and late response.
    for name, lo, hi in [("early", 10, 80), ("late", 80, 220)]:
        a, b = int(lo * fs / 1000.0), int(hi * fs / 1000.0)
        y = np.log(env[a:b] / peak + 1e-6)
        tt = np.arange(len(y)) / fs
        result[f"{prefix}_decay_{name}"] = float(np.polyfit(tt, y, 1)[0]) if len(y) >= 4 else 0.0
    # Compact normalized envelope shape.
    source = np.linspace(0.0, 1.0, len(env))
    sampled = np.interp(np.linspace(0.0, 1.0, 32), source, env / peak)
    for i, value in enumerate(sampled):
        result[f"{prefix}_env_{i:02d}"] = float(value)
    return result


def spectrogram_block(x: np.ndarray, fs: float, prefix: str, bands: list[tuple[int, int]]) -> dict:
    nperseg = 64 if fs <= 1000 else 128
    f, t, z = stft(x - np.mean(x), fs=fs, window="hann", nperseg=nperseg, noverlap=nperseg // 2, boundary=None)
    p = np.abs(z) ** 2
    time_edges = [0.0, 0.04, 0.08, 0.14, 0.24]
    result = {}
    for ti, (ta, tb) in enumerate(zip(time_edges[:-1], time_edges[1:])):
        tm = (t >= ta) & (t < tb)
        denom = float(p[:, tm].sum()) + 1e-12
        for lo, hi in bands:
            fm = (f >= lo) & (f < hi)
            result[f"{prefix}_tf_t{ti}_{lo}_{hi}"] = float(p[np.ix_(fm, tm)].sum() / denom) if tm.any() else 0.0
    return result


def transfer_block(p_1k: np.ndarray, m_1k: np.ndarray) -> dict:
    bands = [(5, 30), (30, 60), (60, 100), (100, 150), (150, 200), (200, 250), (250, 400)]
    win = np.hanning(len(p_1k))
    p_fft = np.abs(np.fft.rfft((p_1k - np.mean(p_1k)) * win)) + 1e-9
    m_fft = np.abs(np.fft.rfft((m_1k - np.mean(m_1k)) * win)) + 1e-9
    freq = np.fft.rfftfreq(len(p_1k), 1.0 / MPU_FS)
    ratio = np.log(m_fft / p_fft)
    cf, coh = coherence(p_1k, m_1k, fs=MPU_FS, nperseg=96, noverlap=48)
    result = {}
    for lo, hi in bands:
        mask = (freq >= lo) & (freq < hi)
        cmask = (cf >= lo) & (cf < hi)
        result[f"xfer_logratio_{lo}_{hi}"] = float(np.median(ratio[mask])) if mask.any() else 0.0
        result[f"xfer_coherence_{lo}_{hi}"] = float(np.mean(coh[cmask])) if cmask.any() else 0.0
    return result


def extract_one(source_file: str, start_ms: float) -> dict:
    data = pd.read_csv(SOURCE / source_file)
    t = pd.to_numeric(data["time_us"], errors="coerce").to_numpy(float) / 1000.0
    raw = pd.to_numeric(data["piezo_raw"], errors="coerce").to_numpy(float)
    pre = np.isfinite(raw) & (t >= start_ms - 90.0) & (t <= start_ms - 10.0)
    baseline = float(np.median(raw[pre])) if pre.sum() else float(np.nanmedian(raw[t < start_ms]))
    piezo = raw - baseline
    p = interp_fixed(t, piezo, start_ms, PIEZO_FS, PIEZO_N)

    valid = pd.to_numeric(data["mpu_valid"], errors="coerce").fillna(0).to_numpy(int) == 1
    mt = t[valid]
    axes = np.column_stack([
        pd.to_numeric(data[name], errors="coerce").to_numpy(float)[valid]
        for name in ["ax_g", "ay_g", "az_g"]
    ])
    finite = np.isfinite(mt) & np.all(np.isfinite(axes), axis=1)
    mt, axes = mt[finite], axes[finite]
    mpre = (mt >= start_ms - 90.0) & (mt <= start_ms - 10.0)
    center = np.median(axes[mpre], axis=0) if mpre.sum() else np.median(axes[mt < start_ms], axis=0)
    dyn = axes - center
    resampled_axes = np.column_stack([interp_fixed(mt, dyn[:, i], start_ms, MPU_FS, MPU_N) for i in range(3)])
    covariance = np.cov(resampled_axes.T)
    eigvals, eigvecs = np.linalg.eigh(covariance)
    order = np.argsort(eigvals)[::-1]
    eigvals, eigvecs = eigvals[order], eigvecs[:, order]
    m = resampled_axes @ eigvecs[:, 0]
    # Resolve PCA sign deterministically so shape features do not flip arbitrarily.
    if abs(float(np.min(m))) > abs(float(np.max(m))):
        m = -m
    m_mag = np.sqrt(np.sum(resampled_axes ** 2, axis=1))

    feats = {
        "raw_piezo_peak": float(np.max(np.abs(p))),
        "raw_mpu_peak": float(np.max(m_mag)),
        "raw_transfer_rms": rms(m) / max(rms(p), 1e-9),
    }
    eig_total = float(np.sum(np.maximum(eigvals, 0.0))) + 1e-12
    for i in range(3):
        feats[f"mpu_pca_share_{i + 1}"] = float(max(eigvals[i], 0.0) / eig_total)

    feats.update(decay_block(p, PIEZO_FS, "rp"))
    feats.update(decay_block(m, MPU_FS, "rm"))
    feats.update(decay_block(m_mag, MPU_FS, "rv"))
    feats.update(spectral_block(p, PIEZO_FS, "rp_spec", 800.0, [(5, 30), (30, 60), (60, 100), (100, 150), (150, 200), (200, 300), (300, 500), (500, 800)]))
    feats.update(spectral_block(m, MPU_FS, "rm_spec", 400.0, [(5, 30), (30, 60), (60, 100), (100, 150), (150, 200), (200, 250), (250, 400)]))
    feats.update(spectrogram_block(p, PIEZO_FS, "rp", [(5, 30), (30, 60), (60, 100), (100, 150), (150, 250), (250, 500), (500, 800)]))
    feats.update(spectrogram_block(m, MPU_FS, "rm", [(5, 30), (30, 60), (60, 100), (100, 150), (150, 200), (200, 250), (250, 400)]))
    p_1k = p[::2][:MPU_N]
    feats.update(transfer_block(p_1k, m))

    # Normalized impulse-response samples.  These contain shape but no amplitude.
    for prefix, x in [("rp_shape", normalized(p)), ("rm_shape", normalized(m))]:
        source = np.linspace(0.0, 1.0, len(x))
        sampled = np.interp(np.linspace(0.0, 1.0, 48), source, x)
        for i, value in enumerate(sampled):
            feats[f"{prefix}_{i:02d}"] = float(value)
    return feats


def build_features() -> pd.DataFrame:
    cache = OUT / "advanced_recrop_features.csv"
    if cache.exists():
        return pd.read_csv(cache)
    base = pd.read_csv(FEATURE_SOURCE)
    summary = pd.read_csv(SOURCE / "summary_v6.csv").drop_duplicates("sample_file", keep="last").set_index("sample_file")
    rows = []
    for i, row in base.iterrows():
        sm = summary.loc[row.source_file]
        record = {
            "day": row.day,
            "label": row.label,
            "label_id": int(row.label_id),
            "source_batch": row.source_batch,
            "captured_at": row.captured_at,
            "source_file": row.source_file,
            "measurement_id": str(sm.measurement_id),
            "strike_index": int(sm.strike_index),
        }
        record.update(extract_one(row.source_file, float(sm.event_start_ms)))
        rows.append(record)
        if (i + 1) % 40 == 0:
            print(f"recrop {i + 1}/{len(base)}", flush=True)
    result = pd.DataFrame(rows)
    result.to_csv(cache, index=False)
    return result


def assign_groups(frame: pd.DataFrame, measurements_per_block: int = 4) -> pd.DataFrame:
    result = frame.copy()
    result["group"] = ""
    for (day, label, batch), part in result.groupby(["day", "label", "source_batch"], sort=False):
        first = part.groupby("measurement_id")["captured_at"].min().sort_values()
        rank = {measurement: i for i, measurement in enumerate(first.index)}
        for idx, measurement in part.measurement_id.items():
            result.loc[idx, "group"] = f"{day}|{label}|{batch}|b{rank[measurement] // measurements_per_block:03d}"
    return result


def feature_sets(frame: pd.DataFrame) -> dict[str, list[str]]:
    numeric = [c for c in frame.columns if pd.api.types.is_numeric_dtype(frame[c]) and c not in {"label_id", "strike_index"} and frame[c].std() > 1e-12]
    amplitude = [c for c in numeric if c.startswith("raw_") or c.endswith("_peak") or c.endswith("_rms")]
    normalized_features = [c for c in numeric if c not in amplitude]
    transfer = [c for c in numeric if c.startswith("xfer_") or c.startswith("mpu_pca_")]
    spectra = [c for c in numeric if "_spec_" in c or "_tf_" in c or c.startswith("xfer_")]
    shape = [c for c in numeric if "_env_" in c or "_shape_" in c or "_decay_" in c or "_energy_" in c]
    compact = [c for c in numeric if "_shape_" not in c and "_env_" not in c and "_tf_" not in c and "_cep_" not in c]
    return {
        "compact": compact,
        "normalized": normalized_features,
        "spectral_transfer": sorted(set(spectra + transfer)),
        "shape_decay": shape,
        "all_recropped": numeric,
    }


def candidates(n_features: int):
    k = min(n_features, 60)
    selector_f = SelectKBest(f_classif, k=k)
    selector_mi = SelectKBest(partial(mutual_info_classif, random_state=42), k=k)
    return {
        "svm_rbf_c2": Pipeline([("impute", SimpleImputer(strategy="median")), ("scale", StandardScaler()), ("select", selector_f), ("model", SVC(C=2.0, gamma="scale", probability=True, class_weight="balanced", random_state=42))]),
        "svm_rbf_c10": Pipeline([("impute", SimpleImputer(strategy="median")), ("scale", StandardScaler()), ("select", selector_f), ("model", SVC(C=10.0, gamma="scale", probability=True, class_weight="balanced", random_state=42))]),
        "extra_trees": Pipeline([("impute", SimpleImputer(strategy="median")), ("select", selector_mi), ("model", ExtraTreesClassifier(n_estimators=800, min_samples_leaf=3, max_features=0.5, class_weight="balanced", n_jobs=-1, random_state=42))]),
        "random_forest": Pipeline([("impute", SimpleImputer(strategy="median")), ("select", selector_mi), ("model", RandomForestClassifier(n_estimators=800, min_samples_leaf=3, max_features=0.5, class_weight="balanced_subsample", n_jobs=-1, random_state=42))]),
        "hist_gradient": Pipeline([("impute", SimpleImputer(strategy="median")), ("select", selector_f), ("model", HistGradientBoostingClassifier(max_iter=300, learning_rate=0.05, max_leaf_nodes=15, min_samples_leaf=20, l2_regularization=3.0, random_state=42))]),
        "logistic": Pipeline([("impute", SimpleImputer(strategy="median")), ("scale", StandardScaler()), ("select", selector_f), ("model", LogisticRegression(C=0.2, max_iter=3000, class_weight="balanced", random_state=42))]),
        "shrinkage_lda": Pipeline([("impute", SimpleImputer(strategy="median")), ("scale", StandardScaler()), ("select", selector_f), ("model", LinearDiscriminantAnalysis(solver="lsqr", shrinkage="auto"))]),
    }


def metrics(y_true, y_pred) -> dict:
    return {
        "accuracy": float(accuracy_score(y_true, y_pred)),
        "balanced_accuracy": float(balanced_accuracy_score(y_true, y_pred)),
        "macro_f1": float(f1_score(y_true, y_pred, average="macro")),
    }


def late_holdout(frame: pd.DataFrame, fraction: float = 0.2):
    train, test = [], []
    for _, part in frame.groupby("label"):
        first = part.groupby("measurement_id")["captured_at"].min().sort_values()
        split = max(1, int(len(first) * (1.0 - fraction)))
        train_ids, test_ids = set(first.index[:split]), set(first.index[split:])
        train.extend(part.index[part.measurement_id.isin(train_ids)])
        test.extend(part.index[part.measurement_id.isin(test_ids)])
    return np.asarray(train), np.asarray(test)


def aggregate_measurements(frame: pd.DataFrame, probabilities: np.ndarray, folds: np.ndarray) -> pd.DataFrame:
    tmp = frame[["measurement_id", "label", "label_id", "strike_index", "source_batch"]].copy()
    tmp["fold"] = folds
    for i, label in enumerate(LABELS):
        tmp[f"p_{label}"] = probabilities[:, i]
    rows = []
    for measurement, part in tmp.groupby("measurement_id"):
        if part.fold.nunique() != 1:
            continue
        prob = part[[f"p_{x}" for x in LABELS]].mean().to_numpy(float)
        rows.append({
            "measurement_id": measurement,
            "label": part.label.iloc[0],
            "label_id": int(part.label_id.iloc[0]),
            "n_hits": len(part),
            "pred_id": int(np.argmax(prob)),
            "confidence": float(np.max(prob)),
            **{f"prob_{label}": float(prob[i]) for i, label in enumerate(LABELS)},
        })
    return pd.DataFrame(rows)


def main():
    all_features = build_features()
    today = assign_groups(all_features[all_features.day == "2026-09-03"].copy().reset_index(drop=True), 4)
    yesterday = assign_groups(all_features[all_features.day == "2026-09-02"].copy().reset_index(drop=True), 4)
    y = today.label_id.to_numpy(int)
    groups = today.group.to_numpy()
    splitter = StratifiedGroupKFold(n_splits=5, shuffle=True, random_state=20260903)
    folds = list(splitter.split(today, y, groups))
    hold_train, hold_test = late_holdout(today)
    sets = feature_sets(today)

    rows, oof_store = [], []
    for set_name, features in sets.items():
        for model_name, model in candidates(len(features)).items():
            proba = np.zeros((len(today), 3))
            fold_ids = np.full(len(today), -1)
            for fold, (tr, te) in enumerate(folds):
                fitted = clone(model).fit(today.iloc[tr][features], y[tr])
                proba[te] = fitted.predict_proba(today.iloc[te][features])
                fold_ids[te] = fold
            pred = np.argmax(proba, axis=1)
            single = metrics(y, pred)
            measurements = aggregate_measurements(today, proba, fold_ids)
            all_measurements = metrics(measurements.label_id, measurements.pred_id)
            triples = measurements[measurements.n_hits == 3]
            complete = metrics(triples.label_id, triples.pred_id) if len(triples) else {"accuracy": np.nan, "balanced_accuracy": np.nan, "macro_f1": np.nan}
            late_model = clone(model).fit(today.iloc[hold_train][features], y[hold_train])
            late_pred = late_model.predict(today.iloc[hold_test][features])
            late = metrics(y[hold_test], late_pred)
            rows.append({
                "feature_set": set_name,
                "model": model_name,
                "n_features": len(features),
                "single_accuracy": single["accuracy"],
                "single_macro_f1": single["macro_f1"],
                "measurement_accuracy": all_measurements["accuracy"],
                "measurement_macro_f1": all_measurements["macro_f1"],
                "complete_triple_n": len(triples),
                "complete_triple_accuracy": complete["accuracy"],
                "complete_triple_macro_f1": complete["macro_f1"],
                "late_accuracy": late["accuracy"],
                "late_macro_f1": late["macro_f1"],
            })
            oof_store.append((all_measurements["macro_f1"], set_name, model_name, features, model, proba, fold_ids, measurements))
            print(f"{set_name}/{model_name}: hit={single['accuracy']:.3f}, measurement={all_measurements['accuracy']:.3f}, triple={complete['accuracy']:.3f}, late={late['accuracy']:.3f}", flush=True)

    leaderboard = pd.DataFrame(rows).sort_values(["measurement_macro_f1", "complete_triple_macro_f1"], ascending=False)
    leaderboard.to_csv(OUT / "advanced_leaderboard.csv", index=False)
    best = max(oof_store, key=lambda item: item[0])
    _, best_set, best_name, best_features, best_model, best_proba, best_folds, best_measurements = best
    best_measurements.to_csv(OUT / "best_oof_measurements.csv", index=False)

    # Honest fixed soft-vote ensemble of the three best distinct model families.
    ranked = sorted(oof_store, key=lambda item: item[0], reverse=True)
    chosen, names = [], set()
    for item in ranked:
        if item[2] not in names:
            chosen.append(item)
            names.add(item[2])
        if len(chosen) == 3:
            break
    ensemble_proba = np.mean([item[5] for item in chosen], axis=0)
    ensemble_measurements = aggregate_measurements(today, ensemble_proba, chosen[0][6])
    ensemble_metrics = metrics(ensemble_measurements.label_id, ensemble_measurements.pred_id)
    ensemble_triples = ensemble_measurements[ensemble_measurements.n_hits == 3]
    ensemble_triple_metrics = metrics(ensemble_triples.label_id, ensemble_triples.pred_id)
    ensemble_measurements.to_csv(OUT / "ensemble_oof_measurements.csv", index=False)

    # Confidence/coverage curve at measurement level.
    confidence_rows = []
    for threshold in np.arange(0.40, 0.86, 0.05):
        accepted = ensemble_measurements.confidence >= threshold
        if accepted.any():
            accepted_metrics = metrics(ensemble_measurements.loc[accepted, "label_id"], ensemble_measurements.loc[accepted, "pred_id"])
            confidence_rows.append({"threshold": threshold, "accepted": int(accepted.sum()), "coverage": float(accepted.mean()), **accepted_metrics})
    pd.DataFrame(confidence_rows).to_csv(OUT / "confidence_curve.csv", index=False)

    # Cross-day audit for the best model. This remains a domain-shift diagnostic.
    fitted = clone(best_model).fit(today[best_features], y)
    cross_pred = fitted.predict(yesterday[best_features])
    cross = metrics(yesterday.label_id, cross_pred)
    final_model = clone(best_model).fit(today[best_features], y)
    joblib.dump({
        "model": final_model,
        "features": best_features,
        "labels": LABELS,
        "crop_ms": CROP_MS,
        "feature_set": best_set,
        "model_name": best_name,
        "status": "experimental_requires_independent_session_validation",
    }, OUT / "candidate_advanced_recrop.joblib")

    summary = {
        "sample_counts": today.label.value_counts().to_dict(),
        "fixed_recrop_ms": CROP_MS,
        "best_feature_set": best_set,
        "best_model": best_name,
        "best_row": leaderboard.iloc[0].to_dict(),
        "best_measurement_confusion": confusion_matrix(best_measurements.label_id, best_measurements.pred_id, labels=[0, 1, 2]).tolist(),
        "ensemble_models": [f"{item[1]}/{item[2]}" for item in chosen],
        "ensemble_measurement_metrics": ensemble_metrics,
        "ensemble_complete_triple_metrics": ensemble_triple_metrics,
        "ensemble_measurement_confusion": confusion_matrix(ensemble_measurements.label_id, ensemble_measurements.pred_id, labels=[0, 1, 2]).tolist(),
        "ensemble_complete_triple_confusion": confusion_matrix(ensemble_triples.label_id, ensemble_triples.pred_id, labels=[0, 1, 2]).tolist(),
        "complete_triple_count": int(len(ensemble_triples)),
        "cross_day_best_single_hit": cross,
    }
    (OUT / "advanced_summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps(summary, ensure_ascii=False, indent=2), flush=True)


if __name__ == "__main__":
    main()
