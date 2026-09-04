#pragma once

#include <math.h>
#include "model_parameters.h"

enum TingguModelClass {
  TINGGU_MODEL_UNCERTAIN = -2,
  TINGGU_MODEL_UNTRAINED = -1,
  TINGGU_TIGHT = 0,
  TINGGU_MEDIUM = 1,
  TINGGU_LOOSE = 2
};

struct TingguModelOutput {
  TingguModelClass classification;
  float confidence;
  float probabilities[TINGGU_MODEL_CLASS_COUNT];
};

inline TingguModelOutput emptyTingguModelOutput() {
  TingguModelOutput output = {};
  output.classification = TINGGU_MODEL_UNTRAINED;
  return output;
}

// Exact deployment form of the StandardScaler + LDA model exported by the
// model teammate. Feature extraction and ordering live in the main sketch.
inline TingguModelOutput inferTingguModel(
    const float features[TINGGU_MODEL_FEATURE_COUNT]) {
  TingguModelOutput output = emptyTingguModelOutput();
  float scores[TINGGU_MODEL_CLASS_COUNT];
  for (unsigned c = 0; c < TINGGU_MODEL_CLASS_COUNT; ++c) {
    scores[c] = TINGGU_MODEL_INTERCEPTS[c];
  }

  for (unsigned i = 0; i < TINGGU_MODEL_FEATURE_COUNT; ++i) {
    float value = isfinite(features[i]) ? features[i] : TINGGU_MODEL_IMPUTER_MEDIAN[i];
    float scale = fabsf(TINGGU_MODEL_SCALER_SCALE[i]) > 1e-12f ? TINGGU_MODEL_SCALER_SCALE[i] : 1.0f;
    float standardized = (value - TINGGU_MODEL_SCALER_MEAN[i]) / scale;
    for (unsigned c = 0; c < TINGGU_MODEL_CLASS_COUNT; ++c) {
      scores[c] += TINGGU_MODEL_COEFFICIENTS[c * TINGGU_MODEL_FEATURE_COUNT + i] * standardized;
    }
  }

  float maximum = scores[0];
  for (unsigned c = 1; c < TINGGU_MODEL_CLASS_COUNT; ++c) maximum = fmaxf(maximum, scores[c]);
  float denominator = 0.0f;
  for (unsigned c = 0; c < TINGGU_MODEL_CLASS_COUNT; ++c) {
    output.probabilities[c] = expf(scores[c] - maximum);
    denominator += output.probabilities[c];
  }
  if (!(denominator > 0.0f) || !isfinite(denominator)) return emptyTingguModelOutput();

  unsigned best = 0;
  for (unsigned c = 0; c < TINGGU_MODEL_CLASS_COUNT; ++c) {
    output.probabilities[c] /= denominator;
    if (output.probabilities[c] > output.probabilities[best]) best = c;
  }
  output.confidence = output.probabilities[best];
  output.classification = output.confidence >= TINGGU_MODEL_CONFIDENCE_THRESHOLD
    ? static_cast<TingguModelClass>(best)
    : TINGGU_MODEL_UNCERTAIN;
  return output;
}

inline TingguModelOutput averageTingguModelOutputs(
    const TingguModelOutput *outputs, unsigned count) {
  TingguModelOutput combined = emptyTingguModelOutput();
  if (outputs == nullptr || count == 0) return combined;
  for (unsigned i = 0; i < count; ++i) {
    if (outputs[i].classification == TINGGU_MODEL_UNTRAINED) return combined;
    for (unsigned c = 0; c < TINGGU_MODEL_CLASS_COUNT; ++c) {
      combined.probabilities[c] += outputs[i].probabilities[c] / count;
    }
  }
  unsigned best = 0;
  for (unsigned c = 1; c < TINGGU_MODEL_CLASS_COUNT; ++c) {
    if (combined.probabilities[c] > combined.probabilities[best]) best = c;
  }
  combined.confidence = combined.probabilities[best];
  combined.classification = combined.confidence >= TINGGU_MODEL_CONFIDENCE_THRESHOLD
    ? static_cast<TingguModelClass>(best)
    : TINGGU_MODEL_UNCERTAIN;
  return combined;
}

inline const char *tingguClassName(TingguModelClass value) {
  switch (value) {
    case TINGGU_TIGHT: return "TIGHT";
    case TINGGU_MEDIUM: return "MEDIUM";
    case TINGGU_LOOSE: return "LOOSE";
    case TINGGU_MODEL_UNCERTAIN: return "UNCERTAIN";
    default: return "MODEL_UNTRAINED";
  }
}
