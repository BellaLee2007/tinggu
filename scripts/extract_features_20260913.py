"""Convert Tinggu event waveforms into the 14-feature training contract.

Each waveform CSV is produced by TingguSignalWorkbench and is joined through a
manifest so ``measurement_id`` remains available for leakage-resistant grouped
validation. Features are calculated from the 0-300 ms post-trigger interval.
"""

from __future__ import annotations

import argparse
import math
import os
from pathlib import Path


ROOT = Path(os.environ.get("TINGGU_ROOT", Path(__file__).resolve().parents[1]))
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
POST_END_US = 300_000.0
LOW_HIGH_SPLIT_HZ = 250.0
MIN_SPECTRAL_HZ = 5.0
DECAY_BIN_MS = 5.0
LABEL_ALIASES = {
    "tight": "tight",
    "firm": "tight",
    "medium": "medium",
    "slight": "medium",
    "loose": "loose",
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--manifest",
        type=Path,
        default=ROOT / "data" / "raw" / "20260911" / "manifest.csv",
        help="CSV containing measurement_id, label, and file/sample_file.",
    )
    parser.add_argument(
        "--waveform-dir",
        type=Path,
        default=None,
        help="Base directory for waveform paths; defaults to the manifest directory.",
    )
    parser.add_argument(
        "--output-csv",
        type=Path,
        default=ROOT / "data" / "processed" / "training_dataset_20260911.csv",
        help="Output feature table for all manifest rows.",
    )
    parser.add_argument(
        "--strict-output-csv",
        type=Path,
        default=ROOT / "data" / "processed" / "training_dataset_20260911_perfect_strict.csv",
        help="Output zero-anomaly, zero-drop subset when manifest flags are present.",
    )
    return parser.parse_args()


def load_dependencies() -> None:
    global np, pd
    try:
        import numpy as np_module
        import pandas as pd_module
    except ImportError as error:
        raise RuntimeError(
            "Feature extraction requires numpy and pandas in the project environment."
        ) from error
    np = np_module
    pd = pd_module


def rms(values) -> float:
    array = np.asarray(values, dtype=float)
    return float(np.sqrt(np.mean(array * array))) if array.size else 0.0


def required_numeric(frame, columns: list[str], path: Path):
    missing = [column for column in columns if column not in frame.columns]
    if missing:
        raise ValueError(f"{path} is missing columns: {missing}")
    numeric = frame[columns].apply(pd.to_numeric, errors="coerce")
    return numeric


def piezo_signal(frame, path: Path):
    if "time_us" not in frame.columns:
        raise ValueError(f"{path} is missing time_us")
    time_us = pd.to_numeric(frame["time_us"], errors="coerce")
    if time_us.isna().any():
        raise ValueError(f"{path} contains missing or invalid time_us values")
    if "piezo_delta" in frame.columns:
        signal = pd.to_numeric(frame["piezo_delta"], errors="coerce")
    elif "piezo_raw" in frame.columns:
        raw = pd.to_numeric(frame["piezo_raw"], errors="coerce")
        pre = raw[time_us < 0].dropna()
        if pre.empty:
            raise ValueError(f"{path} has no pre-trigger samples for piezo baseline")
        signal = raw - float(pre.mean())
    else:
        raise ValueError(f"{path} requires piezo_delta or piezo_raw")
    if signal.isna().any():
        raise ValueError(f"{path} contains missing or invalid piezo samples")
    order = np.argsort(time_us.to_numpy(dtype=float))
    return time_us.to_numpy(dtype=float)[order], signal.to_numpy(dtype=float)[order]


def uniform_post_signal(time_us, signal, path: Path):
    keep = (time_us >= 0.0) & (time_us <= POST_END_US)
    post_time = time_us[keep]
    post_signal = signal[keep]
    if len(post_time) < 32:
        raise ValueError(f"{path} has fewer than 32 post-trigger samples")
    differences = np.diff(post_time)
    differences = differences[differences > 0]
    if not len(differences):
        raise ValueError(f"{path} has no increasing time_us samples")
    step_us = float(np.median(differences))
    sample_rate_hz = 1_000_000.0 / step_us
    grid = np.arange(0.0, POST_END_US + step_us * 0.25, step_us)
    values = np.interp(grid, post_time, post_signal)
    return grid, values, sample_rate_hz


