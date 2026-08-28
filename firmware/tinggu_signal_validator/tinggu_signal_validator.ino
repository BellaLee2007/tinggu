/*
 * Tinggu dual-sensor signal validator
 *
 * Piezo: 2000 Hz ADC sampling and hit trigger.
 * MPU-6050: 1000 Hz raw I2C sampling (one reading per two piezo rows).
 * A completed event is transmitted only after sampling has stopped, so serial
 * output cannot disturb the acquisition timing.
 *
 * Commands (newline terminated): ARM, CAPTURE, RECALIBRATE, STATUS
 */

#include <Arduino.h>
#include <Wire.h>
#include <math.h>

// ---------------------------------------------------------------------------
// User-editable hardware interface settings
// ---------------------------------------------------------------------------
const int PIEZO_ADC_PIN = 34;
const int MPU_SDA_PIN = 21;       // Change this to match the actual wiring.
const int MPU_SCL_PIN = 22;       // Change this to match the actual wiring.
const uint8_t MPU_ADDRESS = 0;    // 0 = auto-detect 0x68 or 0x69.
const uint32_t SERIAL_BAUD = 460800;
const uint32_t I2C_CLOCK_HZ = 400000;

// ---------------------------------------------------------------------------
// Sampling and trigger settings
// ---------------------------------------------------------------------------
const uint32_t PIEZO_SAMPLE_RATE_HZ = 2000;
const uint32_t MPU_SAMPLE_RATE_HZ = 1000;
const uint32_t SAMPLE_PERIOD_US = 1000000UL / PIEZO_SAMPLE_RATE_HZ;
const uint16_t PRE_TRIGGER_SAMPLES = 200;   // 100 ms at 2000 Hz
const uint16_t POST_TRIGGER_SAMPLES = 1800; // t=0 through the next 900 ms
const uint16_t EVENT_SAMPLES = PRE_TRIGGER_SAMPLES + POST_TRIGGER_SAMPLES;
const uint16_t MIN_TRIGGER_COUNTS = 80;
const float TRIGGER_NOISE_MULTIPLIER = 6.0f;
const uint32_t CALIBRATION_MS = 1000;

// MPU-6050 conversion factors for +/-8 g and +/-500 degrees/s.
const float ACCEL_LSB_PER_G = 4096.0f;
const float GYRO_LSB_PER_DPS = 65.5f;

struct SampleRow {
  uint32_t tick;
  uint16_t piezoRaw;
  int16_t ax;
  int16_t ay;
  int16_t az;
  int16_t gx;
  int16_t gy;
  int16_t gz;
  uint8_t mpuValid;
};

enum CaptureState {
  STATE_IDLE,
  STATE_ARMING,
  STATE_ARMED,
  STATE_CAPTURING,
  STATE_TRANSFERRING,
  STATE_HOLDING
};

SampleRow preBuffer[PRE_TRIGGER_SAMPLES];
SampleRow eventBuffer[EVENT_SAMPLES];

CaptureState captureState = STATE_IDLE;
uint16_t preCount = 0;
uint16_t preWriteIndex = 0;
uint16_t eventCount = 0;
uint32_t triggerTick = 0;
uint32_t nextSampleUs = 0;
uint32_t gridTick = 0;
uint32_t eventId = 0;
uint32_t schedulerMissedTotal = 0;
uint32_t eventMissedTicks = 0;
uint32_t eventMpuFailures = 0;
bool manualCaptureRequested = false;

uint8_t activeMpuAddress = 0;
bool mpuAvailable = false;
float piezoBaseline = 0.0f;
float piezoNoiseRms = 0.0f;
uint16_t piezoThreshold = MIN_TRIGGER_COUNTS;
float mpuMeanAx = 0.0f;
float mpuMeanAy = 0.0f;
float mpuMeanAz = 0.0f;
float mpuMeanGx = 0.0f;
float mpuMeanGy = 0.0f;
float mpuMeanGz = 0.0f;

char commandBuffer[32];
uint8_t commandLength = 0;

const char *stateName(CaptureState state) {
  switch (state) {
    case STATE_IDLE: return "IDLE";
    case STATE_ARMING: return "ARMING";
    case STATE_ARMED: return "ARMED";
    case STATE_CAPTURING: return "CAPTURING";
    case STATE_TRANSFERRING: return "TRANSFERRING";
    case STATE_HOLDING: return "HOLDING";
    default: return "UNKNOWN";
  }
}

