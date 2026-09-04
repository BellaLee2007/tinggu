/*
 * Tinggu edge runtime - external electromagnetic striker edition
 *
 * Core 0:
 *   ADC continuous DMA, piezo trigger/windowing, MPU-6050 sampling.
 * Core 1:
 *   quality gates, 512-point FFT, f1/tau/Eratio/C, model/display/serial.
 *
 * The striker is powered and triggered independently. There is deliberately no
 * striker GPIO in this firmware. A measurement consists of three external hits.
 *
 * Serial commands: ARM_MEASUREMENT, ABORT, RECALIBRATE, STATUS
 */

#include <Arduino.h>
#include <Wire.h>
#include <math.h>
#include <stdarg.h>
#include "esp_adc/adc_continuous.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_heap_caps.h"
#include "soc/soc_caps.h"
#include "model_interface.h"
#include "display_interface.h"

// ---------------------------------------------------------------------------
// User-editable hardware and experiment configuration
// ---------------------------------------------------------------------------
const int PIEZO_ADC_PIN = 4;        // ESP32-S3 ADC1_CH3; continuous DMA requires ADC1.
const int MPU_SDA_PIN = 17;
const int MPU_SCL_PIN = 15;
const uint8_t MPU_ADDRESS = 0;      // 0 = auto-detect 0x68/0x69.
const int ARM_BUTTON_PIN = -1;      // -1 disables the physical ARM button.
const bool ARM_BUTTON_ACTIVE_LOW = true;

const uint32_t SERIAL_BAUD = 460800;
const uint32_t I2C_CLOCK_HZ = 400000;
#ifndef TINGGU_PIEZO_SAMPLE_RATE_HZ
#define TINGGU_PIEZO_SAMPLE_RATE_HZ 2000UL
#endif
const uint32_t PIEZO_SAMPLE_RATE_HZ = TINGGU_PIEZO_SAMPLE_RATE_HZ;
const uint32_t MPU_SAMPLE_RATE_HZ = 1000;
const uint16_t PRE_TRIGGER_MS = 100;
const uint16_t POST_TRIGGER_MS = 900;
const uint16_t REFRACTORY_MS = 200;
const uint16_t MEASUREMENT_TIMEOUT_MS = 5000;

const uint16_t MIN_TRIGGER_COUNTS = 80;
const float TRIGGER_NOISE_MULTIPLIER = 6.0f;
const float PIEZO_MIN_SNR_DB = 10.0f;
const float MPU_MIN_SNR_DB = 6.0f;
const float MAX_DROP_RATE = 0.01f;
// Correlation is reported for diagnosis, but is not a hard gate: the deployed
// classifier was trained per hit and combines the three probabilities.
const float MIN_CONSISTENCY = -1.0f;

const uint16_t FFT_SIZE = 512;
const uint16_t SIGNATURE_POINTS = 256;
const float SIGNATURE_WINDOW_MS = 250.0f;
const float LOW_BAND_MIN_HZ = 20.0f;
const float LOW_BAND_MAX_HZ = 200.0f;
const float HIGH_BAND_MAX_HZ = 800.0f;

const uint32_t MAX_PIEZO_RATE_HZ = 20000;
const uint32_t MAX_PIEZO_SAMPLES = PIEZO_SAMPLE_RATE_HZ;
const uint32_t PRE_RING_CAPACITY = PIEZO_SAMPLE_RATE_HZ * PRE_TRIGGER_MS / 1000;
const uint16_t MAX_MPU_SAMPLES = 1200;
const uint8_t HITS_PER_MEASUREMENT = 3;

const uint16_t MODEL_CROP_MS = 240;
const uint16_t MODEL_PIEZO_COUNT = PIEZO_SAMPLE_RATE_HZ * MODEL_CROP_MS / 1000;
const uint16_t MODEL_MPU_COUNT = MPU_SAMPLE_RATE_HZ * MODEL_CROP_MS / 1000;

const float ACCEL_LSB_PER_G = 2048.0f; // MPU +/-16 g.
const float GYRO_LSB_PER_DPS = 32.8f;  // MPU +/-1000 dps.

static_assert(PIEZO_SAMPLE_RATE_HZ <= MAX_PIEZO_RATE_HZ, "Increase MAX_PIEZO_RATE_HZ");
static_assert(FFT_SIZE == 512, "The current FFT workspace is fixed at 512");

// ---------------------------------------------------------------------------
// Runtime types
// ---------------------------------------------------------------------------
enum SystemState {
  STATE_BOOT,
  STATE_SELF_TEST,
  STATE_CALIBRATING,
  STATE_IDLE,
  STATE_MEASUREMENT_ARMED,
  STATE_CAPTURING,
  STATE_PROCESSING,
  STATE_RESULT_HOLDING,
  STATE_FAULT
};

enum RuntimeEventType {
  EVENT_CALIBRATION_DONE,
  EVENT_HIT_TRIGGERED,
  EVENT_HIT_CAPTURED,
  EVENT_MEASUREMENT_TIMEOUT,
  EVENT_DMA_OVERFLOW,
  EVENT_BUFFER_UNAVAILABLE,
  EVENT_READY_QUEUE_FULL
};

struct RuntimeEvent {
  RuntimeEventType type;
  uint8_t hitNumber;
  uint32_t value;
};

struct MpuTimedSample {
  uint32_t timestampUs;
  int16_t ax;
  int16_t ay;
  int16_t az;
  int16_t gx;
  int16_t gy;
  int16_t gz;
};

struct EventBuffer {
  uint32_t measurementId;
  uint8_t hitNumber;
  uint32_t sampleRateHz;
  uint32_t triggerTimestampUs;
  uint32_t eventStartTimestampUs;
  uint32_t dmaOverflowStartCount;
  uint32_t dmaOverflowCount;
  uint32_t expectedPiezoSamples;
  uint32_t piezoCount;
  uint32_t triggerIndex;
  uint16_t piezo[MAX_PIEZO_SAMPLES];
  uint16_t mpuCount;
  MpuTimedSample mpu[MAX_MPU_SAMPLES];
};

struct HitAnalysis {
  bool complete;
  bool qualityValid;
  bool piezoSaturated;
  bool mpuSaturated;
  float piezoSnrDb;
  float mpuSnrDb;
  float dropRate;
  float f1Hz;
  float tauMs;
  float energyRatio;
  float signature[SIGNATURE_POINTS];
  bool modelReady;
  float modelFeatures[TINGGU_MODEL_FEATURE_COUNT];
  TingguModelOutput model;
};

struct MeasurementResult {
  uint32_t measurementId;
  bool valid;
  float features[4]; // f1_hz, tau_ms, e_ratio, consistency_c
  TingguModelOutput model;
  HitAnalysis hits[HITS_PER_MEASUREMENT];
};

// ---------------------------------------------------------------------------
// Fixed memory, queues and shared state
// ---------------------------------------------------------------------------
EventBuffer *eventBuffers[2] = {NULL, NULL};
HitAnalysis hitResults[HITS_PER_MEASUREMENT];
MeasurementResult latestResult;

uint16_t *preTriggerRing = NULL;
uint32_t preRingCount = 0;
uint32_t preRingWrite = 0;

MpuTimedSample mpuRing[MAX_MPU_SAMPLES];
uint16_t mpuRingCount = 0;
uint16_t mpuRingWrite = 0;

QueueHandle_t freeBufferQueue = NULL;
QueueHandle_t readyBufferQueue = NULL;
QueueHandle_t runtimeEventQueue = NULL;
SemaphoreHandle_t stateMutex = NULL;
SemaphoreHandle_t mpuMutex = NULL;
SemaphoreHandle_t serialMutex = NULL;

volatile SystemState systemState = STATE_BOOT;
volatile uint32_t activeMeasurementId = 0;
volatile uint8_t capturedHitCount = 0;
volatile uint8_t processedHitCount = 0;
volatile uint32_t measurementStartMs = 0;
volatile uint32_t refractoryUntilMs = 0;
volatile bool recalibrationRequested = false;

float piezoBaseline = 0.0f;
float piezoNoiseRms = 0.0f;
uint16_t piezoTriggerThreshold = MIN_TRIGGER_COUNTS;

adc_continuous_handle_t adcHandle = NULL;
volatile uint32_t adcPoolOverflowCount = 0;
adc_channel_t piezoAdcChannel;
adc_unit_t piezoAdcUnit = ADC_UNIT_1;
esp_err_t adcInitError = ESP_OK;
const char *adcInitStage = "not_started";
TaskHandle_t acquisitionTaskHandle = NULL;
TaskHandle_t mpuTaskHandle = NULL;
TaskHandle_t processingTaskHandle = NULL;

uint8_t activeMpuAddress = 0;
uint8_t activeMpuWhoAmI = 0;
bool mpuAvailable = false;

