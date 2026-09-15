# 听固：新14维决策树 + OLED（2026-09-15）

本目录是完整ESP32-S3 Arduino测量程序。已接入`decision_tree_full.json`，模型标识`waveform_tree14_20260913_verified_20260915`。正确特征脚本为GitHub提交4859fa6的`models/20260913/train_waveform_14features.py`。

## 上传与接线

Arduino选择 **ESP32S3 Dev Module**，安装 **U8g2**（已验证2.36.19，ESP32板包3.3.7）。打开本目录同名ino并保留配套头文件；不要混用旧OLED包内的模型文件。

| 模块 | 接线 |
|---|---|
| 压电前端输出 | GPIO4 |
| MPU | VCC=3V3、GND=GND、SDA=17、SCL=15 |
| SSD1306 128×64 I²C OLED | VCC=3V3、GND=GND、SDA=8、SCL=9 |
| 电磁撞针 | 独立电源及控制器，不接ESP32功率回路 |

断电改线；以模块丝印为准。OLED自动探测0x3C/0x3D；未接屏时串口测量仍可运行。

## 开机与测量

1. 串口460800，检查`#MODEL_SELFTEST,PASS`、`#MODEL_CONTRACT,version=waveform_tree14_20260913_verified_20260915`及MPU找到日志。自检失败禁止开始测量。
2. 静置完成校准，发送`ARM_MEASUREMENT`并换行。
3. 每次看到STRIKE NOW再按外部撞针；收满三次有效A档后输出TIGHT/MEDIUM/LOOSE或UNCERTAIN，并显示置信度。无效尝试重试，不计入三次。
4. `ABORT`取消，`RECALIBRATE`重新静置标定，`STATUS`查看状态。物理ARM按钮仍未启用。

新增`#MODEL_FEATURES,id=...,attempt=...,<14个数值>`用于现场特征核对；已有结果与预览串口格式保留。OLED保持最终结果至下一次测量。

## 实现与限制

采集仍在Core0、计算和显示在Core1。模型只支持2kHz压电采样；事件采用V16式确认触发及1400–2600行变长窗口。特征使用整段波形，100Hz频带分界及2048/4096点补零FFT；模型阈值与输入均为double。详细契约见项目`docs/data_contract.md`。

新模型叶计数由完全匹配训练波形恢复；三次概率平均，最大值低于0.50输出UNCERTAIN。旧`model_parameters.h`仅为历史LDA文件，当前代码不引用它。

289条原始波形的14维特征和分类均通过Python/C++对照，209条无时间缺口记录通过原始计数EventBuffer适配对照。其余带时间缺口记录仅完成CSV提取器对照，端侧DMA溢出仍判无效。

这些是算法与移植验证，不能替代新装置实测；289条训练数据回放94.46%也不是独立测试准确率。第一次上传后请三档各试几组，检查分类、MPU掉点、有效A档计数及OLED显示。