bool mpuReadBytes(uint8_t address, uint8_t reg, uint8_t *data, size_t length) {
  Wire.beginTransmission(address);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }

  size_t received = Wire.requestFrom((uint8_t)address, (uint8_t)length, (uint8_t)true);
  if (received != length) {
    while (Wire.available()) {
      Wire.read();
    }
    return false;
  }

  for (size_t i = 0; i < length; ++i) {
    if (!Wire.available()) {
      return false;
    }
    data[i] = (uint8_t)Wire.read();
  }
  return true;
}

bool mpuWriteByte(uint8_t address, uint8_t reg, uint8_t value) {
  Wire.beginTransmission(address);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission(true) == 0;
}

bool probeMpuAddress(uint8_t address) {
  uint8_t whoAmI = 0;
  if (!mpuReadBytes(address, 0x75, &whoAmI, 1)) {
    return false;
  }
  return whoAmI == 0x68 || whoAmI == 0x69;
}

bool configureMpu() {
  activeMpuAddress = 0;

  if (MPU_ADDRESS != 0) {
    if (probeMpuAddress(MPU_ADDRESS)) {
      activeMpuAddress = MPU_ADDRESS;
    }
  } else if (probeMpuAddress(0x68)) {
    activeMpuAddress = 0x68;
  } else if (probeMpuAddress(0x69)) {
    activeMpuAddress = 0x69;
  }

  if (activeMpuAddress == 0) {
    return false;
  }

  // Clock source = X gyro, sample divider = 0 (1 kHz with DLPF enabled),
  // DLPF_CFG = 1 (~184 Hz accel bandwidth), gyro +/-500 dps, accel +/-8 g.
  if (!mpuWriteByte(activeMpuAddress, 0x6B, 0x01)) return false;
  delay(10);
  if (!mpuWriteByte(activeMpuAddress, 0x19, 0x00)) return false;
  if (!mpuWriteByte(activeMpuAddress, 0x1A, 0x01)) return false;
  if (!mpuWriteByte(activeMpuAddress, 0x1B, 0x08)) return false;
  if (!mpuWriteByte(activeMpuAddress, 0x1C, 0x10)) return false;
  return true;
}

int16_t combineInt16(uint8_t highByte, uint8_t lowByte) {
  return (int16_t)(((uint16_t)highByte << 8) | lowByte);
}

bool readMpuRaw(SampleRow &row) {
  row.ax = 0;
  row.ay = 0;
  row.az = 0;
  row.gx = 0;
  row.gy = 0;
  row.gz = 0;
  row.mpuValid = 0;

  if (!mpuAvailable) {
    return false;
  }

  uint8_t data[14];
  if (!mpuReadBytes(activeMpuAddress, 0x3B, data, sizeof(data))) {
    return false;
  }

  row.ax = combineInt16(data[0], data[1]);
  row.ay = combineInt16(data[2], data[3]);
  row.az = combineInt16(data[4], data[5]);
  row.gx = combineInt16(data[8], data[9]);
  row.gy = combineInt16(data[10], data[11]);
  row.gz = combineInt16(data[12], data[13]);
  row.mpuValid = 1;
  return true;
}

void printMpuStatus() {
  if (mpuAvailable) {
    Serial.print("#MPU,FOUND,0x");
    if (activeMpuAddress < 0x10) Serial.print('0');
    Serial.println(activeMpuAddress, HEX);
  } else {
    Serial.println("#MPU_NOT_FOUND,0x00");
  }
}

void printState() {
  Serial.print("#STATE,");
  Serial.println(stateName(captureState));
}

void printStatus() {
  Serial.print("#STATUS,state=");
  Serial.print(stateName(captureState));
  Serial.print(",mpu=");
  Serial.print(mpuAvailable ? 1 : 0);
  Serial.print(",address=0x");
  if (activeMpuAddress < 0x10) Serial.print('0');
  Serial.print(activeMpuAddress, HEX);
  Serial.print(",piezo_rate=");
  Serial.print(PIEZO_SAMPLE_RATE_HZ);
  Serial.print(",mpu_rate=");
  Serial.print(MPU_SAMPLE_RATE_HZ);
  Serial.print(",baseline=");
  Serial.print(piezoBaseline, 2);
  Serial.print(",noise_rms=");
  Serial.print(piezoNoiseRms, 2);
  Serial.print(",threshold=");
  Serial.print(piezoThreshold);
  Serial.print(",missed_total=");
  Serial.println(schedulerMissedTotal);
}