float fftReal[FFT_SIZE];
float fftImag[FFT_SIZE];

// Reused only by the Core 1 processing task; kept global to avoid a large task
// stack and to make peak RAM use deterministic.
float modelPiezo[MODEL_PIEZO_COUNT];
float modelMpuAxes[3][MODEL_MPU_COUNT];
float modelMpu[MODEL_MPU_COUNT];
float modelP1k[MODEL_MPU_COUNT];
float modelPowerPiezo[MODEL_PIEZO_COUNT / 2 + 1];
float modelPowerMpu[MODEL_MPU_COUNT / 2 + 1];
float modelLogPower[MODEL_PIEZO_COUNT / 2 + 1];

char commandBuffer[48];
uint8_t commandLength = 0;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
const char *stateName(SystemState state) {
  switch (state) {
    case STATE_BOOT: return "BOOT";
    case STATE_SELF_TEST: return "SELF_TEST";
    case STATE_CALIBRATING: return "CALIBRATING";
    case STATE_IDLE: return "IDLE";
    case STATE_MEASUREMENT_ARMED: return "MEASUREMENT_ARMED";
    case STATE_CAPTURING: return "CAPTURING";
    case STATE_PROCESSING: return "PROCESSING";
    case STATE_RESULT_HOLDING: return "RESULT_HOLDING";
    case STATE_FAULT: return "FAULT";
    default: return "UNKNOWN";
  }
}

void serialPrintf(const char *format, ...) {
  if (serialMutex != NULL) xSemaphoreTake(serialMutex, portMAX_DELAY);
  va_list args;
  va_start(args, format);
  Serial.vprintf(format, args);
  va_end(args);
  if (serialMutex != NULL) xSemaphoreGive(serialMutex);
}

void postRuntimeEvent(RuntimeEventType type, uint8_t hitNumber, uint32_t value) {
  if (runtimeEventQueue == NULL) return;
  RuntimeEvent event;
  event.type = type;
  event.hitNumber = hitNumber;
  event.value = value;
  xQueueSend(runtimeEventQueue, &event, 0);
}

float finiteSnr(float eventPower, float noisePower) {
  if (noisePower < 1e-12f) noisePower = 1e-12f;
  float signalPower = eventPower - noisePower;
  if (signalPower <= 0.0f) return -1000.0f;
  return 10.0f * log10f(signalPower / noisePower);
}

float medianOfThree(float a, float b, float c) {
  if (a > b) { float t = a; a = b; b = t; }
  if (b > c) { float t = b; b = c; c = t; }
  if (a > b) { float t = a; a = b; b = t; }
  return b;
}

