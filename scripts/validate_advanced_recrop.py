"""Repeated grouped validation for the advanced fixed-window features."""

from __future__ import annotations

import json
from pathlib import Path

import numpy as np
import pandas as pd
from sklearn.base import clone
from sklearn.metrics import confusion_matrix
from sklearn.model_selection import StratifiedGroupKFold

from advanced_recrop_search import (
    LABELS,
    OUT,
    aggregate_measurements,
    assign_groups,
    candidates,
    feature_sets,
    metrics,
)


def run_configuration(frame, feature_name, model_name, block_size, seed):
    grouped = assign_groups(frame, block_size)
    features = feature_sets(grouped)[feature_name]
    model = candidates(len(features))[model_name]
    y = grouped.label_id.to_numpy(int)
    groups = grouped.group.to_numpy()
    splitter = StratifiedGroupKFold(n_splits=5, shuffle=True, random_state=seed)
    probabilities = np.zeros((len(grouped), len(LABELS)))
    folds = np.full(len(grouped), -1)
    for fold, (train, test) in enumerate(splitter.split(grouped, y, groups)):
        fitted = clone(model).fit(grouped.iloc[train][features], y[train])
        probabilities[test] = fitted.predict_proba(grouped.iloc[test][features])
        folds[test] = fold
    hit = metrics(y, np.argmax(probabilities, axis=1))
    measurement = aggregate_measurements(grouped, probabilities, folds)
    measurement_score = metrics(measurement.label_id, measurement.pred_id)
    triples = measurement[measurement.n_hits == 3]
    triple_score = metrics(triples.label_id, triples.pred_id)
    return {
        "feature_set": feature_name,
        "model": model_name,
        "block_size": block_size,
        "seed": seed,
        "hit_accuracy": hit["accuracy"],
        "measurement_accuracy": measurement_score["accuracy"],
        "measurement_macro_f1": measurement_score["macro_f1"],
        "triple_accuracy": triple_score["accuracy"],
        "triple_macro_f1": triple_score["macro_f1"],
    }, measurement


def main():
    features = pd.read_csv(OUT / "advanced_recrop_features.csv")
    today = features[features.day == "2026-09-03"].copy().reset_index(drop=True)
    configurations = [
        ("spectral_transfer", "svm_rbf_c10"),
        ("spectral_transfer", "shrinkage_lda"),
        ("all_recropped", "shrinkage_lda"),
    ]
    rows = []
    combined_predictions = []
    for block_size in [2, 4, 6, 8]:
        for seed in [11, 29, 47, 83, 131]:
            seed_measurements = []
            for feature_name, model_name in configurations:
                row, measurement = run_configuration(today, feature_name, model_name, block_size, seed)
                rows.append(row)
                seed_measurements.append(measurement.set_index("measurement_id"))
                print(f"block={block_size} seed={seed} {feature_name}/{model_name}: measurement={row['measurement_accuracy']:.3f}, triple={row['triple_accuracy']:.3f}", flush=True)
            common = seed_measurements[0].index
            for item in seed_measurements[1:]:
                common = common.intersection(item.index)
            averaged = sum(item.loc[common, [f"prob_{label}" for label in LABELS]].to_numpy(float) for item in seed_measurements) / len(seed_measurements)
            truth = seed_measurements[0].loc[common]
            pred = np.argmax(averaged, axis=1)
            score = metrics(truth.label_id, pred)
            triple_mask = truth.n_hits.to_numpy(int) == 3
            triple_score = metrics(truth.label_id.to_numpy(int)[triple_mask], pred[triple_mask])
            rows.append({
                "feature_set": "fixed_three_model_ensemble",
                "model": "soft_vote",
                "block_size": block_size,
                "seed": seed,
                "hit_accuracy": np.nan,
                "measurement_accuracy": score["accuracy"],
                "measurement_macro_f1": score["macro_f1"],
                "triple_accuracy": triple_score["accuracy"],
                "triple_macro_f1": triple_score["macro_f1"],
            })
            for measurement_id, true_id, n_hits, predicted_id in zip(common, truth.label_id, truth.n_hits, pred):
                combined_predictions.append({
                    "block_size": block_size,
                    "seed": seed,
                    "measurement_id": measurement_id,
                    "true_id": int(true_id),
                    "n_hits": int(n_hits),
                    "predicted_id": int(predicted_id),
                })

    results = pd.DataFrame(rows)
    results.to_csv(OUT / "repeated_group_validation.csv", index=False)
    pd.DataFrame(combined_predictions).to_csv(OUT / "repeated_ensemble_predictions.csv", index=False)
    aggregate = results.groupby(["feature_set", "model", "block_size"], as_index=False).agg(
        measurement_accuracy_mean=("measurement_accuracy", "mean"),
        measurement_accuracy_std=("measurement_accuracy", "std"),
        triple_accuracy_mean=("triple_accuracy", "mean"),
        triple_accuracy_std=("triple_accuracy", "std"),
        measurement_macro_f1_mean=("measurement_macro_f1", "mean"),
        triple_macro_f1_mean=("triple_macro_f1", "mean"),
    )
    aggregate.to_csv(OUT / "repeated_group_summary.csv", index=False)
    best = aggregate.sort_values(["measurement_accuracy_mean", "triple_accuracy_mean"], ascending=False).iloc[0]
    summary = {
        "validation_runs": int(len(results)),
        "seeds": [11, 29, 47, 83, 131],
        "block_sizes": [2, 4, 6, 8],
        "best_repeated_configuration": best.to_dict(),
        "ensemble_by_block_size": aggregate[aggregate.feature_set == "fixed_three_model_ensemble"].to_dict("records"),
    }
    (OUT / "repeated_group_summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps(summary, ensure_ascii=False, indent=2), flush=True)


if __name__ == "__main__":
    main()
