#pragma once

// Exact OLED hardware has not been frozen. These no-op functions keep display
// calls out of the acquisition task and provide one stable replacement point.
// Replace their bodies only after the module model, address and pins are known.

inline bool tingguDisplayBegin() {
  return false;
}

inline void tingguDisplayState(const char *stateText) {
  (void)stateText;
}

inline void tingguDisplayResult(const char *resultText, float confidence) {
  (void)resultText;
  (void)confidence;
}
