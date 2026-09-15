/*
 * Tinggu edge runtime - external electromagnetic striker edition
 *
 * Core 0:
 *   ADC continuous DMA, piezo trigger/windowing, MPU-6050 sampling.
 * Core 1:
 *   quality gates, 14 waveform features (up to 4096-point FFT), tree/OLED/serial.
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
#include "waveform_features.h"
#include "model_selftest.h"

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
const uint16_t POST_TRIGGER_MS = 1200; // V16 max window; finish earlier only after 50 ms quiet.
const uint16_t MIN_EVENT_SAMPLES = 1400;
const uint16_t CAPTURE_QUIET_SAMPLES = 100;
const uint16_t REFRACTORY_MS = 200;
const uint16_t STRIKE_PREPARE_MS = 2000;
const uint16_t STRIKE_TIMEOUT_MS = 5000;

const uint16_t MIN_TRIGGER_COUNTS = 20; // Match waveform training V16 trigger.
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
const uint32_t MAX_PIEZO_SAMPLES = 2600;
const uint32_t PRE_RING_CAPACITY = PIEZO_SAMPLE_RATE_HZ * PRE_TRIGGER_MS / 1000;
const uint16_t MAX_MPU_SAMPLES = 1400;
const uint8_t HITS_PER_MEASUREMENT = 3;

const float ACCEL_LSB_PER_G = 2048.0f; // MPU +/-16 g.
const float GYRO_LSB_PER_DPS = 32.8f;  // MPU +/-1000 dps.

static_assert(PIEZO_SAMPLE_RATE_HZ == 2000, "Waveform tree was trained at exactly 2000 Hz");
static_assert(FFT_SIZE == 512, "The current FFT workspace is fixed at 512");

// ---------------------------------------------------------------------------
// Runtime types
// ---------------------------------------------------------------------------
enum SystemState {
  STATE_BOOT,
  STATE_SELF_TEST,
  STATE_CALIBRATING,
  STATE_IDLE,
  STATE_STRIKE_PREPARING,
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
  uint16_t attemptNumber;
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
  double modelFeatures[TINGGU_MODEL_FEATURE_COUNT];
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
volatile uint8_t acceptedHitCount = 0;
volatile uint32_t strikePrepareStartMs = 0;
volatile uint32_t strikeWaitStartMs = 0;
volatile uint8_t preparingStrikeNumber = 0;
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

// Single processing-task workspace; no large stack allocation.
TingguWaveform14::Workspace treeWorkspace;
bool modelSelfTestPassed = false;
volatile float mpuQuietMean[3] = {0,0,0};
volatile float mpuQuietNoiseG = 0;
volatile uint32_t calibrationEpoch = 0;

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
    case STATE_STRIKE_PREPARING: return "STRIKE_PREPARING";
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
  if (activeMpuWhoAmI==0x70 && !mpuWriteByte(activeMpuAddress,0x1D,0x01))return false;
  return true;
}

int16_t int16FromBytes(uint8_t highByte, uint8_t lowByte) {
  return (int16_t)(((uint16_t)highByte << 8) | lowByte);
}

bool readMpuSample(MpuTimedSample &sample) {
  uint8_t data[14];
  if (!mpuAvailable || !mpuReadBytes(activeMpuAddress, 0x3B, data, sizeof(data))) return false;
  bool allFF=true;for(unsigned i=0;i<14;++i)allFF &= data[i]==0xFF;
  if(allFF)return false;
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
  TingguWaveform14::Moments calibrationAxes[3];
  uint32_t seenEpoch=calibrationEpoch;
  TickType_t lastWake = xTaskGetTickCount();
  const TickType_t period = max((TickType_t)1, pdMS_TO_TICKS(1000UL / MPU_SAMPLE_RATE_HZ));
  for (;;) {
    if (mpuAvailable) {
      MpuTimedSample sample;
      if (readMpuSample(sample)) {
        if(seenEpoch!=calibrationEpoch){for(auto &axis:calibrationAxes)axis=TingguWaveform14::Moments();seenEpoch=calibrationEpoch;}
        if(systemState==STATE_CALIBRATING){
          calibrationAxes[0].add(sample.ax);calibrationAxes[1].add(sample.ay);calibrationAxes[2].add(sample.az);
          double variance=0;for(unsigned a=0;a<3;++a){mpuQuietMean[a]=calibrationAxes[a].mean;variance+=calibrationAxes[a].stddev()*calibrationAxes[a].stddev();}
          mpuQuietNoiseG=sqrt(variance)/ACCEL_LSB_PER_G;
        }
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
  uint32_t windowStart = event.eventStartTimestampUs;
  uint32_t windowEnd = windowStart + (event.piezoCount - 1) * 500UL;
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

// DMA batches lag wall time; use MPU samples at/before the ADC sample, never a future sample.
bool mpuQuietAt(uint32_t timestamp) {
  bool quiet=false;
  xSemaphoreTake(mpuMutex,portMAX_DELAY);
  for(unsigned back=0;back<mpuRingCount;++back){
    const auto &v=mpuRing[(mpuRingWrite+MAX_MPU_SAMPLES-1-back)%MAX_MPU_SAMPLES];
    int32_t age=(int32_t)(timestamp-v.timestampUs);if(age<0)continue;
    if(age<=5000){float dx=(v.ax-mpuQuietMean[0])/ACCEL_LSB_PER_G,dy=(v.ay-mpuQuietMean[1])/ACCEL_LSB_PER_G,dz=(v.az-mpuQuietMean[2])/ACCEL_LSB_PER_G;
      quiet=sqrtf(dx*dx+dy*dy+dz*dz)<max(0.02f,3*mpuQuietNoiseG);}
    break;
  }
  xSemaphoreGive(mpuMutex);return quiet;
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
  ++calibrationEpoch;
  systemState = STATE_CALIBRATING;
}

void announceStrikePreparation(uint8_t strikeNumber) {
  serialPrintf("#PROMPT,PREPARE_STRIKE,index=%u,total=%u,countdown_ms=%u\n",
    strikeNumber, HITS_PER_MEASUREMENT, STRIKE_PREPARE_MS);
  tingguDisplayState("PREPARE_STRIKE");
}

bool armMeasurement() {
  bool started = false;
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  if (modelSelfTestPassed && (systemState == STATE_IDLE || systemState == STATE_RESULT_HOLDING)) {
    ++activeMeasurementId;
    capturedHitCount = 0;
    processedHitCount = 0;
    acceptedHitCount = 0;
    preparingStrikeNumber = 1;
    strikePrepareStartMs = millis();
    strikeWaitStartMs = 0;
    refractoryUntilMs = 0;
    memset(hitResults, 0, sizeof(hitResults));
    memset(&latestResult, 0, sizeof(latestResult));
    latestResult.measurementId = activeMeasurementId;
    systemState = STATE_STRIKE_PREPARING;
    started = true;
  }
  xSemaphoreGive(stateMutex);
  return started;
}

void abortMeasurement() {
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  ++activeMeasurementId; // Invalidates any event that was already queued.
  capturedHitCount = 0;
  processedHitCount = 0;
  acceptedHitCount = 0;
  preparingStrikeNumber = 0;
  systemState = STATE_IDLE;
  xSemaphoreGive(stateMutex);
}

int beginTriggeredEvent(uint16_t triggerRaw, uint32_t triggerTimestampUs, unsigned confirmSamples) {
  uint8_t bufferIndex;
  if (xQueueReceive(freeBufferQueue, &bufferIndex, 0) != pdTRUE) {
    systemState = STATE_FAULT;
    postRuntimeEvent(EVENT_BUFFER_UNAVAILABLE, capturedHitCount + 1, 0);
    return -1;
  }

  EventBuffer &event = *eventBuffers[bufferIndex];
  event.measurementId = activeMeasurementId;
  event.hitNumber = acceptedHitCount + 1;
  event.attemptNumber = capturedHitCount + 1;
  event.sampleRateHz = PIEZO_SAMPLE_RATE_HZ;
  event.triggerTimestampUs = triggerTimestampUs;
  event.eventStartTimestampUs = triggerTimestampUs - (200-confirmSamples)*500UL;
  event.dmaOverflowStartCount = adcPoolOverflowCount;
  event.dmaOverflowCount = 0;
  event.expectedPiezoSamples = PIEZO_SAMPLE_RATE_HZ * (PRE_TRIGGER_MS + POST_TRIGGER_MS) / 1000UL;
  event.piezoCount = 0;

  uint32_t requiredPre = 199; // Current confirming row is appended below.
  uint32_t availablePre = min(preRingCount, requiredPre);
  uint32_t oldest = (preRingWrite + PRE_RING_CAPACITY - availablePre) % PRE_RING_CAPACITY;
  for (uint32_t i = 0; i < availablePre; ++i) {
    event.piezo[event.piezoCount++] = preTriggerRing[(oldest + i) % PRE_RING_CAPACITY];
  }
  while (event.piezoCount < requiredPre) event.piezo[event.piezoCount++] = (uint16_t)piezoBaseline;
  event.piezo[event.piezoCount++] = triggerRaw;
  event.triggerIndex = 200-confirmSamples;

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
  unsigned candidateSamples=0,candidateActive=0,candidateSum=0,quietSamples=0;
  uint32_t candidateTimestamp=0;

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

    uint32_t batchEndUs=micros();
    for (uint32_t offset = 0; offset < bytesRead; offset += SOC_ADC_DIGI_RESULT_BYTES) {
      uint16_t raw = adcRawValue(&dmaData[offset]);
      uint32_t sampleTimestampUs = batchEndUs -
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

      if(systemState!=STATE_MEASUREMENT_ARMED){candidateSamples=candidateActive=candidateSum=0;}
      if (systemState == STATE_MEASUREMENT_ARMED) {
        if (millis() - strikeWaitStartMs > STRIKE_TIMEOUT_MS) {
          systemState = STATE_RESULT_HOLDING;
           postRuntimeEvent(EVENT_MEASUREMENT_TIMEOUT, acceptedHitCount + 1, 0);
        } else if ((int32_t)(millis() - refractoryUntilMs) >= 0) {
          int amplitude = abs((int)raw - (int)lroundf(piezoBaseline));
          if(amplitude>=piezoTriggerThreshold || candidateSamples){
            if(!candidateSamples)candidateTimestamp=sampleTimestampUs;
            ++candidateSamples;candidateSum+=amplitude;
            if(amplitude>=ceilf(max(8.0f,3*piezoNoiseRms)))++candidateActive;
            if(candidateActive>=3 && candidateSum>=3U*piezoTriggerThreshold){
              activeBuffer=beginTriggeredEvent(raw,candidateTimestamp,candidateSamples);quietSamples=0;
              candidateSamples=candidateActive=candidateSum=0;
            }else if(candidateSamples>=10){candidateSamples=candidateActive=candidateSum=0;}
          }
        }
      } else if (systemState == STATE_CAPTURING && activeBuffer >= 0) {
        EventBuffer &event = *eventBuffers[activeBuffer];
        if (event.piezoCount < event.expectedPiezoSamples && event.piezoCount < MAX_PIEZO_SAMPLES) {
          event.piezo[event.piezoCount++] = raw;
        }
        bool piezoQuiet=abs((int)raw-(int)lroundf(piezoBaseline))<ceilf(max(8.0f,3*piezoNoiseRms));
        bool mpuQuiet=mpuQuietAt(sampleTimestampUs);
        if(piezoQuiet && (!mpuAvailable || mpuQuiet))++quietSamples;else quietSamples=0;
        if (event.piezoCount >= event.expectedPiezoSamples ||
            (event.piezoCount>=MIN_EVENT_SAMPLES && quietSamples>=CAPTURE_QUIET_SAMPLES)) {
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
            systemState = STATE_PROCESSING;
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

#include "tree_event_adapter.h"

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
  uint32_t expectedMpu = event.piezoCount / 2;
  result.dropRate = expectedMpu > 0 ? max(0.0f, 1.0f - event.mpuCount / (float)expectedMpu) : 1.0f;
  if (event.dmaOverflowCount > 0) result.dropRate = 1.0f;

  result.modelReady = modelSelfTestPassed && extractWaveformModelFeatures(event, treeWorkspace, result.modelFeatures);
  if (result.modelReady) result.model = inferTingguModel(result.modelFeatures);
  result.qualityValid = !result.piezoSaturated && !result.mpuSaturated &&
    result.piezoSnrDb >= PIEZO_MIN_SNR_DB && result.mpuSnrDb >= MPU_MIN_SNR_DB &&
    result.dropRate < MAX_DROP_RATE && mpuAvailable && result.modelReady;
  result.complete = true;
  return result;
}

float signatureCorrelation(const float *a, const float *b) {
  float sum = 0.0f;
  for (uint16_t i = 0; i < SIGNATURE_POINTS; ++i) sum += a[i] * b[i];
  return sum / SIGNATURE_POINTS;
}

void finishMeasurementIfReady() {
  if (acceptedHitCount < HITS_PER_MEASUREMENT) return;
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
  // Three accepted A-grade strikes are sufficient for inference. Consistency
  // remains a diagnostic feature, but it must not discard a group that has
  // already passed all three per-strike quality gates.
  latestResult.valid = true;
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

void beginNextStrikePreparation(uint8_t strikeNumber) {
  preparingStrikeNumber = strikeNumber;
  strikePrepareStartMs = millis();
  strikeWaitStartMs = 0;
  systemState = STATE_STRIKE_PREPARING;
  announceStrikePreparation(strikeNumber);
}

void emitHitPreview(const EventBuffer &event) {
  // The edge runtime keeps the full-rate samples for feature extraction, but the
  // desktop UI only needs a compact visual preview.  Sending 101 decimated
  // points avoids the multi-second serial transfer used by the validator.
  const uint16_t previewPoints = 101;
  if (event.piezoCount < 2 || event.sampleRateHz == 0) return;

  uint32_t piezoBaselineCount = event.triggerIndex;
  if (piezoBaselineCount == 0 || piezoBaselineCount > event.piezoCount)
    piezoBaselineCount = event.piezoCount < 64 ? event.piezoCount : 64;
  double piezoBaselineSum = 0.0;
  for (uint32_t i = 0; i < piezoBaselineCount; ++i) piezoBaselineSum += event.piezo[i];
  float previewPiezoBaseline = (float)(piezoBaselineSum / piezoBaselineCount);

  double meanAx = 0.0, meanAy = 0.0, meanAz = 0.0;
  uint16_t mpuBaselineCount = 0;
  for (uint16_t i = 0; i < event.mpuCount; ++i) {
    if ((int32_t)(event.mpu[i].timestampUs - event.triggerTimestampUs) >= 0) break;
    meanAx += event.mpu[i].ax;
    meanAy += event.mpu[i].ay;
    meanAz += event.mpu[i].az;
    ++mpuBaselineCount;
  }
  if (mpuBaselineCount == 0 && event.mpuCount > 0) {
    meanAx = event.mpu[0].ax;
    meanAy = event.mpu[0].ay;
    meanAz = event.mpu[0].az;
    mpuBaselineCount = 1;
  }
  if (mpuBaselineCount > 0) {
    meanAx /= mpuBaselineCount;
    meanAy /= mpuBaselineCount;
    meanAz /= mpuBaselineCount;
  }

  serialPrintf("#PREVIEW_BEGIN,measurement_id=%lu,strike_index=%u,count=%u\n",
    (unsigned long)event.measurementId, event.hitNumber, previewPoints);
  uint16_t mpuCursor = 0;
  for (uint16_t point = 0; point < previewPoints; ++point) {
    uint32_t piezoIndex = (uint32_t)point * (event.piezoCount - 1) / (previewPoints - 1);
    float timeMs = ((int32_t)piezoIndex - (int32_t)event.triggerIndex) *
      1000.0f / event.sampleRateHz;
    int64_t targetTimestampUs = (int64_t)event.triggerTimestampUs + (int64_t)(timeMs * 1000.0f);
    while (mpuCursor + 1 < event.mpuCount &&
           (int64_t)event.mpu[mpuCursor + 1].timestampUs <= targetTimestampUs)
      ++mpuCursor;
    float mpuDynamicG = 0.0f;
    if (event.mpuCount > 0 && mpuBaselineCount > 0) {
      float dx = (event.mpu[mpuCursor].ax - meanAx) / ACCEL_LSB_PER_G;
      float dy = (event.mpu[mpuCursor].ay - meanAy) / ACCEL_LSB_PER_G;
      float dz = (event.mpu[mpuCursor].az - meanAz) / ACCEL_LSB_PER_G;
      mpuDynamicG = sqrtf(dx * dx + dy * dy + dz * dz);
    }
    serialPrintf("#PREVIEW_POINT,index=%u,time_ms=%.3f,piezo_delta=%.3f,mpu_dynamic_g=%.6f\n",
      point, timeMs, event.piezo[piezoIndex] - previewPiezoBaseline, mpuDynamicG);
  }
  serialPrintf("#PREVIEW_END,measurement_id=%lu,strike_index=%u\n",
    (unsigned long)event.measurementId, event.hitNumber);
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
    uint32_t eventMeasurementId = event.measurementId;
    HitAnalysis analysis = analyzeHit(event);
    if(analysis.modelReady){
      char featureLine[768];int used=snprintf(featureLine,sizeof(featureLine),"#MODEL_FEATURES,id=%lu,attempt=%u",(unsigned long)eventMeasurementId,event.attemptNumber);
      for(unsigned f=0;f<14 && used<(int)sizeof(featureLine)-40;++f)used+=snprintf(featureLine+used,sizeof(featureLine)-used,",%.12g",analysis.modelFeatures[f]);
      serialPrintf("%s\n",featureLine);
    }
    emitHitPreview(event);
    xSemaphoreTake(stateMutex,portMAX_DELAY);
    if(eventMeasurementId!=activeMeasurementId){xSemaphoreGive(stateMutex);xQueueSend(freeBufferQueue,&bufferIndex,portMAX_DELAY);continue;}
    uint8_t resultIndex = event.hitNumber > 0 ? event.hitNumber - 1 : 0;
    ++processedHitCount;
    if (analysis.qualityValid && resultIndex < HITS_PER_MEASUREMENT) {
      hitResults[resultIndex] = analysis;
      ++acceptedHitCount;
    }
    serialPrintf("#HIT_FEATURES,measurement_id=%lu,strike_index=%u,attempt_index=%u,valid=%d,piezo_snr_db=%.3f,mpu_snr_db=%.3f,drop_rate=%.6f,f1_hz=%.4f,tau_ms=%.4f,e_ratio=%.6f\n",
      (unsigned long)event.measurementId, event.hitNumber, event.attemptNumber,
      analysis.qualityValid ? 1 : 0,
      analysis.piezoSnrDb, analysis.mpuSnrDb, analysis.dropRate,
      analysis.f1Hz, analysis.tauMs, analysis.energyRatio);
    serialPrintf("#STRIKE_RESULT,measurement_id=%lu,strike_index=%u,attempt_index=%u,accepted_count=%u,required_count=%u,grade=%s,counted=%d,piezo_saturated=%d,mpu_saturated=%d,piezo_snr_ok=%d,mpu_snr_ok=%d,drop_rate_ok=%d,model_ready=%d\n",
      (unsigned long)event.measurementId, event.hitNumber, event.attemptNumber,
      acceptedHitCount, HITS_PER_MEASUREMENT,
      analysis.qualityValid ? "A" : "INVALID", analysis.qualityValid ? 1 : 0,
      analysis.piezoSaturated ? 1 : 0, analysis.mpuSaturated ? 1 : 0,
      analysis.piezoSnrDb >= PIEZO_MIN_SNR_DB ? 1 : 0,
      analysis.mpuSnrDb >= MPU_MIN_SNR_DB ? 1 : 0,
      analysis.dropRate < MAX_DROP_RATE ? 1 : 0, analysis.modelReady ? 1 : 0);
    serialPrintf("#HIT_MODEL,measurement_id=%lu,strike_index=%u,attempt_index=%u,accepted_count=%u,ready=%d,counted=%d,class=%s,confidence=%.4f,p_tight=%.5f,p_medium=%.5f,p_loose=%.5f\n",
      (unsigned long)event.measurementId, event.hitNumber, event.attemptNumber,
      acceptedHitCount, analysis.modelReady ? 1 : 0,
      analysis.qualityValid ? 1 : 0,
      analysis.qualityValid ? tingguClassName(analysis.model.classification) : "NOT_COUNTED",
      analysis.model.confidence,
      analysis.model.probabilities[0], analysis.model.probabilities[1], analysis.model.probabilities[2]);
    xQueueSend(freeBufferQueue, &bufferIndex, portMAX_DELAY);
    if (acceptedHitCount < HITS_PER_MEASUREMENT) {
      // Invalid attempts are discarded and the same accepted-strike slot is
      // retried. A valid attempt advances to the next slot.
      beginNextStrikePreparation(acceptedHitCount + 1);
    } else {
      serialPrintf("#PROMPT,MODEL_ANALYZING,measurement_id=%lu\n",
        (unsigned long)eventMeasurementId);
      tingguDisplayState("MODEL_ANALYZING");
      finishMeasurementIfReady();
    }
    xSemaphoreGive(stateMutex);
  }
}

// ---------------------------------------------------------------------------
// Serial/control task (Arduino loop runs on Core 1)
// ---------------------------------------------------------------------------
void printStatus() {
  serialPrintf("#STATUS,state=%s,measurement_id=%lu,captured=%u,processed=%u,accepted=%u,required=%u,piezo_rate=%lu,mpu_rate=%lu,baseline=%.3f,noise_rms=%.3f,threshold=%u,mpu=%d,address=0x%02X,who_am_i=0x%02X,mpu_model=%s,classifier=%s,core_acq=%d,core_process=%d\n",
    stateName(systemState), (unsigned long)activeMeasurementId, capturedHitCount, processedHitCount,
    acceptedHitCount, HITS_PER_MEASUREMENT,
    (unsigned long)PIEZO_SAMPLE_RATE_HZ, (unsigned long)MPU_SAMPLE_RATE_HZ,
    piezoBaseline, piezoNoiseRms, piezoTriggerThreshold, mpuAvailable ? 1 : 0, activeMpuAddress,
    activeMpuWhoAmI, mpuAvailable ? mpuModelName() : "NONE", TINGGU_MODEL_VERSION,
    acquisitionTaskHandle ? xTaskGetCoreID(acquisitionTaskHandle) : -1,
    processingTaskHandle ? xTaskGetCoreID(processingTaskHandle) : -1);
}

void handleCommand(const char *command) {
  if (strcmp(command, "ARM_MEASUREMENT") == 0 || strcmp(command, "ARM") == 0) {
    if (armMeasurement()) {
      serialPrintf("#ACK,ARM_MEASUREMENT\n");
      serialPrintf("#PROMPT,START_TEST,measurement_id=%lu,required_a_strikes=%u\n",
        (unsigned long)activeMeasurementId, HITS_PER_MEASUREMENT);
      announceStrikePreparation(1);
    } else {
      serialPrintf("#ERROR,ARM_REJECTED,state=%s\n", stateName(systemState));
    }
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
  bool oledFound = tingguDisplayBegin();
  serialPrintf("#OLED,%s,SDA=8,SCL=9,address=0x%02X\n",
    oledFound ? "READY" : "NOT_FOUND", tingguOledAddress);
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
  modelSelfTestPassed=tingguModelStartupSelfTest(treeWorkspace);
  serialPrintf("#MODEL_SELFTEST,%s,features=14,source=%s\n",modelSelfTestPassed?"PASS":"FAIL",TINGGU_SELFTEST_SOURCE);
  if(!modelSelfTestPassed){systemState=STATE_FAULT;return;}
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
  serialPrintf("#MODEL_CONTRACT,version=%s,features=14,piezo_rate=2000,event_rows=1400..2600,band_split_hz=100\n",TINGGU_MODEL_VERSION);
  serialPrintf("#ADC_DMA,READY,pin=%d,unit=%d,channel=%d\n",
    PIEZO_ADC_PIN, piezoAdcUnit == ADC_UNIT_1 ? 1 : 2, (int)piezoAdcChannel);

  resetCalibration();
  xTaskCreatePinnedToCore(mpuTask, "tinggu_mpu", 4096, NULL, 4, &mpuTaskHandle, 0);
  xTaskCreatePinnedToCore(acquisitionTask, "tinggu_acquisition", 6144, NULL, 5, &acquisitionTaskHandle, 0);
  xTaskCreatePinnedToCore(processingTask, "tinggu_processing", 8192, NULL, 2, &processingTaskHandle, 1);
  serialPrintf("#READY,CALIBRATING_KEEP_STILL\n");
}

void loop() {
  processSerial();
  if (systemState == STATE_STRIKE_PREPARING &&
      millis() - strikePrepareStartMs >= STRIKE_PREPARE_MS) {
    strikeWaitStartMs = millis();
    uint8_t strikeNumber = preparingStrikeNumber;
    systemState = STATE_MEASUREMENT_ARMED;
    serialPrintf("#PROMPT,STRIKE_NOW,index=%u,total=%u,timeout_ms=%u\n",
      strikeNumber, HITS_PER_MEASUREMENT, STRIKE_TIMEOUT_MS);
    tingguDisplayState("STRIKE_NOW");
  }
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
        serialPrintf("#STRIKE_TIMEOUT,strike_index=%u,accepted_count=%u,required_count=%u\n",
          event.hitNumber, acceptedHitCount, HITS_PER_MEASUREMENT);
        beginNextStrikePreparation(acceptedHitCount + 1);
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
    if (pressed && !lastButton && armMeasurement()) {
      serialPrintf("#PROMPT,START_TEST,measurement_id=%lu,required_a_strikes=%u\n",
        (unsigned long)activeMeasurementId, HITS_PER_MEASUREMENT);
      announceStrikePreparation(1);
    }
    lastButton = pressed;
  }

  static SystemState lastState = STATE_BOOT;
  if (systemState != lastState) {
    serialPrintf("#STATE,%s\n", stateName(systemState));
    tingguDisplayState(stateName(systemState));
    lastState = systemState;
  }
  tingguDisplayProgress(acceptedHitCount, HITS_PER_MEASUREMENT);
  tingguDisplayPoll();
  delay(2);
}
