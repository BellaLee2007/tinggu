# 听固 Tinggu

听固是一个面向螺栓连接件的便携式主动敲击检测原型：用受控冲击激励结构，采集压电与 IMU 的瞬态响应，在 ESP32-S3 上提取波形特征并进行端侧 TinyML 推理。

当前仓库包含可编译的 Arduino 固件、14 维决策树模型、模型验证脚本、硬件接线文档和一个用于采集/分析串口数据的 Windows 工具。它适合复现实验、继续开发和进行原型验证；当前结果不等同于工业安全认证。

## 当前版本

当前主线固件位于 [`firmware/tinggu_edge_runtime`](firmware/tinggu_edge_runtime)，目标板为 ESP32-S3，显示设备为 SSD1306 128x64 I2C OLED，模型为 `waveform_tree14_20260913_verified_20260915`。

一次测量的输出为：

- `TIGHT`：牢固
- `MEDIUM`：轻微松动
- `LOOSE`：明显松动
- `UNCERTAIN`：输入无效或置信度不足，需要重新测量

固件包含上电模型自检；只有串口输出 `#MODEL_SELFTEST,PASS` 后才应开始测量。

## 工作流程

```text
受控主动敲击
    -> 压电 + IMU 采集瞬态响应
    -> ESP32-S3 触发、截窗与特征提取
    -> 14 维决策树推理
    -> 三次有效敲击融合
    -> OLED 与串口输出结果
```

端侧模型使用以下 14 项特征，顺序固定在模型 JSON 与固件实现中：

```text
peak_abs, peak_time_ms, rms_0_100ms, rms_100_300ms,
dominant_freq_hz, spectral_centroid_hz, low_high_energy_ratio,
decay_tau_ms, piezo_std, accel_mag_mean, accel_mag_std,
gyro_mag_mean, gyro_mag_std, mpu_valid_ratio
```

详细算法契约见 [`docs/data_contract.md`](docs/data_contract.md)。

## 快速开始

### 1. 准备 Arduino 环境

1. 克隆仓库并打开 `firmware/tinggu_edge_runtime/tinggu_edge_runtime.ino`。
2. 在 Arduino IDE 中选择 `ESP32S3 Dev Module`。
3. 安装 `U8g2` 库。当前固件曾在 U8g2 2.36.19、ESP32 Arduino Core 3.3.7 环境下编译验证。
4. 保留同目录下的全部 `.h` 文件，不要把历史 OLED 发布版中的模型头文件与当前固件混用。
5. 编译并上传；首次上电先检查串口自检和 MPU 初始化日志。

### 2. 接线

| 模块 | ESP32-S3 接口 | 说明 |
| --- | --- | --- |
| 压电模拟前端 | GPIO4 / ADC1_CH3 | 输入范围目标为 0--3.3 V |
| MPU（MPU-6050 外形，实测为 MPU-6500 兼容芯片） | SDA=GPIO17，SCL=GPIO15 | 3.3 V，地址 0x68 |
| SSD1306 128x64 OLED | SDA=GPIO8，SCL=GPIO9 | 使用 `Wire1`，自动探测 0x3C/0x3D |
| 电磁撞针 | 不连接 ESP32 功率回路 | 使用独立电源与控制器 |

改线前断电。完整引脚、电源和模拟前端说明见 [`docs/hardware_pinout.md`](docs/hardware_pinout.md) 与 [`docs/tonight_bringup_wiring.md`](docs/tonight_bringup_wiring.md)。

### 3. 运行测量

串口设为 `460800` 波特率：

1. 等待 `#MODEL_SELFTEST,PASS` 和 MPU 初始化完成。
2. 发送 `ARM_MEASUREMENT` 并换行。
3. 每次出现 `STRIKE NOW` 时触发外部撞针；完成三次有效敲击后读取分类结果。
4. 使用 `ABORT` 取消当前测量，使用 `RECALIBRATE` 重新静置校准，使用 `STATUS` 查看状态。

物理 ARM 按钮当前未启用；外部撞针由独立控制器负责，不能直接由 ESP32 供电。

## 模型与验证

推荐模型为 [`models/20260913/decision_tree_full.json`](models/20260913/decision_tree_full.json)。它是最大深度 4、最小叶节点样本数 3 的 CART 风格决策树。

在 2026-09-11 数据批次上，按 `measurement_id` 分组的 5 折 OOF 结果为：

| 指标 | 结果 | 口径 |
| --- | ---: | --- |
| 事件数 | 289 | 完整清洗数据集 |
| 测量组数 | 114 | 同一测量的敲击不跨折 |
| Accuracy | 87.89% | 分组 OOF |
| Macro F1 | 86.83% | 分组 OOF |

端侧移植验证还包括：

- 289 条波形的 Python/C++ 特征与分类对照，分类差异为 0；
- 209 条无时间缺口记录的固件 EventBuffer 适配对照，分类差异为 0；
- ESP32-S3 目标编译与内置启动自检通过。

289 条训练波形回放的命中率为 94.46%，这是训练数据回放结果，不是独立测试集准确率。项目已完成实物现场验收；README 只保留可复现的模型与端侧验证口径，具体现场记录以项目验收材料为准。

模型训练和验证说明见 [`models/20260913/README.md`](models/20260913/README.md) 与 [`docs/model_integration_20260915.md`](docs/model_integration_20260915.md)。原始波形数据未提交到仓库。

## 仓库结构

```text
tinggu/
├── firmware/
│   ├── tinggu_edge_runtime/       # 当前 ESP32-S3 端侧程序
│   ├── tinggu_signal_validator/   # 单次敲击与信号链验证固件
│   └── releases/                  # 2026-09-14 OLED 历史发布版
├── models/20260903/               # 历史 LDA/候选模型
├── models/20260913/               # 当前 14 维决策树及验证产物
├── scripts/                       # 特征、训练、验证和导出脚本
├── tools/TingguSignalWorkbench/   # Windows 串口采集与分析工具
├── docs/                          # 需求、数据契约、接线和验证记录
└── README.md
```

## 从数据复现模型

训练脚本需要本地原始波形数据以及 `numpy`、`pandas`、`scikit-learn`、`matplotlib` 等依赖。数据集不随仓库分发，因此以下命令需要将路径替换为本地数据位置：

```powershell
python models/20260913/train_waveform_14features.py `
  --dataset D:\path\to\training_dataset_20260911 `
  --outdir .\model_output
```

若只需复核已提交模型与固件实现的一致性，可参考 [`docs/model_integration_20260915.md`](docs/model_integration_20260915.md) 中的验证命令；该流程不会自动获取或上传数据。

## 相关文档

- [当前固件上传、接线与运行说明](firmware/tinggu_edge_runtime/README.md)
- [硬件引脚与实物接口](docs/hardware_pinout.md)
- [数据契约与 14 维特征接口](docs/data_contract.md)
- [模型训练与分组验证](docs/model_training_20260913.md)
- [模型接入与端侧验证](docs/model_integration_20260915.md)
- [历史 OLED 发布版](firmware/releases/oled_ssd1306_20260914/README.md)

## 许可证

当前仓库尚未添加开源许可证。除非另有书面授权，仓库内容仍按著作权法保留全部权利；如需复用代码或模型，请先联系仓库作者。
