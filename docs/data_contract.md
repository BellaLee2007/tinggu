# 数据契约（初版）

原始数据建议“一次敲击一个文件”，另用 `manifest.csv` 管理标签和实验条件。字段名称一旦用于训练，不要随意改名。

## 原始采样字段

| 字段 | 含义 |
|---|---|
| `timestamp_us` | ESP32 采样时间戳（微秒） |
| `adc_raw` | 压电调理电路后的 ADC 原始值 |
| `imu_x`, `imu_y`, `imu_z` | IMU 三轴值；没有 IMU 时留空 |
| `sample_index` | 当前采样点序号 |

## manifest 字段

| 字段 | 含义 |
|---|---|
| `file` | 原始文件相对路径 |
| `state_label` | `firm`, `slight`, `loose`, `invalid` |
| `session_id` | 一次连续实验批次 |
| `setup_id` | 试验板、传感器位置和安装方式组合 |
| `trial_id` | 批次内的敲击编号 |
| `sampling_rate_hz` | 实际采样率 |
| `bolt_setting` | 圈数、扭矩或文字说明 |
| `hammer_setting` | 弹簧形变量、舵机动作等 |
| `notes` | 削顶、误击、外部扰动等备注 |

## 数据划分规则

训练集、验证集和测试集按 `session_id` 或实验批次划分，不按同一段波形随机切窗。否则会把同一次敲击的相邻窗口泄漏到测试集，得到虚高准确率。