def spectral_features(values, sample_rate_hz: float) -> tuple[float, float, float]:
    centered = values - float(np.mean(values))
    tapered = centered * np.hanning(len(centered))
    power = np.abs(np.fft.rfft(tapered)) ** 2
    frequencies = np.fft.rfftfreq(len(tapered), d=1.0 / sample_rate_hz)
    valid = frequencies >= MIN_SPECTRAL_HZ
    frequencies = frequencies[valid]
    power = power[valid]
    total = float(power.sum())
    if not power.size or total <= 1e-12:
        return 0.0, 0.0, 0.0
    dominant = float(frequencies[int(np.argmax(power))])
    centroid = float(np.sum(frequencies * power) / total)
    low = float(power[frequencies < LOW_HIGH_SPLIT_HZ].sum())
    high = float(power[frequencies >= LOW_HIGH_SPLIT_HZ].sum())
    return dominant, centroid, low / max(high, 1e-12)


def decay_tau_ms(time_us, values, peak_index: int) -> float:
    start_ms = float(time_us[peak_index] / 1000.0)
    end_ms = POST_END_US / 1000.0
    centers = []
    envelope = []
    left = start_ms
    while left < end_ms:
        right = min(end_ms, left + DECAY_BIN_MS)
        mask = (time_us / 1000.0 >= left) & (time_us / 1000.0 < right)
        if mask.any():
            centers.append((left + right) / 2.0)
            envelope.append(rms(values[mask]))
        left = right
    if len(envelope) < 3:
        return 0.0
    envelope = np.asarray(envelope, dtype=float)
    centers = np.asarray(centers, dtype=float)
    threshold = max(float(envelope.max()) * 0.05, 1e-9)
    usable = envelope >= threshold
    if int(usable.sum()) < 3:
        return 0.0
    slope, _ = np.polyfit(centers[usable] - start_ms, np.log(envelope[usable]), 1)
    if not math.isfinite(float(slope)) or slope >= -1e-12:
        return end_ms - start_ms
    return float(min(end_ms - start_ms, -1.0 / slope))


def mpu_features(frame, path: Path) -> tuple[float, float, float, float, float]:
    time_us = pd.to_numeric(frame["time_us"], errors="coerce")
    post = (time_us >= 0.0) & (time_us <= POST_END_US)
    if "mpu_valid" in frame.columns:
        valid_value = pd.to_numeric(frame["mpu_valid"], errors="coerce").fillna(0.0)
        valid = post & (valid_value > 0.0)
        valid_ratio = float(valid.sum() / max(1, int(post.sum())))
    else:
        valid = post
        valid_ratio = 1.0
    axes = ["ax_g", "ay_g", "az_g", "gx_dps", "gy_dps", "gz_dps"]
    available_axes = [column for column in axes if column in frame.columns]
    if not available_axes:
        return 0.0, 0.0, 0.0, 0.0, 0.0
    numeric = required_numeric(frame, axes, path)
    valid &= numeric.notna().all(axis=1)
    if not valid.any():
        return 0.0, 0.0, 0.0, 0.0, valid_ratio
    acceleration = np.linalg.norm(numeric.loc[valid, axes[:3]].to_numpy(dtype=float), axis=1)
    gyroscope = np.linalg.norm(numeric.loc[valid, axes[3:]].to_numpy(dtype=float), axis=1)
    return (
        float(acceleration.mean()),
        float(acceleration.std(ddof=0)),
        float(gyroscope.mean()),
        float(gyroscope.std(ddof=0)),
        valid_ratio,
    )


