# Tinggu edge runtime

这是比赛端侧固件骨架，不替代已经验证可用的 `tinggu_signal_validator`。

## 当前已实现

- 底层 `adc_continuous` DMA保留每个压电ADC原始点；
- MPU-6050原始I2C驱动；
- 采集任务固定到Core 0，分析任务固定到Core 1；
- 两个固定事件缓冲区和FreeRTOS Queue所有权传递；
- 三次外部敲击组成一次测量；
- 质量门控、512点Hann FFT、`f1/tau/Eratio/C`；
- 240 ms固定截窗、40项频谱/时频/相干特征和三分类LDA模型；
- 三次敲击分别推理，再平均三类概率输出最终结果；
- 模型和OLED的独立替换接口；
- 撞针完全外部触发，没有撞针控制GPIO。

## 当前硬件接口

1. 当前ESP32-S3接口为 `PIEZO_ADC_PIN=4`（ADC1_CH3）、`MPU_SDA_PIN=17`、`MPU_SCL_PIN=15`；完整模拟前端见 `docs/tonight_bringup_wiring.md`。
2. 上电标定的一秒内保持铁板静止。
3. 串口发送 `ARM_MEASUREMENT`，然后用外部撞针敲三次。
4. 当前模型为 `spectral_lda40_20260903`，类别为 `TIGHT/MEDIUM/LOOSE`；最终置信度低于0.50时输出 `UNCERTAIN`。
5. `#HIT_MODEL`输出每次敲击的概率；`#MEASUREMENT_RESULT`输出三次概率平均后的类别。
6. 当前模型的同日分组验证准确率约81.8%，仍必须用下一次独立实验验证，不能把它当作最终准确率。
7. 当前 `display_interface.h` 是空实现；OLED准确型号、地址和引脚冻结后再接库。

## 串口命令

- `ARM_MEASUREMENT`：开始等待三次敲击；
- `ABORT`：中止本次测量；
- `RECALIBRATE`：重新静置标定；
- `STATUS`：输出任务核心、状态、采样率和传感器状态。