void calibrateSensors() {
  captureState = STATE_IDLE;
  manualCaptureRequested = false;
  Serial.println("#CALIBRATING,KEEP_STILL,1000");

  // Welford online variance for the piezo channel.
  double piezoMean = 0.0;
  double piezoM2 = 0.0;
  uint32_t piezoSamples = 0;

  int64_t sumAx = 0;
  int64_t sumAy = 0;
  int64_t sumAz = 0;
  int64_t sumGx = 0;
  int64_t sumGy = 0;
  int64_t sumGz = 0;
  uint32_t mpuSamples = 0;

  uint32_t calibrationStart = micros();
  uint32_t nextDue = calibrationStart;
  uint32_t localTick = 0;

  while ((uint32_t)(micros() - calibrationStart) < CALIBRATION_MS * 1000UL) {
    if ((int32_t)(micros() - nextDue) < 0) {
      continue;
    }
    nextDue += SAMPLE_PERIOD_US;

    int raw = analogRead(PIEZO_ADC_PIN);
    ++piezoSamples;
    double difference = raw - piezoMean;
    piezoMean += difference / piezoSamples;
    piezoM2 += difference * (raw - piezoMean);

    if ((localTick & 1U) == 0U && mpuAvailable) {
      SampleRow row = {};
      if (readMpuRaw(row)) {
        sumAx += row.ax;
        sumAy += row.ay;
        sumAz += row.az;
        sumGx += row.gx;
        sumGy += row.gy;
        sumGz += row.gz;
        ++mpuSamples;
      }
    }
    ++localTick;
  }

  piezoBaseline = piezoSamples > 0 ? (float)piezoMean : 0.0f;
  piezoNoiseRms = piezoSamples > 1 ? (float)sqrt(piezoM2 / (piezoSamples - 1)) : 0.0f;
  float calculatedThreshold = piezoNoiseRms * TRIGGER_NOISE_MULTIPLIER;
  if (calculatedThreshold < MIN_TRIGGER_COUNTS) calculatedThreshold = MIN_TRIGGER_COUNTS;
  if (calculatedThreshold > 4095.0f) calculatedThreshold = 4095.0f;
  piezoThreshold = (uint16_t)ceilf(calculatedThreshold);

  if (mpuSamples > 0) {
    mpuMeanAx = (float)sumAx / mpuSamples;
    mpuMeanAy = (float)sumAy / mpuSamples;
    mpuMeanAz = (float)sumAz / mpuSamples;
    mpuMeanGx = (float)sumGx / mpuSamples;
    mpuMeanGy = (float)sumGy / mpuSamples;
    mpuMeanGz = (float)sumGz / mpuSamples;
  } else {
    mpuMeanAx = mpuMeanAy = mpuMeanAz = 0.0f;
    mpuMeanGx = mpuMeanGy = mpuMeanGz = 0.0f;
  }

  Serial.print("#CALIBRATION,baseline=");
  Serial.print(piezoBaseline, 3);
  Serial.print(",noise_rms=");
  Serial.print(piezoNoiseRms, 3);
  Serial.print(",threshold=");
  Serial.print(piezoThreshold);
  Serial.print(",mpu_samples=");
  Serial.print(mpuSamples);
  Serial.print(",mpu_mean=");
  Serial.print(mpuMeanAx, 2); Serial.print('|');
  Serial.print(mpuMeanAy, 2); Serial.print('|');
  Serial.print(mpuMeanAz, 2); Serial.print('|');
  Serial.print(mpuMeanGx, 2); Serial.print('|');
  Serial.print(mpuMeanGy, 2); Serial.print('|');
  Serial.println(mpuMeanGz, 2);

  nextSampleUs = micros() + SAMPLE_PERIOD_US;
  printState();
}

void armCapture() {
  preCount = 0;
  preWriteIndex = 0;
  eventCount = 0;
  eventMissedTicks = 0;
  eventMpuFailures = 0;
  manualCaptureRequested = false;
  captureState = STATE_ARMING;
  Serial.println("#ACK,ARM");
  printState();
}

void pushPreSample(const SampleRow &row) {
  preBuffer[preWriteIndex] = row;
  preWriteIndex = (preWriteIndex + 1) % PRE_TRIGGER_SAMPLES;
  if (preCount < PRE_TRIGGER_SAMPLES) {
    ++preCount;
  }
  if (captureState == STATE_ARMING && preCount == PRE_TRIGGER_SAMPLES) {
    captureState = STATE_ARMED;
    printState();
  }
}

