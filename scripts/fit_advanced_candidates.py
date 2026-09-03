"""Fit and export the advanced recrop candidates after grouped validation."""

from __future__ import annotations

import sys
from pathlib import Path

# Allow importing the sibling search module when invoked from another folder.
sys.path.insert(0, str(Path(__file__).resolve().parent))

import json

import joblib
import numpy as np
import pandas as pd
from sklearn.base import clone
from sklearn.discriminant_analysis import LinearDiscriminantAnalysis
from sklearn.feature_selection import SelectKBest, f_classif
from sklearn.impute import SimpleImputer
from sklearn.pipeline import Pipeline
from sklearn.preprocessing import StandardScaler

from advanced_recrop_search import CROP_MS, LABELS, OUT, candidates, feature_sets


def export_linear_pipeline(pipeline, all_features):
    imputer = pipeline.named_steps["impute"]
    scaler = pipeline.named_steps["scale"]
    selector = pipeline.named_steps["select"]
    model = pipeline.named_steps["model"]
    selected_indices = np.flatnonzero(selector.get_support())
    return {
        "model_type": "linear_discriminant_analysis",
        "labels": LABELS,
        "crop_ms": CROP_MS,
        "all_input_features": all_features,
        "imputer_median": imputer.statistics_.tolist(),
        "scaler_mean": scaler.mean_.tolist(),
        "scaler_scale": scaler.scale_.tolist(),
        "selected_indices": selected_indices.tolist(),
        "selected_features": [all_features[i] for i in selected_indices],
        "coef": model.coef_.tolist(),
        "intercept": model.intercept_.tolist(),
        "inference": "impute -> standardize -> select -> argmax(coef*x + intercept)",
        "validation_status": "same-day grouped >80%; independent-session validation still required",
    }


def main():
    data = pd.read_csv(OUT / "advanced_recrop_features.csv")
    today = data[data.day == "2026-09-03"].copy().reset_index(drop=True)
    sets = feature_sets(today)
    y = today.label_id.to_numpy(int)

    specs = [
        ("spectral_transfer", "svm_rbf_c10"),
        ("spectral_transfer", "extra_trees"),
        ("all_recropped", "shrinkage_lda"),
    ]
    fitted = []
    for feature_set, model_name in specs:
        features = sets[feature_set]
        model = clone(candidates(len(features))[model_name]).fit(today[features], y)
        fitted.append({"feature_set": feature_set, "model_name": model_name, "features": features, "model": model})

    joblib.dump({
        "models": fitted,
        "labels": LABELS,
        "crop_ms": CROP_MS,
        "aggregation": "mean class probabilities across models, then mean across three hits",
        "confidence_policy": "if max probability < 0.50, return UNCERTAIN",
        "status": "experimental_requires_independent_session_validation",
    }, OUT / "candidate_advanced_ensemble.joblib")

    lda_features = sets["spectral_transfer"]
    lda = clone(candidates(len(lda_features))["shrinkage_lda"]).fit(today[lda_features], y)
    joblib.dump({
        "model": lda,
        "features": lda_features,
        "labels": LABELS,
        "crop_ms": CROP_MS,
        "aggregation": "mean probabilities across three hits",
        "status": "preferred_compact_linear_candidate_requires_independent_session_validation",
    }, OUT / "candidate_spectral_lda.joblib")
    contract = export_linear_pipeline(lda, lda_features)
    (OUT / "spectral_lda_export.json").write_text(json.dumps(contract, ensure_ascii=False, indent=2), encoding="utf-8")
    pd.DataFrame({"selected_feature": contract["selected_features"]}).to_csv(OUT / "spectral_lda_selected_features.csv", index=False)

    # Smaller embedded candidate: 40 selected features still achieved 81.8%
    # repeated grouped measurement accuracy and is a linear 3-class decision.
    lda40 = Pipeline([
        ("impute", SimpleImputer(strategy="median")),
        ("scale", StandardScaler()),
        ("select", SelectKBest(f_classif, k=40)),
        ("model", LinearDiscriminantAnalysis(solver="lsqr", shrinkage="auto")),
    ]).fit(today[lda_features], y)
    joblib.dump({
        "model": lda40,
        "features": lda_features,
        "labels": LABELS,
        "crop_ms": CROP_MS,
        "aggregation": "mean probabilities across three hits",
        "validation": "repeated grouped measurement accuracy 81.8%; complete triple 81.5%",
        "status": "preferred_embedded_candidate_requires_independent_session_validation",
    }, OUT / "candidate_spectral_lda40.joblib")
    contract40 = export_linear_pipeline(lda40, lda_features)
    contract40["validation"] = {
        "measurement_accuracy_mean": 0.8184049079754601,
        "measurement_accuracy_std": 0.012721743161550772,
        "complete_triple_accuracy_mean": 0.8146341463414635,
        "complete_triple_accuracy_std": 0.021815297341461325,
    }
    (OUT / "spectral_lda40_export.json").write_text(json.dumps(contract40, ensure_ascii=False, indent=2), encoding="utf-8")
    pd.DataFrame({"selected_feature": contract40["selected_features"]}).to_csv(OUT / "spectral_lda40_selected_features.csv", index=False)
    print(json.dumps({
        "ensemble_models": [f"{item['feature_set']}/{item['model_name']}" for item in fitted],
        "spectral_lda_input_features": len(lda_features),
        "spectral_lda_selected_features": len(contract["selected_features"]),
        "spectral_lda40_selected_features": len(contract40["selected_features"]),
    }, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
