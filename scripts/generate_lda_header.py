"""Regenerate the ESP32 LDA parameter header from the checked-in JSON export."""

from __future__ import annotations

import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MODEL_JSON = ROOT / "models" / "20260903" / "spectral_lda40_export.json"
OUTPUT = ROOT / "firmware" / "tinggu_edge_runtime" / "model_parameters.h"


def array(name: str, values, columns: int = 4) -> str:
    flat = list(values)
    lines = []
    for start in range(0, len(flat), columns):
        chunk = ", ".join(f"{float(value):.9g}f" for value in flat[start : start + columns])
        lines.append(f"  {chunk},")
    return f"static const float {name}[{len(flat)}] = {{\n" + "\n".join(lines) + "\n};\n"


def main() -> None:
    model = json.loads(MODEL_JSON.read_text(encoding="utf-8"))
    selected = [int(value) for value in model["selected_indices"]]
    names = [str(value) for value in model["selected_features"]]
    medians = [model["imputer_median"][index] for index in selected]
    means = [model["scaler_mean"][index] for index in selected]
    scales = [model["scaler_scale"][index] for index in selected]
    coefficients = [value for row in model["coef"] for value in row]
    name_lines = "\n".join(f'  "{name}",' for name in names)
    content = f"""#pragma once

// Generated from models/20260903/spectral_lda40_export.json.
// Feature extraction order is frozen; do not reorder these arrays by hand.
static const char TINGGU_MODEL_VERSION[] = "spectral_lda40_20260903";
static const unsigned TINGGU_MODEL_FEATURE_COUNT = 40;
static const unsigned TINGGU_MODEL_CLASS_COUNT = 3;
static const float TINGGU_MODEL_CONFIDENCE_THRESHOLD = 0.50f;

static const char *const TINGGU_MODEL_FEATURE_NAMES[40] = {{
{name_lines}
}};

{array("TINGGU_MODEL_IMPUTER_MEDIAN", medians)}
{array("TINGGU_MODEL_SCALER_MEAN", means)}
{array("TINGGU_MODEL_SCALER_SCALE", scales)}
{array("TINGGU_MODEL_COEFFICIENTS", coefficients, 5)}
{array("TINGGU_MODEL_INTERCEPTS", model["intercept"], 3)}
"""
    OUTPUT.write_text(content, encoding="utf-8", newline="\n")
    print(f"generated {OUTPUT} ({len(names)} features, {len(coefficients)} coefficients)")


if __name__ == "__main__":
    main()