void beginEvent(const SampleRow &triggerSample) {
  if (preCount < PRE_TRIGGER_SAMPLES) {
    return;
  }

  eventCount = 0;
  // With a full circular buffer, preWriteIndex points to its oldest row.
  for (uint16_t i = 0; i < PRE_TRIGGER_SAMPLES; ++i) {
    uint16_t source = (preWriteIndex + i) % PRE_TRIGGER_SAMPLES;
    eventBuffer[eventCount++] = preBuffer[source];
  }

  triggerTick = triggerSample.tick;
  eventBuffer[eventCount++] = triggerSample;
  eventMissedTicks = 0;
  eventMpuFailures = (mpuAvailable && (triggerSample.tick & 1U) == 0U && !triggerSample.mpuValid) ? 1 : 0;
  manualCaptureRequested = false;
  captureState = STATE_CAPTURING;
  Serial.println("#TRIGGER");
}

uint32_t fnv1aByte(uint32_t checksum, uint8_t value) {
  checksum ^= value;
  return checksum * 16777619UL;
}

uint32_t checksumUInt16(uint32_t checksum, uint16_t value) {
  checksum = fnv1aByte(checksum, (uint8_t)(value & 0xFF));
  checksum = fnv1aByte(checksum, (uint8_t)((value >> 8) & 0xFF));
  return checksum;
}

uint32_t checksumUInt32(uint32_t checksum, uint32_t value) {
  checksum = fnv1aByte(checksum, (uint8_t)(value & 0xFF));
  checksum = fnv1aByte(checksum, (uint8_t)((value >> 8) & 0xFF));
  checksum = fnv1aByte(checksum, (uint8_t)((value >> 16) & 0xFF));
  checksum = fnv1aByte(checksum, (uint8_t)((value >> 24) & 0xFF));
  return checksum;
}

uint32_t eventChecksum() {
  uint32_t checksum = 2166136261UL;
  for (uint16_t i = 0; i < eventCount; ++i) {
    const SampleRow &row = eventBuffer[i];
    int32_t timeUs = (int32_t)(row.tick - triggerTick) * (int32_t)SAMPLE_PERIOD_US;
    checksum = checksumUInt32(checksum, (uint32_t)timeUs);
    checksum = checksumUInt16(checksum, row.piezoRaw);
    checksum = fnv1aByte(checksum, row.mpuValid);
    checksum = checksumUInt16(checksum, (uint16_t)row.ax);
    checksum = checksumUInt16(checksum, (uint16_t)row.ay);
    checksum = checksumUInt16(checksum, (uint16_t)row.az);
    checksum = checksumUInt16(checksum, (uint16_t)row.gx);
    checksum = checksumUInt16(checksum, (uint16_t)row.gy);
    checksum = checksumUInt16(checksum, (uint16_t)row.gz);
  }
  return checksum;
}

void transmitEvent() {
  captureState = STATE_TRANSFERRING;
  printState();
  ++eventId;

  Serial.print("#BEGIN,");
  Serial.print(eventId);
  Serial.print(','); Serial.print(EVENT_SAMPLES);
  Serial.print(','); Serial.print(PRE_TRIGGER_SAMPLES);
  Serial.print(','); Serial.print(POST_TRIGGER_SAMPLES);
  Serial.print(','); Serial.print(PIEZO_SAMPLE_RATE_HZ);
  Serial.print(','); Serial.print(MPU_SAMPLE_RATE_HZ);
  Serial.print(','); Serial.print(piezoBaseline, 3);
  Serial.print(','); Serial.print(piezoThreshold);
  Serial.print(','); Serial.print(mpuAvailable ? 1 : 0);
  Serial.print(','); Serial.print(ACCEL_LSB_PER_G, 1);
  Serial.print(','); Serial.print(GYRO_LSB_PER_DPS, 1);
  Serial.print(','); Serial.print(eventMissedTicks);
  Serial.print(','); Serial.println(eventMpuFailures);

  for (uint16_t i = 0; i < eventCount; ++i) {
    const SampleRow &row = eventBuffer[i];
    int32_t timeUs = (int32_t)(row.tick - triggerTick) * (int32_t)SAMPLE_PERIOD_US;
    Serial.print("D,");
    Serial.print(i);
    Serial.print(','); Serial.print(timeUs);
    Serial.print(','); Serial.print(row.piezoRaw);
    Serial.print(','); Serial.print(row.mpuValid);
    Serial.print(','); Serial.print(row.ax);
    Serial.print(','); Serial.print(row.ay);
    Serial.print(','); Serial.print(row.az);
    Serial.print(','); Serial.print(row.gx);
    Serial.print(','); Serial.print(row.gy);
    Serial.print(','); Serial.println(row.gz);
  }

  uint32_t checksum = eventChecksum();
  char checksumText[9];
  snprintf(checksumText, sizeof(checksumText), "%08lX", (unsigned long)checksum);
  Serial.print("#END,");
  Serial.print(eventId);
  Serial.print(','); Serial.print(eventCount);
  Serial.print(','); Serial.println(checksumText);
  Serial.flush();

  captureState = STATE_HOLDING;
  printState();
}

