#pragma once

#include <math.h>
#include "tree_model_parameters.h"

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

// Producer is a custom double-precision CART, not sklearn. Never cast input to float.
inline int tingguTreeLeaf(const double *features) {
  if (!features) return -1;
  for (unsigned i=0;i<TINGGU_MODEL_FEATURE_COUNT;++i) if (!isfinite(features[i])) return -1;
  int at=0;
  for (unsigned steps=0;steps<sizeof(TINGGU_TREE_NODES)/sizeof(TINGGU_TREE_NODES[0]);++steps) {
    const TingguTreeNode &n=TINGGU_TREE_NODES[at];
    if(n.feature<0)return at;
    at=features[n.feature]<=n.threshold?n.left:n.right;
  }
  return -1;
}
inline TingguModelOutput inferTingguModel(const double features[TINGGU_MODEL_FEATURE_COUNT]) {
  TingguModelOutput output=emptyTingguModelOutput();
  int at=tingguTreeLeaf(features);if(at<0)return output;
  const TingguTreeNode &leaf=TINGGU_TREE_NODES[at];
  unsigned total=leaf.counts[0]+leaf.counts[1]+leaf.counts[2];if(!total)return output;
  for(unsigned c=0;c<3;++c)output.probabilities[c]=double(leaf.counts[c])/total;
  output.confidence=output.probabilities[leaf.prediction];
  output.classification=output.confidence>=TINGGU_MODEL_CONFIDENCE_THRESHOLD?
    static_cast<TingguModelClass>(leaf.prediction):TINGGU_MODEL_UNCERTAIN;
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