// ---------------------------------------------------------------------------
// MPU-6050 raw I2C driver
// ---------------------------------------------------------------------------
bool mpuReadBytes(uint8_t address, uint8_t reg, uint8_t *data, size_t length) {
  Wire.beginTransmission(address);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  size_t received = Wire.requestFrom(address, (uint8_t)length, (uint8_t)true);
  if (received != length) {
    while (Wire.available()) Wire.read();
    return false;
  }
  for (size_t i = 0; i < length; ++i) {
    if (!Wire.available()) return false;
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

bool probeMpu(uint8_t address) {
  uint8_t value = 0;
  if (!mpuReadBytes(address, 0x75, &value, 1)) return false;
  if (value != 0x68 && value != 0x69 && value != 0x70) return false;
  activeMpuWhoAmI = value;
  return true;
}

const char *mpuModelName() {
  return activeMpuWhoAmI == 0x70 ? "MPU6500" : "MPU6050";
}

bool configureMpu() {
  activeMpuAddress = 0;
  activeMpuWhoAmI = 0;
  if (MPU_ADDRESS != 0 && probeMpu(MPU_ADDRESS)) activeMpuAddress = MPU_ADDRESS;
  else if (MPU_ADDRESS == 0 && probeMpu(0x68)) activeMpuAddress = 0x68;
  else if (MPU_ADDRESS == 0 && probeMpu(0x69)) activeMpuAddress = 0x69;
  else return false;

  if (!mpuWriteByte(activeMpuAddress, 0x6B, 0x01)) return false;
  delay(10);
  if (!mpuWriteByte(activeMpuAddress, 0x19, 0x00)) return false;
  if (!mpuWriteByte(activeMpuAddress, 0x1A, 0x01)) return false;
  if (!mpuWriteByte(activeMpuAddress, 0x1B, 0x10)) return false;
  if (!mpuWriteByte(activeMpuAddress, 0x1C, 0x18)) return false;
  return true;
}

int16_t int16FromBytes(uint8_t highByte, uint8_t lowByte) {
  return (int16_t)(((uint16_t)highByte << 8) | lowByte);
}

bool readMpuSample(MpuTimedSample &sample) {
  uint8_t data[14];
  if (!mpuAvailable || !mpuReadBytes(activeMpuAddress, 0x3B, data, sizeof(data))) return false;
  sample.timestampUs = micros();
  sample.ax = int16FromBytes(data[0], data[1]);
  sample.ay = int16FromBytes(data[2], data[3]);
  sample.az = int16FromBytes(data[4], data[5]);
  sample.gx = int16FromBytes(data[8], data[9]);
  sample.gy = int16FromBytes(data[10], data[11]);
  sample.gz = int16FromBytes(data[12], data[13]);
  return true;
}

void mpuTask(void *parameter) {
  (void)parameter;
  TickType_t lastWake = xTaskGetTickCount();
  const TickType_t period = max((TickType_t)1, pdMS_TO_TICKS(1000UL / MPU_SAMPLE_RATE_HZ));
  for (;;) {
    if (mpuAvailable) {
      MpuTimedSample sample;
      if (readMpuSample(sample)) {
        xSemaphoreTake(mpuMutex, portMAX_DELAY);
        mpuRing[mpuRingWrite] = sample;
        mpuRingWrite = (mpuRingWrite + 1) % MAX_MPU_SAMPLES;
        if (mpuRingCount < MAX_MPU_SAMPLES) ++mpuRingCount;
        xSemaphoreGive(mpuMutex);
      }
    }
    vTaskDelayUntil(&lastWake, period);
  }
}

void copyMpuWindow(EventBuffer &event) {
  uint32_t windowStart = event.triggerTimestampUs - PRE_TRIGGER_MS * 1000UL;
  uint32_t windowEnd = event.triggerTimestampUs + POST_TRIGGER_MS * 1000UL;
  event.mpuCount = 0;
  xSemaphoreTake(mpuMutex, portMAX_DELAY);
  uint16_t oldest = (mpuRingWrite + MAX_MPU_SAMPLES - mpuRingCount) % MAX_MPU_SAMPLES;
  for (uint16_t i = 0; i < mpuRingCount && event.mpuCount < MAX_MPU_SAMPLES; ++i) {
    const MpuTimedSample &sample = mpuRing[(oldest + i) % MAX_MPU_SAMPLES];
    if ((int32_t)(sample.timestampUs - windowStart) >= 0 &&
        (int32_t)(windowEnd - sample.timestampUs) >= 0) {
      event.mpu[event.mpuCount++] = sample;
    }
  }
  xSemaphoreGive(mpuMutex);
}

// ---------------------------------------------------------------------------
// ADC continuous DMA
// ---------------------------------------------------------------------------
bool IRAM_ATTR onAdcPoolOverflow(adc_continuous_handle_t handle,
                                 const adc_continuous_evt_data_t *eventData,
                                 void *userData) {
  (void)handle;
  (void)eventData;
  (void)userData;
  ++adcPoolOverflowCount;
  return false;
}

bool beginAdcDma() {
  adcInitStage = "io_to_channel";
  adcInitError = adc_continuous_io_to_channel(PIEZO_ADC_PIN, &piezoAdcUnit, &piezoAdcChannel);
  if (adcInitError != ESP_OK) return false;

  const uint32_t conversionsPerFrame = 32;
  const uint32_t frameBytes = conversionsPerFrame * SOC_ADC_DIGI_RESULT_BYTES;
  adc_continuous_handle_cfg_t handleConfig = {};
  handleConfig.max_store_buf_size = frameBytes * 8;
  handleConfig.conv_frame_size = frameBytes;
  adcInitStage = "new_handle";
  adcInitError = adc_continuous_new_handle(&handleConfig, &adcHandle);
  if (adcInitError != ESP_OK) return false;

  adc_digi_pattern_config_t pattern = {};
  pattern.atten = ADC_ATTEN_DB_12;
  pattern.channel = piezoAdcChannel;
  pattern.unit = piezoAdcUnit;
  pattern.bit_width = ADC_BITWIDTH_12;

  adc_continuous_config_t config = {};
  config.sample_freq_hz = PIEZO_SAMPLE_RATE_HZ;
  config.conv_mode = piezoAdcUnit == ADC_UNIT_1 ? ADC_CONV_SINGLE_UNIT_1 : ADC_CONV_SINGLE_UNIT_2;
#if CONFIG_IDF_TARGET_ESP32
  config.format = ADC_DIGI_OUTPUT_FORMAT_TYPE1;
#else
  config.format = ADC_DIGI_OUTPUT_FORMAT_TYPE2;
#endif
  config.pattern_num = 1;
  config.adc_pattern = &pattern;
  adcInitStage = "configure";
  adcInitError = adc_continuous_config(adcHandle, &config);
  if (adcInitError != ESP_OK) return false;
  adc_continuous_evt_cbs_t callbacks = {};
  callbacks.on_pool_ovf = onAdcPoolOverflow;
  adcInitStage = "callbacks";
  adcInitError = adc_continuous_register_event_callbacks(adcHandle, &callbacks, NULL);
  if (adcInitError != ESP_OK) return false;
  adcInitStage = "start";
  adcInitError = adc_continuous_start(adcHandle);
  if (adcInitError != ESP_OK) return false;
  adcInitStage = "ready";
  return true;
}

uint16_t adcRawValue(const uint8_t *data) {
  const adc_digi_output_data_t *output = (const adc_digi_output_data_t *)data;
#if CONFIG_IDF_TARGET_ESP32
  return output->type1.data;
#else
  return output->type2.data;
#endif
}

void resetCalibration() {
  piezoBaseline = 0.0f;
  piezoNoiseRms = 0.0f;
  piezoTriggerThreshold = MIN_TRIGGER_COUNTS;
  preRingCount = 0;
  preRingWrite = 0;
  recalibrationRequested = false;
  systemState = STATE_CALIBRATING;
}

void armMeasurement() {
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  if (systemState == STATE_IDLE || systemState == STATE_RESULT_HOLDING) {
    ++activeMeasurementId;
    capturedHitCount = 0;
    processedHitCount = 0;
    measurementStartMs = millis();
    refractoryUntilMs = 0;
    memset(hitResults, 0, sizeof(hitResults));
    memset(&latestResult, 0, sizeof(latestResult));
    latestResult.measurementId = activeMeasurementId;
    systemState = STATE_MEASUREMENT_ARMED;
  }
  xSemaphoreGive(stateMutex);
}

void abortMeasurement() {
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  ++activeMeasurementId; // Invalidates any event that was already queued.
  capturedHitCount = 0;
  processedHitCount = 0;
  systemState = STATE_IDLE;
  xSemaphoreGive(stateMutex);
}

int beginTriggeredEvent(uint16_t triggerRaw, uint32_t triggerTimestampUs) {
  uint8_t bufferIndex;
  if (xQueueReceive(freeBufferQueue, &bufferIndex, 0) != pdTRUE) {
    systemState = STATE_FAULT;
    postRuntimeEvent(EVENT_BUFFER_UNAVAILABLE, capturedHitCount + 1, 0);
    return -1;
  }

  EventBuffer &event = *eventBuffers[bufferIndex];
  event.measurementId = activeMeasurementId;
  event.hitNumber = capturedHitCount + 1;
  event.sampleRateHz = PIEZO_SAMPLE_RATE_HZ;
  event.triggerTimestampUs = triggerTimestampUs;
  event.eventStartTimestampUs = triggerTimestampUs - PRE_TRIGGER_MS * 1000UL;
  event.dmaOverflowStartCount = adcPoolOverflowCount;
  event.dmaOverflowCount = 0;
  event.expectedPiezoSamples = PIEZO_SAMPLE_RATE_HZ * (PRE_TRIGGER_MS + POST_TRIGGER_MS) / 1000UL;
  event.piezoCount = 0;

  uint32_t requiredPre = PIEZO_SAMPLE_RATE_HZ * PRE_TRIGGER_MS / 1000UL;
  uint32_t availablePre = min(preRingCount, requiredPre);
  uint32_t oldest = (preRingWrite + PRE_RING_CAPACITY - availablePre) % PRE_RING_CAPACITY;
  for (uint32_t i = 0; i < availablePre; ++i) {
    event.piezo[event.piezoCount++] = preTriggerRing[(oldest + i) % PRE_RING_CAPACITY];
  }
  while (event.piezoCount < requiredPre) event.piezo[event.piezoCount++] = (uint16_t)piezoBaseline;
  event.triggerIndex = event.piezoCount;
  event.piezo[event.piezoCount++] = triggerRaw;

  systemState = STATE_CAPTURING;
  postRuntimeEvent(EVENT_HIT_TRIGGERED, event.hitNumber, bufferIndex);
  return bufferIndex;
}

void acquisitionTask(void *parameter) {
  (void)parameter;
  const uint32_t frameBytes = 32 * SOC_ADC_DIGI_RESULT_BYTES;
  uint8_t dmaData[32 * SOC_ADC_DIGI_RESULT_BYTES];
  uint32_t bytesRead = 0;
  uint32_t calibrationCount = 0;
  double calibrationMean = 0.0;
  double calibrationM2 = 0.0;
  int activeBuffer = -1;

  for (;;) {
    if (activeBuffer >= 0 && systemState != STATE_CAPTURING) {
      uint8_t abandoned = (uint8_t)activeBuffer;
      xQueueSend(freeBufferQueue, &abandoned, portMAX_DELAY);
      activeBuffer = -1;
    }

    esp_err_t readResult = adc_continuous_read(adcHandle, dmaData, frameBytes, &bytesRead, pdMS_TO_TICKS(25));
    if (readResult == ESP_ERR_TIMEOUT) continue;
    if (readResult != ESP_OK) {
      if (activeBuffer >= 0) ++eventBuffers[activeBuffer]->dmaOverflowCount;
      postRuntimeEvent(EVENT_DMA_OVERFLOW, capturedHitCount + 1, (uint32_t)readResult);
      continue;
    }

    for (uint32_t offset = 0; offset < bytesRead; offset += SOC_ADC_DIGI_RESULT_BYTES) {
      uint16_t raw = adcRawValue(&dmaData[offset]);
      uint32_t sampleTimestampUs = micros() -
        ((bytesRead - offset) / SOC_ADC_DIGI_RESULT_BYTES) * (1000000UL / PIEZO_SAMPLE_RATE_HZ);

      if (recalibrationRequested) {
        calibrationCount = 0;
        calibrationMean = 0.0;
        calibrationM2 = 0.0;
        resetCalibration();
      }

      if (systemState == STATE_CALIBRATING) {
        ++calibrationCount;
        double delta = raw - calibrationMean;
        calibrationMean += delta / calibrationCount;
        calibrationM2 += delta * (raw - calibrationMean);
        if (calibrationCount >= PIEZO_SAMPLE_RATE_HZ) {
          piezoBaseline = (float)calibrationMean;
          piezoNoiseRms = calibrationCount > 1 ? sqrtf((float)(calibrationM2 / (calibrationCount - 1))) : 0.0f;
          piezoTriggerThreshold = (uint16_t)ceilf(max((float)MIN_TRIGGER_COUNTS,
            piezoNoiseRms * TRIGGER_NOISE_MULTIPLIER));
          systemState = STATE_IDLE;
          postRuntimeEvent(EVENT_CALIBRATION_DONE, 0, piezoTriggerThreshold);
        }
      }

      if (systemState == STATE_MEASUREMENT_ARMED) {
        if (millis() - measurementStartMs > MEASUREMENT_TIMEOUT_MS) {
          systemState = STATE_RESULT_HOLDING;
          postRuntimeEvent(EVENT_MEASUREMENT_TIMEOUT, capturedHitCount + 1, 0);
        } else if ((int32_t)(millis() - refractoryUntilMs) >= 0) {
          int amplitude = abs((int)raw - (int)lroundf(piezoBaseline));
          if (amplitude >= piezoTriggerThreshold) {
            activeBuffer = beginTriggeredEvent(raw, sampleTimestampUs);
          }
        }
      } else if (systemState == STATE_CAPTURING && activeBuffer >= 0) {
        EventBuffer &event = *eventBuffers[activeBuffer];
        if (event.piezoCount < event.expectedPiezoSamples && event.piezoCount < MAX_PIEZO_SAMPLES) {
          event.piezo[event.piezoCount++] = raw;
        }
        if (event.piezoCount >= event.expectedPiezoSamples) {
          event.dmaOverflowCount += adcPoolOverflowCount - event.dmaOverflowStartCount;
          copyMpuWindow(event);
          uint8_t completedIndex = (uint8_t)activeBuffer;
          ++capturedHitCount;
          refractoryUntilMs = millis() + REFRACTORY_MS;
          if (xQueueSend(readyBufferQueue, &completedIndex, 0) != pdTRUE) {
            xQueueSend(freeBufferQueue, &completedIndex, 0);
            systemState = STATE_FAULT;
            postRuntimeEvent(EVENT_READY_QUEUE_FULL, event.hitNumber, 0);
          } else {
            postRuntimeEvent(EVENT_HIT_CAPTURED, event.hitNumber, event.piezoCount);
            systemState = capturedHitCount >= HITS_PER_MEASUREMENT ? STATE_PROCESSING : STATE_MEASUREMENT_ARMED;
          }
          activeBuffer = -1;
        }
      }

      uint32_t preCapacity = PRE_RING_CAPACITY;
      preTriggerRing[preRingWrite] = raw;
      preRingWrite = (preRingWrite + 1) % preCapacity;
      if (preRingCount < preCapacity) ++preRingCount;
    }
  }
}

// ---------------------------------------------------------------------------
// Signal processing on Core 1
// ---------------------------------------------------------------------------
void fft512(float *realValues, float *imagValues) {
  uint16_t j = 0;
  for (uint16_t i = 1; i < FFT_SIZE; ++i) {
    uint16_t bit = FFT_SIZE >> 1;
    while (j & bit) { j ^= bit; bit >>= 1; }
    j ^= bit;
    if (i < j) {
      float temp = realValues[i]; realValues[i] = realValues[j]; realValues[j] = temp;
      temp = imagValues[i]; imagValues[i] = imagValues[j]; imagValues[j] = temp;
    }
  }

  for (uint16_t length = 2; length <= FFT_SIZE; length <<= 1) {
    float angle = -2.0f * PI / length;
    float wLengthReal = cosf(angle);
    float wLengthImag = sinf(angle);
    for (uint16_t start = 0; start < FFT_SIZE; start += length) {
      float wReal = 1.0f;
      float wImag = 0.0f;
      for (uint16_t k = 0; k < length / 2; ++k) {
        uint16_t even = start + k;
        uint16_t odd = even + length / 2;
        float oddReal = realValues[odd] * wReal - imagValues[odd] * wImag;
        float oddImag = realValues[odd] * wImag + imagValues[odd] * wReal;
        realValues[odd] = realValues[even] - oddReal;
        imagValues[odd] = imagValues[even] - oddImag;
        realValues[even] += oddReal;
        imagValues[even] += oddImag;
        float nextReal = wReal * wLengthReal - wImag * wLengthImag;
        wImag = wReal * wLengthImag + wImag * wLengthReal;
        wReal = nextReal;
      }
    }
  }
}

// ---------------------------------------------------------------------------
// 240 ms spectral model feature extractor (matches advanced_recrop_search.py)
// ---------------------------------------------------------------------------
float medianInPlace(float *values, uint16_t count) {
  if (count == 0) return 0.0f;
  for (uint16_t i = 1; i < count; ++i) {
    float value = values[i];
    int j = (int)i - 1;
    while (j >= 0 && values[j] > value) {
      values[j + 1] = values[j];
      --j;
    }
    values[j + 1] = value;
  }
  return count & 1 ? values[count / 2]
                   : 0.5f * (values[count / 2 - 1] + values[count / 2]);
}

void modelPowerSpectrum(const float *values, uint16_t count, bool periodicHann,
                        float *power) {
  float mean = 0.0f;
  for (uint16_t i = 0; i < count; ++i) mean += values[i];
  mean /= count;
  for (uint16_t bin = 0; bin <= count / 2; ++bin) {
    float step = -2.0f * PI * bin / count;
    float stepReal = cosf(step), stepImag = sinf(step);
    float oscillatorReal = 1.0f, oscillatorImag = 0.0f;
    float sumReal = 0.0f, sumImag = 0.0f;
    for (uint16_t i = 0; i < count; ++i) {
      float divisor = periodicHann ? (float)count : (float)(count - 1);
      float window = 0.5f - 0.5f * cosf(2.0f * PI * i / divisor);
      float sample = (values[i] - mean) * window;
      sumReal += sample * oscillatorReal;
      sumImag += sample * oscillatorImag;
      float nextReal = oscillatorReal * stepReal - oscillatorImag * stepImag;
      oscillatorImag = oscillatorReal * stepImag + oscillatorImag * stepReal;
      oscillatorReal = nextReal;
    }
    power[bin] = sumReal * sumReal + sumImag * sumImag;
  }
}

float modelBandShare(const float *power, uint16_t count, float sampleRate,
                     float validMaximumHz, float lowHz, float highHz) {
  double total = 1e-12, selected = 0.0;
  for (uint16_t bin = 0; bin <= count / 2; ++bin) {
    float frequency = bin * sampleRate / count;
    if (frequency >= 5.0f && frequency <= validMaximumHz) total += power[bin];
    if (frequency >= lowHz && frequency < highHz) selected += power[bin];
  }
  return (float)(selected / total);
}

float modelCepstralCoefficient(const float *power, uint16_t count,
                               float sampleRate, float validMaximumHz,
                               uint8_t coefficient) {
  double total = 1e-12;
  uint16_t validCount = 0;
  for (uint16_t bin = 0; bin <= count / 2; ++bin) {
    float frequency = bin * sampleRate / count;
    if (frequency >= 5.0f && frequency <= validMaximumHz) {
      total += power[bin];
      ++validCount;
    }
  }
  uint16_t index = 0;
  for (uint16_t bin = 0; bin <= count / 2; ++bin) {
    float frequency = bin * sampleRate / count;
    if (frequency >= 5.0f && frequency <= validMaximumHz) {
      modelLogPower[index++] = log1pf((float)(power[bin] / total) * 10000.0f);
    }
  }
  if (validCount == 0) return 0.0f;
  double sum = 0.0;
  for (uint16_t i = 0; i < validCount; ++i) {
    sum += modelLogPower[i] * cos(PI * coefficient * (2.0 * i + 1.0) / (2.0 * validCount));
  }
  return (float)(sum * (coefficient == 0 ? sqrt(1.0 / validCount)
                                         : sqrt(2.0 / validCount)));
}

void modelStftShares(const float *values, uint16_t count, float sampleRate,
                     uint16_t segmentLength, const float bands[][2],
                     uint8_t bandCount, float shares[4][7]) {
  memset(shares, 0, sizeof(float) * 4 * 7);
  double denominators[4] = {1e-12, 1e-12, 1e-12, 1e-12};
  double numerators[4][7] = {};
  float mean = 0.0f;
  for (uint16_t i = 0; i < count; ++i) mean += values[i];
  mean /= count;
  uint16_t hop = segmentLength / 2;
  uint16_t frames = (count <= segmentLength) ? 1 :
    (uint16_t)((count - segmentLength + hop - 1) / hop + 1);
  for (uint16_t frame = 0; frame < frames; ++frame) {
    float centerSeconds = (frame * hop + segmentLength / 2) / sampleRate;
    uint8_t group = centerSeconds < 0.04f ? 0 :
                    centerSeconds < 0.08f ? 1 :
                    centerSeconds < 0.14f ? 2 : 3;
    for (uint16_t bin = 0; bin <= segmentLength / 2; ++bin) {
      float step = -2.0f * PI * bin / segmentLength;
      float stepReal = cosf(step), stepImag = sinf(step);
      float oscillatorReal = 1.0f, oscillatorImag = 0.0f;
      float sumReal = 0.0f, sumImag = 0.0f;
      for (uint16_t i = 0; i < segmentLength; ++i) {
        uint16_t source = frame * hop + i;
        float sample = source < count ? values[source] - mean : 0.0f;
        float window = 0.5f - 0.5f * cosf(2.0f * PI * i / segmentLength);
        sumReal += sample * window * oscillatorReal;
        sumImag += sample * window * oscillatorImag;
        float nextReal = oscillatorReal * stepReal - oscillatorImag * stepImag;
        oscillatorImag = oscillatorReal * stepImag + oscillatorImag * stepReal;
        oscillatorReal = nextReal;
      }
      double power = sumReal * sumReal + sumImag * sumImag;
      float frequency = bin * sampleRate / segmentLength;
      denominators[group] += power;
      for (uint8_t band = 0; band < bandCount; ++band) {
        if (frequency >= bands[band][0] && frequency < bands[band][1]) {
          numerators[group][band] += power;
        }
      }
    }
  }
  for (uint8_t group = 0; group < 4; ++group) {
    for (uint8_t band = 0; band < bandCount; ++band) {
      shares[group][band] = (float)(numerators[group][band] / denominators[group]);
    }
  }
}

float modelMpuAxisValue(const MpuTimedSample &sample, uint8_t axis) {
  if (axis == 0) return sample.ax / ACCEL_LSB_PER_G;
  if (axis == 1) return sample.ay / ACCEL_LSB_PER_G;
  return sample.az / ACCEL_LSB_PER_G;
}

bool resampleModelMpu(const EventBuffer &event) {
  if (event.mpuCount < 4) return false;
  float preValues[3][128];
  uint16_t preCount = 0;
  for (uint16_t i = 0; i < event.mpuCount && preCount < 128; ++i) {
    int32_t relativeUs = (int32_t)(event.mpu[i].timestampUs - event.triggerTimestampUs);
    if (relativeUs >= -90000 && relativeUs <= -10000) {
      for (uint8_t axis = 0; axis < 3; ++axis) preValues[axis][preCount] = modelMpuAxisValue(event.mpu[i], axis);
      ++preCount;
    }
  }
  if (preCount < 2) return false;
  float center[3];
  for (uint8_t axis = 0; axis < 3; ++axis) center[axis] = medianInPlace(preValues[axis], preCount);

  uint16_t cursor = 0;
  for (uint16_t targetIndex = 0; targetIndex < MODEL_MPU_COUNT; ++targetIndex) {
    int32_t targetUs = (int32_t)targetIndex * 1000;
    while (cursor + 1 < event.mpuCount &&
           (int32_t)(event.mpu[cursor + 1].timestampUs - event.triggerTimestampUs) < targetUs) {
      ++cursor;
    }
    uint16_t left = cursor;
    uint16_t right = min((uint16_t)(cursor + 1), (uint16_t)(event.mpuCount - 1));
    int32_t leftUs = (int32_t)(event.mpu[left].timestampUs - event.triggerTimestampUs);
    int32_t rightUs = (int32_t)(event.mpu[right].timestampUs - event.triggerTimestampUs);
    float fraction = rightUs != leftUs ? constrain((targetUs - leftUs) / (float)(rightUs - leftUs), 0.0f, 1.0f) : 0.0f;
    for (uint8_t axis = 0; axis < 3; ++axis) {
      float leftValue = modelMpuAxisValue(event.mpu[left], axis);
      float rightValue = modelMpuAxisValue(event.mpu[right], axis);
      modelMpuAxes[axis][targetIndex] = leftValue + fraction * (rightValue - leftValue) - center[axis];
    }
  }

  float means[3] = {};
  for (uint8_t axis = 0; axis < 3; ++axis) {
    for (uint16_t i = 0; i < MODEL_MPU_COUNT; ++i) means[axis] += modelMpuAxes[axis][i];
    means[axis] /= MODEL_MPU_COUNT;
  }
  float covariance[3][3] = {};
  for (uint8_t row = 0; row < 3; ++row) {
    for (uint8_t column = 0; column < 3; ++column) {
      double sum = 0.0;
      for (uint16_t i = 0; i < MODEL_MPU_COUNT; ++i) {
        sum += (modelMpuAxes[row][i] - means[row]) * (modelMpuAxes[column][i] - means[column]);
      }
      covariance[row][column] = (float)(sum / (MODEL_MPU_COUNT - 1));
    }
  }
  uint8_t largestVarianceAxis = 0;
  if (covariance[1][1] > covariance[largestVarianceAxis][largestVarianceAxis]) largestVarianceAxis = 1;
  if (covariance[2][2] > covariance[largestVarianceAxis][largestVarianceAxis]) largestVarianceAxis = 2;
  float eigenvector[3] = {0.0f, 0.0f, 0.0f};
  eigenvector[largestVarianceAxis] = 1.0f;
  for (uint8_t iteration = 0; iteration < 16; ++iteration) {
    float next[3] = {};
    for (uint8_t row = 0; row < 3; ++row) {
      for (uint8_t column = 0; column < 3; ++column) next[row] += covariance[row][column] * eigenvector[column];
    }
    float norm = sqrtf(next[0] * next[0] + next[1] * next[1] + next[2] * next[2]);
    if (norm < 1e-12f) return false;
    for (uint8_t axis = 0; axis < 3; ++axis) eigenvector[axis] = next[axis] / norm;
  }
  for (uint16_t i = 0; i < MODEL_MPU_COUNT; ++i) {
    modelMpu[i] = modelMpuAxes[0][i] * eigenvector[0] +
                  modelMpuAxes[1][i] * eigenvector[1] +
                  modelMpuAxes[2][i] * eigenvector[2];
  }
  return true;
}

float modelLogRatioBand(const float *piezoPower, const float *mpuPower,
                        float lowHz, float highHz) {
  uint16_t count = 0;
  for (uint16_t bin = 0; bin <= MODEL_MPU_COUNT / 2; ++bin) {
    float frequency = bin * MPU_SAMPLE_RATE_HZ / (float)MODEL_MPU_COUNT;
    if (frequency >= lowHz && frequency < highHz) {
      modelLogPower[count++] = logf((sqrtf(mpuPower[bin]) + 1e-9f) /
                                    (sqrtf(piezoPower[bin]) + 1e-9f));
    }
  }
  return medianInPlace(modelLogPower, count);
}

float modelCoherence30To60(const float *piezo, const float *mpu) {
  const uint16_t segmentLength = 96, hop = 48, segments = 4;
  float coherenceSum = 0.0f;
  uint8_t binCount = 0;
  for (uint16_t bin = 0; bin <= segmentLength / 2; ++bin) {
    float frequency = bin * MPU_SAMPLE_RATE_HZ / (float)segmentLength;
    if (frequency < 30.0f || frequency >= 60.0f) continue;
    double crossReal = 0.0, crossImag = 0.0, piezoAuto = 0.0, mpuAuto = 0.0;
    for (uint16_t segment = 0; segment < segments; ++segment) {
      uint16_t start = segment * hop;
      float piezoMean = 0.0f, mpuMean = 0.0f;
      for (uint16_t i = 0; i < segmentLength; ++i) {
        piezoMean += piezo[start + i];
        mpuMean += mpu[start + i];
      }
      piezoMean /= segmentLength;
      mpuMean /= segmentLength;
      float step = -2.0f * PI * bin / segmentLength;
      float stepReal = cosf(step), stepImag = sinf(step);
      float oscillatorReal = 1.0f, oscillatorImag = 0.0f;
      float pr = 0.0f, pi = 0.0f, mr = 0.0f, mi = 0.0f;
      for (uint16_t i = 0; i < segmentLength; ++i) {
        float window = 0.5f - 0.5f * cosf(2.0f * PI * i / segmentLength);
        float pv = (piezo[start + i] - piezoMean) * window;
        float mv = (mpu[start + i] - mpuMean) * window;
        pr += pv * oscillatorReal; pi += pv * oscillatorImag;
        mr += mv * oscillatorReal; mi += mv * oscillatorImag;
        float nextReal = oscillatorReal * stepReal - oscillatorImag * stepImag;
        oscillatorImag = oscillatorReal * stepImag + oscillatorImag * stepReal;
        oscillatorReal = nextReal;
      }
      crossReal += pr * mr + pi * mi;
      crossImag += pi * mr - pr * mi;
      piezoAuto += pr * pr + pi * pi;
      mpuAuto += mr * mr + mi * mi;
    }
    double denominator = piezoAuto * mpuAuto;
    coherenceSum += denominator > 1e-24 ? (float)((crossReal * crossReal + crossImag * crossImag) / denominator) : 0.0f;
    ++binCount;
  }
  return binCount > 0 ? coherenceSum / binCount : 0.0f;
}

bool extractSpectralModelFeatures(const EventBuffer &event,
                                  float features[TINGGU_MODEL_FEATURE_COUNT]) {
  if (event.piezoCount < event.triggerIndex + MODEL_PIEZO_COUNT || !resampleModelMpu(event)) return false;
  memset(features, 0, sizeof(float) * TINGGU_MODEL_FEATURE_COUNT);

  uint16_t baselineCount = 0;
  int32_t baselineStart = (int32_t)event.triggerIndex - (int32_t)(event.sampleRateHz * 90 / 1000);
  int32_t baselineEnd = (int32_t)event.triggerIndex - (int32_t)(event.sampleRateHz * 10 / 1000);
  for (int32_t i = max((int32_t)0, baselineStart); i <= baselineEnd && i < (int32_t)event.piezoCount; ++i) {
    modelLogPower[baselineCount++] = event.piezo[i];
  }
  float baseline = medianInPlace(modelLogPower, baselineCount);
  for (uint16_t i = 0; i < MODEL_PIEZO_COUNT; ++i) modelPiezo[i] = event.piezo[event.triggerIndex + i] - baseline;

  modelPowerSpectrum(modelMpu, MODEL_MPU_COUNT, false, modelPowerMpu);
  features[0] = modelBandShare(modelPowerMpu, MODEL_MPU_COUNT, MPU_SAMPLE_RATE_HZ, 400.0f, 150.0f, 200.0f);
  features[1] = modelBandShare(modelPowerMpu, MODEL_MPU_COUNT, MPU_SAMPLE_RATE_HZ, 400.0f, 200.0f, 250.0f);
  features[2] = modelBandShare(modelPowerMpu, MODEL_MPU_COUNT, MPU_SAMPLE_RATE_HZ, 400.0f, 250.0f, 400.0f);
  features[3] = modelBandShare(modelPowerMpu, MODEL_MPU_COUNT, MPU_SAMPLE_RATE_HZ, 400.0f, 60.0f, 100.0f);
  const uint8_t mpuCepIndices[6] = {3, 6, 8, 9, 15, 16};
  for (uint8_t i = 0; i < 6; ++i) features[4 + i] = modelCepstralCoefficient(
    modelPowerMpu, MODEL_MPU_COUNT, MPU_SAMPLE_RATE_HZ, 400.0f, mpuCepIndices[i]);

  const float mpuBands[7][2] = {{5,30},{30,60},{60,100},{100,150},{150,200},{200,250},{250,400}};
  float mpuStft[4][7];
  modelStftShares(modelMpu, MODEL_MPU_COUNT, MPU_SAMPLE_RATE_HZ, 64, mpuBands, 7, mpuStft);
  features[10] = mpuStft[0][3]; features[11] = mpuStft[0][4];
  features[12] = mpuStft[0][5]; features[13] = mpuStft[0][0]; features[14] = mpuStft[0][2];
  features[15] = mpuStft[1][4]; features[16] = mpuStft[1][6]; features[17] = mpuStft[1][2];
  features[18] = mpuStft[2][4];

  modelPowerSpectrum(modelPiezo, MODEL_PIEZO_COUNT, false, modelPowerPiezo);
  features[19] = modelBandShare(modelPowerPiezo, MODEL_PIEZO_COUNT, PIEZO_SAMPLE_RATE_HZ, 800.0f, 100.0f, 150.0f);
  features[20] = modelBandShare(modelPowerPiezo, MODEL_PIEZO_COUNT, PIEZO_SAMPLE_RATE_HZ, 800.0f, 150.0f, 200.0f);
  features[21] = modelBandShare(modelPowerPiezo, MODEL_PIEZO_COUNT, PIEZO_SAMPLE_RATE_HZ, 800.0f, 300.0f, 500.0f);
  const uint8_t piezoCepIndices[6] = {2, 3, 5, 7, 9, 19};
  for (uint8_t i = 0; i < 6; ++i) features[22 + i] = modelCepstralCoefficient(
    modelPowerPiezo, MODEL_PIEZO_COUNT, PIEZO_SAMPLE_RATE_HZ, 800.0f, piezoCepIndices[i]);

  const float piezoBands[7][2] = {{5,30},{30,60},{60,100},{100,150},{150,250},{250,500},{500,800}};
  float piezoStft[4][7];
  modelStftShares(modelPiezo, MODEL_PIEZO_COUNT, PIEZO_SAMPLE_RATE_HZ, 128, piezoBands, 7, piezoStft);
  features[28] = piezoStft[1][3]; features[29] = piezoStft[1][5];
  features[30] = piezoStft[2][3]; features[31] = piezoStft[2][0];
  features[32] = piezoStft[3][3]; features[33] = piezoStft[3][4];
  features[34] = piezoStft[3][0]; features[35] = piezoStft[3][2];

  for (uint16_t i = 0; i < MODEL_MPU_COUNT; ++i) modelP1k[i] = modelPiezo[i * 2];
  modelPowerSpectrum(modelP1k, MODEL_MPU_COUNT, false, modelPowerPiezo);
  modelPowerSpectrum(modelMpu, MODEL_MPU_COUNT, false, modelPowerMpu);
  features[36] = modelCoherence30To60(modelP1k, modelMpu);
  features[37] = modelLogRatioBand(modelPowerPiezo, modelPowerMpu, 100.0f, 150.0f);
  features[38] = modelLogRatioBand(modelPowerPiezo, modelPowerMpu, 150.0f, 200.0f);
  features[39] = modelLogRatioBand(modelPowerPiezo, modelPowerMpu, 60.0f, 100.0f);
  for (uint8_t i = 0; i < TINGGU_MODEL_FEATURE_COUNT; ++i) if (!isfinite(features[i])) return false;
  return true;
}

float estimateTauMs(const EventBuffer &event, float baseline, float noiseRms) {
  const uint32_t blockSamples = max(1UL, event.sampleRateHz / 200UL); // 5 ms RMS blocks.
  double sumX = 0.0, sumY = 0.0, sumXX = 0.0, sumXY = 0.0;
  uint32_t pointCount = 0;
  uint32_t start = event.triggerIndex + event.sampleRateHz * 20 / 1000;
  uint32_t end = min(event.piezoCount, event.triggerIndex + event.sampleRateHz * 300 / 1000);
  for (uint32_t index = start; index + blockSamples <= end; index += blockSamples) {
    double power = 0.0;
    for (uint32_t k = 0; k < blockSamples; ++k) {
      float value = event.piezo[index + k] - baseline;
      power += value * value;
    }
    float envelope = sqrtf((float)(power / blockSamples));
    if (envelope <= noiseRms * 3.0f) continue;
    double x = (index - event.triggerIndex) / (double)event.sampleRateHz;
    double y = log(envelope);
    sumX += x; sumY += y; sumXX += x * x; sumXY += x * y;
    ++pointCount;
  }
  double denominator = pointCount * sumXX - sumX * sumX;
  if (pointCount < 4 || fabs(denominator) < 1e-12) return 0.0f;
  double slope = (pointCount * sumXY - sumX * sumY) / denominator;
  if (slope >= 0.0) return 0.0f;
  return (float)(-1000.0 / slope);
}

void buildSignature(const EventBuffer &event, float baseline, float *signature) {
  uint32_t windowSamples = (uint32_t)(event.sampleRateHz * SIGNATURE_WINDOW_MS / 1000.0f);
  float mean = 0.0f;
  for (uint16_t i = 0; i < SIGNATURE_POINTS; ++i) {
    uint32_t source = event.triggerIndex + (uint32_t)((uint64_t)i * windowSamples / SIGNATURE_POINTS);
    signature[i] = source < event.piezoCount ? event.piezo[source] - baseline : 0.0f;
    mean += signature[i];
  }
  mean /= SIGNATURE_POINTS;
  float power = 0.0f;
  for (uint16_t i = 0; i < SIGNATURE_POINTS; ++i) {
    signature[i] -= mean;
    power += signature[i] * signature[i];
  }
  float rms = sqrtf(power / SIGNATURE_POINTS);
  if (rms < 1e-6f) rms = 1.0f;
  for (uint16_t i = 0; i < SIGNATURE_POINTS; ++i) signature[i] /= rms;
}

HitAnalysis analyzeHit(const EventBuffer &event) {
  HitAnalysis result = {};
  result.model = emptyTingguModelOutput();
  uint32_t preCount = min(event.triggerIndex, event.sampleRateHz * PRE_TRIGGER_MS / 1000UL);
  if (preCount < 8 || event.piezoCount <= event.triggerIndex + FFT_SIZE) return result;

  double baselineSum = 0.0;
  for (uint32_t i = 0; i < preCount; ++i) baselineSum += event.piezo[i];
  float baseline = baselineSum / preCount;
  double noisePower = 0.0;
  for (uint32_t i = 0; i < preCount; ++i) {
    float value = event.piezo[i] - baseline;
    noisePower += value * value;
  }
  noisePower /= preCount;
  float noiseRms = sqrtf(noisePower);
  if (noiseRms < 0.001f) noiseRms = 0.001f;

  double eventPower = 0.0;
  uint32_t eventPowerCount = 0;
  result.piezoSaturated = false;
  uint32_t powerEnd = min(event.piezoCount, event.triggerIndex + event.sampleRateHz * 300 / 1000UL);
  for (uint32_t i = event.triggerIndex; i < event.piezoCount; ++i) {
    if (event.piezo[i] <= 5 || event.piezo[i] >= 4090) result.piezoSaturated = true;
    if (i < powerEnd) {
      float value = event.piezo[i] - baseline;
      eventPower += value * value;
      ++eventPowerCount;
    }
  }
  if (eventPowerCount > 0) eventPower /= eventPowerCount;
  result.piezoSnrDb = finiteSnr((float)eventPower, (float)noisePower);

  for (uint16_t i = 0; i < FFT_SIZE; ++i) {
    float value = event.piezo[event.triggerIndex + i] - baseline;
    float window = 0.5f - 0.5f * cosf(2.0f * PI * i / (FFT_SIZE - 1));
    fftReal[i] = value * window;
    fftImag[i] = 0.0f;
  }
  fft512(fftReal, fftImag);

  float bestPower = -1.0f;
  float lowEnergy = 0.0f;
  float highEnergy = 0.0f;
  uint16_t bestBin = 0;
  float nyquistLimit = event.sampleRateHz * 0.4f;
  float highLimit = min(HIGH_BAND_MAX_HZ, nyquistLimit);
  for (uint16_t bin = 1; bin < FFT_SIZE / 2; ++bin) {
    float frequency = bin * event.sampleRateHz / (float)FFT_SIZE;
    float power = fftReal[bin] * fftReal[bin] + fftImag[bin] * fftImag[bin];
    if (frequency >= 5.0f && frequency <= nyquistLimit && power > bestPower) {
      bestPower = power;
      bestBin = bin;
    }
    if (frequency >= LOW_BAND_MIN_HZ && frequency < LOW_BAND_MAX_HZ) lowEnergy += power;
    else if (frequency >= LOW_BAND_MAX_HZ && frequency <= highLimit) highEnergy += power;
  }
  result.f1Hz = bestBin * event.sampleRateHz / (float)FFT_SIZE;
  result.energyRatio = highEnergy / max(lowEnergy, 1e-12f);
  result.tauMs = estimateTauMs(event, baseline, noiseRms);
  buildSignature(event, baseline, result.signature);

  // MPU quality on the automatically selected dynamic acceleration axis.
  uint16_t mpuPreCount = 0;
  double meanAx = 0.0, meanAy = 0.0, meanAz = 0.0;
  for (uint16_t i = 0; i < event.mpuCount; ++i) {
    int32_t relativeUs = (int32_t)(event.mpu[i].timestampUs - event.triggerTimestampUs);
    if (relativeUs < 0) {
      meanAx += event.mpu[i].ax; meanAy += event.mpu[i].ay; meanAz += event.mpu[i].az;
      ++mpuPreCount;
    }
  }
  if (mpuPreCount > 0) { meanAx /= mpuPreCount; meanAy /= mpuPreCount; meanAz /= mpuPreCount; }
  double prePower[3] = {0.0, 0.0, 0.0};
  double postPower[3] = {0.0, 0.0, 0.0};
  uint16_t postCount = 0;
  result.mpuSaturated = false;
  for (uint16_t i = 0; i < event.mpuCount; ++i) {
    const MpuTimedSample &sample = event.mpu[i];
    float values[3] = {(float)(sample.ax - meanAx), (float)(sample.ay - meanAy), (float)(sample.az - meanAz)};
    if (abs((int)sample.ax) >= 32760 || abs((int)sample.ay) >= 32760 || abs((int)sample.az) >= 32760) result.mpuSaturated = true;
    int32_t relativeUs = (int32_t)(sample.timestampUs - event.triggerTimestampUs);
    if (relativeUs < 0) {
      for (uint8_t axis = 0; axis < 3; ++axis) prePower[axis] += values[axis] * values[axis];
    } else if (relativeUs <= 300000) {
      for (uint8_t axis = 0; axis < 3; ++axis) postPower[axis] += values[axis] * values[axis];
      ++postCount;
    }
  }
  uint8_t dominantAxis = 0;
  if (postPower[1] > postPower[dominantAxis]) dominantAxis = 1;
  if (postPower[2] > postPower[dominantAxis]) dominantAxis = 2;
  float mpuNoisePower = mpuPreCount > 0 ? prePower[dominantAxis] / mpuPreCount : 1e12f;
  float mpuEventPower = postCount > 0 ? postPower[dominantAxis] / postCount : 0.0f;
  result.mpuSnrDb = finiteSnr(mpuEventPower, mpuNoisePower);
  uint32_t expectedMpu = MPU_SAMPLE_RATE_HZ * (PRE_TRIGGER_MS + POST_TRIGGER_MS) / 1000UL;
  result.dropRate = expectedMpu > 0 ? max(0.0f, 1.0f - event.mpuCount / (float)expectedMpu) : 1.0f;
  if (event.dmaOverflowCount > 0) result.dropRate = 1.0f;

  result.qualityValid = !result.piezoSaturated && !result.mpuSaturated &&
    result.piezoSnrDb >= PIEZO_MIN_SNR_DB && result.mpuSnrDb >= MPU_MIN_SNR_DB &&
    result.dropRate < MAX_DROP_RATE && mpuAvailable;
  result.modelReady = extractSpectralModelFeatures(event, result.modelFeatures);
  if (result.modelReady) result.model = inferTingguModel(result.modelFeatures);
  result.complete = true;
  return result;
}

float signatureCorrelation(const float *a, const float *b) {
  float sum = 0.0f;
  for (uint16_t i = 0; i < SIGNATURE_POINTS; ++i) sum += a[i] * b[i];
  return sum / SIGNATURE_POINTS;
}

void finishMeasurementIfReady() {
  if (processedHitCount < HITS_PER_MEASUREMENT) return;
  latestResult.measurementId = activeMeasurementId;
  for (uint8_t i = 0; i < HITS_PER_MEASUREMENT; ++i) latestResult.hits[i] = hitResults[i];
  float c01 = signatureCorrelation(hitResults[0].signature, hitResults[1].signature);
  float c02 = signatureCorrelation(hitResults[0].signature, hitResults[2].signature);
  float c12 = signatureCorrelation(hitResults[1].signature, hitResults[2].signature);
  float consistency = (c01 + c02 + c12) / 3.0f;

  latestResult.features[0] = medianOfThree(hitResults[0].f1Hz, hitResults[1].f1Hz, hitResults[2].f1Hz);
  latestResult.features[1] = medianOfThree(hitResults[0].tauMs, hitResults[1].tauMs, hitResults[2].tauMs);
  latestResult.features[2] = medianOfThree(hitResults[0].energyRatio, hitResults[1].energyRatio, hitResults[2].energyRatio);
  latestResult.features[3] = consistency;
  latestResult.valid = consistency >= MIN_CONSISTENCY;
  for (uint8_t i = 0; i < HITS_PER_MEASUREMENT; ++i) latestResult.valid &= hitResults[i].qualityValid;
  TingguModelOutput perHitModels[HITS_PER_MEASUREMENT];
  for (uint8_t i = 0; i < HITS_PER_MEASUREMENT; ++i) perHitModels[i] = hitResults[i].model;
  latestResult.model = latestResult.valid
    ? averageTingguModelOutputs(perHitModels, HITS_PER_MEASUREMENT)
    : emptyTingguModelOutput();
  systemState = STATE_RESULT_HOLDING;

  serialPrintf("#MEASUREMENT_RESULT,id=%lu,valid=%d,f1_hz=%.4f,tau_ms=%.4f,e_ratio=%.6f,consistency_c=%.6f,class=%s,confidence=%.4f,p_tight=%.5f,p_medium=%.5f,p_loose=%.5f,model_version=%s\n",
    (unsigned long)latestResult.measurementId, latestResult.valid ? 1 : 0,
    latestResult.features[0], latestResult.features[1], latestResult.features[2], latestResult.features[3],
    latestResult.valid ? tingguClassName(latestResult.model.classification) : "MEASUREMENT_INVALID",
    latestResult.model.confidence, latestResult.model.probabilities[0],
    latestResult.model.probabilities[1], latestResult.model.probabilities[2], TINGGU_MODEL_VERSION);
  tingguDisplayResult(latestResult.valid ? tingguClassName(latestResult.model.classification) : "MEASUREMENT_INVALID",
                      latestResult.model.confidence);
}

void processingTask(void *parameter) {
  (void)parameter;
  for (;;) {
    uint8_t bufferIndex;
    if (xQueueReceive(readyBufferQueue, &bufferIndex, portMAX_DELAY) != pdTRUE) continue;
    EventBuffer &event = *eventBuffers[bufferIndex];
    if (event.measurementId != activeMeasurementId) {
      xQueueSend(freeBufferQueue, &bufferIndex, portMAX_DELAY);
      continue;
    }
    HitAnalysis analysis = analyzeHit(event);
    uint8_t resultIndex = event.hitNumber > 0 ? event.hitNumber - 1 : 0;
    if (resultIndex < HITS_PER_MEASUREMENT) hitResults[resultIndex] = analysis;
    ++processedHitCount;
    serialPrintf("#HIT_FEATURES,measurement_id=%lu,strike_index=%u,valid=%d,piezo_snr_db=%.3f,mpu_snr_db=%.3f,drop_rate=%.6f,f1_hz=%.4f,tau_ms=%.4f,e_ratio=%.6f\n",
      (unsigned long)event.measurementId, event.hitNumber, analysis.qualityValid ? 1 : 0,
      analysis.piezoSnrDb, analysis.mpuSnrDb, analysis.dropRate,
      analysis.f1Hz, analysis.tauMs, analysis.energyRatio);
    serialPrintf("#HIT_MODEL,measurement_id=%lu,strike_index=%u,ready=%d,class=%s,confidence=%.4f,p_tight=%.5f,p_medium=%.5f,p_loose=%.5f\n",
      (unsigned long)event.measurementId, event.hitNumber, analysis.modelReady ? 1 : 0,
      tingguClassName(analysis.model.classification), analysis.model.confidence,
      analysis.model.probabilities[0], analysis.model.probabilities[1], analysis.model.probabilities[2]);
    xQueueSend(freeBufferQueue, &bufferIndex, portMAX_DELAY);
    finishMeasurementIfReady();
  }
}

// ---------------------------------------------------------------------------
// Serial/control task (Arduino loop runs on Core 1)
// ---------------------------------------------------------------------------
void printStatus() {
  serialPrintf("#STATUS,state=%s,measurement_id=%lu,captured=%u,processed=%u,piezo_rate=%lu,mpu_rate=%lu,baseline=%.3f,noise_rms=%.3f,threshold=%u,mpu=%d,address=0x%02X,who_am_i=0x%02X,mpu_model=%s,classifier=%s,core_acq=%d,core_process=%d\n",
    stateName(systemState), (unsigned long)activeMeasurementId, capturedHitCount, processedHitCount,
    (unsigned long)PIEZO_SAMPLE_RATE_HZ, (unsigned long)MPU_SAMPLE_RATE_HZ,
    piezoBaseline, piezoNoiseRms, piezoTriggerThreshold, mpuAvailable ? 1 : 0, activeMpuAddress,
    activeMpuWhoAmI, mpuAvailable ? mpuModelName() : "NONE", TINGGU_MODEL_VERSION,
    acquisitionTaskHandle ? xTaskGetCoreID(acquisitionTaskHandle) : -1,
    processingTaskHandle ? xTaskGetCoreID(processingTaskHandle) : -1);
}

void handleCommand(const char *command) {
  if (strcmp(command, "ARM_MEASUREMENT") == 0 || strcmp(command, "ARM") == 0) {
    armMeasurement();
    serialPrintf("#ACK,ARM_MEASUREMENT\n");
  } else if (strcmp(command, "ABORT") == 0) {
    abortMeasurement();
    serialPrintf("#ACK,ABORT\n");
  } else if (strcmp(command, "RECALIBRATE") == 0) {
    abortMeasurement();
    recalibrationRequested = true;
    serialPrintf("#ACK,RECALIBRATE\n");
  } else if (strcmp(command, "STATUS") == 0) {
    printStatus();
  } else {
    serialPrintf("#ERROR,UNKNOWN_COMMAND,%s\n", command);
  }
}

void processSerial() {
  while (Serial.available()) {
    char value = (char)Serial.read();
    if (value == '\r') continue;
    if (value == '\n') {
      commandBuffer[commandLength] = '\0';
      for (uint8_t i = 0; i < commandLength; ++i) {
        if (commandBuffer[i] >= 'a' && commandBuffer[i] <= 'z') commandBuffer[i] -= 32;
      }
      if (commandLength > 0) handleCommand(commandBuffer);
      commandLength = 0;
    } else if (commandLength < sizeof(commandBuffer) - 1) {
      commandBuffer[commandLength++] = value;
    }
  }
}

void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(300);
  serialMutex = xSemaphoreCreateMutex();
  stateMutex = xSemaphoreCreateMutex();
  mpuMutex = xSemaphoreCreateMutex();
  freeBufferQueue = xQueueCreate(2, sizeof(uint8_t));
  readyBufferQueue = xQueueCreate(2, sizeof(uint8_t));
  runtimeEventQueue = xQueueCreate(12, sizeof(RuntimeEvent));
  preTriggerRing = (uint16_t *)heap_caps_calloc(PRE_RING_CAPACITY, sizeof(uint16_t), MALLOC_CAP_8BIT);
  eventBuffers[0] = (EventBuffer *)heap_caps_calloc(1, sizeof(EventBuffer), MALLOC_CAP_8BIT);
  eventBuffers[1] = (EventBuffer *)heap_caps_calloc(1, sizeof(EventBuffer), MALLOC_CAP_8BIT);
  if (preTriggerRing == NULL || eventBuffers[0] == NULL || eventBuffers[1] == NULL) {
    systemState = STATE_FAULT;
    serialPrintf("#FATAL,EVENT_BUFFER_ALLOCATION_FAILED,required_each=%u,free_heap=%u\n",
      (unsigned)sizeof(EventBuffer), (unsigned)ESP.getFreeHeap());
    return;
  }
  uint8_t zero = 0, one = 1;
  xQueueSend(freeBufferQueue, &zero, 0);
  xQueueSend(freeBufferQueue, &one, 0);

  serialPrintf("\n#HELLO,TINGGU_EDGE_RUNTIME,1\n");
  systemState = STATE_SELF_TEST;

  if (ARM_BUTTON_PIN >= 0) pinMode(ARM_BUTTON_PIN, ARM_BUTTON_ACTIVE_LOW ? INPUT_PULLUP : INPUT);
  Wire.begin(MPU_SDA_PIN, MPU_SCL_PIN, I2C_CLOCK_HZ);
  Wire.setTimeOut(5);
  mpuAvailable = configureMpu();
  if (mpuAvailable) {
    serialPrintf("#MPU,FOUND,0x%02X,who_am_i=0x%02X,model=%s\n",
      activeMpuAddress, activeMpuWhoAmI, mpuModelName());
  } else {
    serialPrintf("#MPU_NOT_FOUND,0x00\n");
  }

  if (!beginAdcDma()) {
    systemState = STATE_FAULT;
    serialPrintf("#FATAL,ADC_DMA_INIT_FAILED,pin=%d,stage=%s,error=%s\n",
      PIEZO_ADC_PIN, adcInitStage, esp_err_to_name(adcInitError));
    return;
  }
  serialPrintf("#ADC_DMA,READY,pin=%d,unit=%d,channel=%d\n",
    PIEZO_ADC_PIN, piezoAdcUnit == ADC_UNIT_1 ? 1 : 2, (int)piezoAdcChannel);

  tingguDisplayBegin();
  resetCalibration();
  xTaskCreatePinnedToCore(mpuTask, "tinggu_mpu", 4096, NULL, 4, &mpuTaskHandle, 0);
  xTaskCreatePinnedToCore(acquisitionTask, "tinggu_acquisition", 6144, NULL, 5, &acquisitionTaskHandle, 0);
  xTaskCreatePinnedToCore(processingTask, "tinggu_processing", 8192, NULL, 2, &processingTaskHandle, 1);
  serialPrintf("#READY,CALIBRATING_KEEP_STILL\n");
}

void loop() {
  processSerial();
  RuntimeEvent event;
  while (runtimeEventQueue && xQueueReceive(runtimeEventQueue, &event, 0) == pdTRUE) {
    switch (event.type) {
      case EVENT_CALIBRATION_DONE:
        serialPrintf("#CALIBRATION,baseline=%.3f,noise_rms=%.3f,threshold=%u\n",
          piezoBaseline, piezoNoiseRms, piezoTriggerThreshold);
        break;
      case EVENT_HIT_TRIGGERED:
        serialPrintf("#TRIGGER,measurement_id=%lu,strike_index=%u\n",
          (unsigned long)activeMeasurementId, event.hitNumber);
        break;
      case EVENT_HIT_CAPTURED:
        serialPrintf("#CAPTURED,measurement_id=%lu,strike_index=%u,samples=%lu\n",
          (unsigned long)activeMeasurementId, event.hitNumber, (unsigned long)event.value);
        break;
      case EVENT_MEASUREMENT_TIMEOUT:
        serialPrintf("#MEASUREMENT_INVALID,reason=HIT_TIMEOUT,next_strike=%u\n", event.hitNumber);
        break;
      case EVENT_DMA_OVERFLOW:
        serialPrintf("#WARNING,ADC_DMA_READ_ERROR,code=%lu\n", (unsigned long)event.value);
        break;
      case EVENT_BUFFER_UNAVAILABLE:
        serialPrintf("#FAULT,NO_FREE_EVENT_BUFFER\n");
        break;
      case EVENT_READY_QUEUE_FULL:
        serialPrintf("#FAULT,PROCESSING_QUEUE_FULL\n");
        break;
    }
  }

  static bool lastButton = false;
  if (ARM_BUTTON_PIN >= 0) {
    bool pressed = digitalRead(ARM_BUTTON_PIN) == (ARM_BUTTON_ACTIVE_LOW ? LOW : HIGH);
    if (pressed && !lastButton) armMeasurement();
    lastButton = pressed;
  }

  static SystemState lastState = STATE_BOOT;
  if (systemState != lastState) {
    serialPrintf("#STATE,%s\n", stateName(systemState));
    tingguDisplayState(stateName(systemState));
    lastState = systemState;
  }
  delay(2);
}
