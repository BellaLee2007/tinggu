#pragma once
#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <math.h>
#include <string.h>

// OLED uses controller 1. Wire/controller 0 remains exclusively with the MPU.
static constexpr int TINGGU_OLED_SDA = 8;
static constexpr int TINGGU_OLED_SCL = 9;
static U8G2_SSD1306_128X64_NONAME_F_2ND_HW_I2C tingguOled(U8G2_R0, U8X8_PIN_NONE);
static portMUX_TYPE tingguDisplayMux = portMUX_INITIALIZER_UNLOCKED;
struct TingguDisplayView {
  char text[32] = "BOOT";
  float confidence = 0;
  uint8_t accepted = 0;
  uint8_t required = 3;
  bool result = false;
  uint32_t revision = 1;
};
static TingguDisplayView tingguView;
static bool tingguOledReady = false;
static uint8_t tingguOledAddress = 0;

// These setters only copy state; they never perform I2C or render from a worker.
inline void tingguDisplayState(const char *text) {
  if (!text || strcmp(text, "RESULT_HOLDING") == 0) return;
  portENTER_CRITICAL(&tingguDisplayMux);
  // A late PROCESSING notification must not erase a published result.
  bool reset = !strcmp(text,"IDLE") || !strcmp(text,"CALIBRATING") ||
    !strcmp(text,"PREPARE_STRIKE") || !strcmp(text,"STRIKE_PREPARING") || !strcmp(text,"FAULT");
  if (tingguView.result && !reset) {
    portEXIT_CRITICAL(&tingguDisplayMux);
    return;
  }
  if (tingguView.result || strcmp(tingguView.text, text) != 0) {
    snprintf(tingguView.text, sizeof(tingguView.text), "%s", text);
    tingguView.result = false;
    ++tingguView.revision;
  }
  portEXIT_CRITICAL(&tingguDisplayMux);
}
inline void tingguDisplayResult(const char *text, float confidence) {
  portENTER_CRITICAL(&tingguDisplayMux);
  snprintf(tingguView.text, sizeof(tingguView.text), "%s", text ? text : "MEASUREMENT_INVALID");
  tingguView.confidence = confidence;
  tingguView.result = true;
  ++tingguView.revision;
  portEXIT_CRITICAL(&tingguDisplayMux);
}
inline void tingguDisplayProgress(uint8_t accepted, uint8_t required) {
  portENTER_CRITICAL(&tingguDisplayMux);
  if (tingguView.accepted != accepted || tingguView.required != required) {
    tingguView.accepted = accepted;
    tingguView.required = required;
    ++tingguView.revision;
  }
  portEXIT_CRITICAL(&tingguDisplayMux);
}
inline bool tingguDisplayProbe(uint8_t address) {
  Wire1.beginTransmission(address);
  return Wire1.endTransmission() == 0;
}
inline bool tingguDisplayBegin() {
  if (!Wire1.begin(TINGGU_OLED_SDA, TINGGU_OLED_SCL, 400000)) return false;
  Wire1.setTimeOut(10);
  for (uint8_t address : {uint8_t(0x3C), uint8_t(0x3D)}) {
    if (tingguDisplayProbe(address)) { tingguOledAddress = address; break; }
  }
  if (!tingguOledAddress) return false;
  tingguOled.setI2CAddress(tingguOledAddress << 1);
  tingguOled.setBusClock(400000);
  tingguOled.begin();
  tingguOledReady = true;
  return true;
}
inline const char *tingguDisplayCaption(const char *state) {
  if (!strcmp(state,"CALIBRATING")) return "KEEP STILL";
  if (!strcmp(state,"IDLE")) return "READY";
  if (!strcmp(state,"STRIKE_PREPARING") || !strcmp(state,"PREPARE_STRIKE")) return "PREPARE";
  if (!strcmp(state,"MEASUREMENT_ARMED") || !strcmp(state,"STRIKE_NOW")) return "STRIKE NOW";
  if (!strcmp(state,"MODEL_ANALYZING") || !strcmp(state,"PROCESSING")) return "ANALYZING";
  if (!strcmp(state,"MODEL_UNTRAINED")) return "MODEL NOT READY";
  if (!strcmp(state,"MEASUREMENT_INVALID")) return "INVALID / RETRY";
  if (!strcmp(state,"FAULT")) return "DEVICE FAULT";
  return state;
}
// Call ONLY from Arduino loop. Missing/disconnected OLED never halts measurement.
inline void tingguDisplayPoll() {
  static uint32_t lastMs = 0, drawnRevision = 0;
  if (!tingguOledReady || millis() - lastMs < 150) return;
  TingguDisplayView view;
  portENTER_CRITICAL(&tingguDisplayMux);
  view = tingguView;
  portEXIT_CRITICAL(&tingguDisplayMux);
  if (view.revision == drawnRevision) return;
  lastMs = millis();
  if (!tingguDisplayProbe(tingguOledAddress)) {
    tingguOledReady = false; // Reboot after reconnecting; no continual timeout loop.
    Serial.println("#OLED,DISCONNECTED");
    return;
  }
  tingguOled.clearBuffer();
  tingguOled.setFont(u8g2_font_6x12_tf);
  tingguOled.drawStr(0,11,"TINGGU");
  tingguOled.drawStr(80,11,view.result ? "RESULT" : "STATUS");
  tingguOled.drawHLine(0,14,128);
  const char *caption = tingguDisplayCaption(view.text);
  tingguOled.setFont(strlen(caption) <= 10 ? u8g2_font_9x15B_tf : u8g2_font_6x12_tf);
  tingguOled.drawStr(0,34,caption);
  tingguOled.setFont(u8g2_font_6x12_tf);
  char line[32];
  snprintf(line,sizeof(line),"Valid hits: %u/%u",view.accepted,view.required);
  tingguOled.drawStr(0,49,line);
  bool classified = !strcmp(view.text,"TIGHT") || !strcmp(view.text,"MEDIUM") || !strcmp(view.text,"LOOSE") || !strcmp(view.text,"UNCERTAIN");
  if (view.result && classified && isfinite(view.confidence) && view.confidence >= 0 && view.confidence <= 1) {
    snprintf(line,sizeof(line),"Confidence: %.0f%%",view.confidence * 100);
  } else {
    snprintf(line,sizeof(line),"%s",view.result ? "ARM to test again" : "Use serial to ARM");
  }
  tingguOled.drawStr(0,63,line);
  tingguOled.sendBuffer();
  drawnRevision = view.revision;
}