void acquireOneSample(uint32_t tick) {
  SampleRow row = {};
  row.tick = tick;
  row.piezoRaw = (uint16_t)analogRead(PIEZO_ADC_PIN);

  if ((tick & 1U) == 0U) {
    bool ok = readMpuRaw(row);
    if (captureState == STATE_CAPTURING && mpuAvailable && !ok) {
      ++eventMpuFailures;
    }
  }

  if (captureState == STATE_ARMING || captureState == STATE_ARMED) {
    int amplitude = abs((int)row.piezoRaw - (int)lroundf(piezoBaseline));
    bool automaticTrigger = captureState == STATE_ARMED && amplitude >= piezoThreshold;
    bool manualTrigger = captureState == STATE_ARMED && manualCaptureRequested;
    if (automaticTrigger || manualTrigger) {
      beginEvent(row);
    } else {
      pushPreSample(row);
    }
    return;
  }

  if (captureState == STATE_CAPTURING) {
    if (eventCount < EVENT_SAMPLES) {
      eventBuffer[eventCount++] = row;
    }
    if (eventCount >= EVENT_SAMPLES) {
      transmitEvent();
      nextSampleUs = micros() + SAMPLE_PERIOD_US;
    }
  }
}

void handleCommand(const char *command) {
  if (strcmp(command, "ARM") == 0) {
    armCapture();
  } else if (strcmp(command, "CAPTURE") == 0) {
    if (captureState == STATE_ARMED) {
      manualCaptureRequested = true;
      Serial.println("#ACK,CAPTURE");
    } else {
      Serial.print("#ERROR,CAPTURE_REQUIRES_ARMED,current=");
      Serial.println(stateName(captureState));
    }
  } else if (strcmp(command, "RECALIBRATE") == 0) {
    Serial.println("#ACK,RECALIBRATE");
    calibrateSensors();
  } else if (strcmp(command, "STATUS") == 0) {
    printStatus();
  } else if (command[0] != '\0') {
    Serial.print("#ERROR,UNKNOWN_COMMAND,");
    Serial.println(command);
  }
}

void processSerialCommands() {
  while (Serial.available() > 0) {
    char value = (char)Serial.read();
    if (value == '\r') continue;
    if (value == '\n') {
      commandBuffer[commandLength] = '\0';
      for (uint8_t i = 0; i < commandLength; ++i) {
        if (commandBuffer[i] >= 'a' && commandBuffer[i] <= 'z') {
          commandBuffer[i] = commandBuffer[i] - 'a' + 'A';
        }
      }
      handleCommand(commandBuffer);
      commandLength = 0;
    } else if (commandLength < sizeof(commandBuffer) - 1) {
      commandBuffer[commandLength++] = value;
    }
  }
}

void setup() {
  pinMode(PIEZO_ADC_PIN, INPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(PIEZO_ADC_PIN, ADC_11db);

  Serial.begin(SERIAL_BAUD);
  delay(300);
  Serial.println();
  Serial.println("#HELLO,TINGGU_SIGNAL_VALIDATOR,1");

  Wire.begin(MPU_SDA_PIN, MPU_SCL_PIN, I2C_CLOCK_HZ);
  Wire.setTimeOut(5);
  mpuAvailable = configureMpu();
  printMpuStatus();
  calibrateSensors();
  printStatus();
  Serial.println("#READY,SEND_ARM");
}

void loop() {
  processSerialCommands();

  if (captureState == STATE_IDLE || captureState == STATE_HOLDING ||
      captureState == STATE_TRANSFERRING) {
    return;
  }

  uint32_t now = micros();
  if ((int32_t)(now - nextSampleUs) < 0) {
    return;
  }

  uint32_t lateness = now - nextSampleUs;
  uint32_t skippedTicks = lateness / SAMPLE_PERIOD_US;
  if (skippedTicks > 0) {
    gridTick += skippedTicks;
    schedulerMissedTotal += skippedTicks;
    if (captureState == STATE_CAPTURING) {
      eventMissedTicks += skippedTicks;
    }
  }
  nextSampleUs += (skippedTicks + 1) * SAMPLE_PERIOD_US;

  acquireOneSample(gridTick);
  ++gridTick;
}