def extract_waveform(path: Path) -> dict[str, float]:
    frame = pd.read_csv(path)
    time_us, signal = piezo_signal(frame, path)
    post_time, post_signal, sample_rate_hz = uniform_post_signal(time_us, signal, path)
    peak_index = int(np.argmax(np.abs(post_signal)))
    early = (post_time >= 0.0) & (post_time < 100_000.0)
    late = (post_time >= 100_000.0) & (post_time <= POST_END_US)
    dominant, centroid, ratio = spectral_features(post_signal, sample_rate_hz)
    accel_mean, accel_std, gyro_mean, gyro_std, valid_ratio = mpu_features(frame, path)
    return {
        "peak_abs": float(abs(post_signal[peak_index])),
        "peak_time_ms": float(post_time[peak_index] / 1000.0),
        "rms_0_100ms": rms(post_signal[early]),
        "rms_100_300ms": rms(post_signal[late]),
        "dominant_freq_hz": dominant,
        "spectral_centroid_hz": centroid,
        "low_high_energy_ratio": ratio,
        "decay_tau_ms": decay_tau_ms(post_time, post_signal, peak_index),
        "piezo_std": float(np.std(post_signal, ddof=0)),
        "accel_mag_mean": accel_mean,
        "accel_mag_std": accel_std,
        "gyro_mag_mean": gyro_mean,
        "gyro_mag_std": gyro_std,
        "mpu_valid_ratio": valid_ratio,
    }


def column_name(frame, candidates: list[str], purpose: str) -> str:
    for candidate in candidates:
        if candidate in frame.columns:
            return candidate
    raise ValueError(f"Manifest requires {purpose}; expected one of {candidates}")


def normalize_label(value, row_number: int) -> str:
    key = str(value).strip().lower()
    if key not in LABEL_ALIASES:
        raise ValueError(
            f"Unsupported label at manifest row {row_number}: {value!r}; "
            f"expected one of {sorted(LABEL_ALIASES)}"
        )
    return LABEL_ALIASES[key]


def strict_mask(manifest):
    if "perfect_strict" in manifest.columns:
        values = manifest["perfect_strict"].astype(str).str.lower()
        return values.isin(["1", "true", "yes", "y"])
    anomaly_column = next(
        (name for name in ["anomaly_count", "anomalies"] if name in manifest.columns), None
    )
    drop_column = next(
        (name for name in ["drop_rate_percent", "drop_rate"] if name in manifest.columns), None
    )
    if anomaly_column and drop_column:
        anomalies = pd.to_numeric(manifest[anomaly_column], errors="coerce")
        drops = pd.to_numeric(manifest[drop_column], errors="coerce")
        return anomalies.eq(0) & drops.eq(0)
    return None


def main() -> None:
    args = parse_args()
    load_dependencies()
    manifest_path = args.manifest.resolve()
    if not manifest_path.is_file():
        raise FileNotFoundError(f"Manifest not found: {manifest_path}")
    manifest = pd.read_csv(manifest_path)
    file_column = column_name(manifest, ["file", "sample_file"], "waveform path")
    label_column = column_name(manifest, ["label", "tightness_label", "state_label"], "label")
    if "measurement_id" not in manifest.columns:
        raise ValueError("Manifest requires measurement_id for grouped validation")
    waveform_dir = (
        args.waveform_dir.resolve() if args.waveform_dir else manifest_path.parent
    )

    rows = []
    for index, item in manifest.iterrows():
        relative = Path(str(item[file_column]))
        waveform_path = relative if relative.is_absolute() else waveform_dir / relative
        if not waveform_path.is_file():
            raise FileNotFoundError(f"Waveform not found at manifest row {index + 2}: {waveform_path}")
        rows.append(
            {
                "measurement_id": item["measurement_id"],
                "label": normalize_label(item[label_column], index + 2),
                "sample_file": str(relative).replace("\\", "/"),
                **extract_waveform(waveform_path),
            }
        )
        print(f"[{index + 1}/{len(manifest)}] {relative}", flush=True)

    output = pd.DataFrame(rows, columns=["measurement_id", "label", "sample_file", *FEATURES])
    args.output_csv.parent.mkdir(parents=True, exist_ok=True)
    output.to_csv(args.output_csv, index=False)
    mask = strict_mask(manifest)
    if mask is not None:
        args.strict_output_csv.parent.mkdir(parents=True, exist_ok=True)
        output.loc[mask.to_numpy()].to_csv(args.strict_output_csv, index=False)
        strict_message = f" strict_events={int(mask.sum())} strict_output={args.strict_output_csv}"
    else:
        strict_message = " strict_output=skipped(no strict flags in manifest)"
    print(f"events={len(output)} output={args.output_csv}{strict_message}", flush=True)


if __name__ == "__main__":
    main()
