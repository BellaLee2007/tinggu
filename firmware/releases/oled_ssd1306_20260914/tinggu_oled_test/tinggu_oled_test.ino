#include "display_interface.h"
void setup() {
  Serial.begin(460800);
  delay(300);
  bool found = tingguDisplayBegin();
  Serial.printf("#OLED,%s,SDA=8,SCL=9,address=0x%02X\n", found ? "READY" : "NOT_FOUND", tingguOledAddress);
  Serial.println("#OLED_TEST,DEMO_ONLY,NO_SENSOR_OR_MODEL");
}
void loop() {
  static unsigned lastStep = 99;
  unsigned step = (millis() / 2500) % 6;
  if (step != lastStep) {
    if (step == 0) { tingguDisplayProgress(0,3); tingguDisplayState("IDLE"); }
    if (step == 1) tingguDisplayState("PREPARE_STRIKE");
    if (step == 2) { tingguDisplayProgress(1,3); tingguDisplayState("STRIKE_NOW"); }
    if (step == 3) { tingguDisplayProgress(2,3); tingguDisplayState("ANALYZING"); }
    if (step == 4) { tingguDisplayProgress(3,3); tingguDisplayResult("DEMO ONLY",0); }
    if (step == 5) tingguDisplayState("RESULT_HOLDING");
    lastStep = step;
  }
  tingguDisplayPoll();
  delay(2);
}
