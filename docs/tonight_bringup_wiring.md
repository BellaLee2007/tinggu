# 今晚接线与上电检查表

当前接口于2026-09-14更新为ESP32-S3：压电GPIO4，MPU SDA17/SCL15，OLED SDA8/SCL9。旧经典ESP32的GPIO34/21/22不再用于本固件。

## 安全边界

- 改动任何导线前必须拔掉ESP32 USB。
- TLV9062和MPU都使用3.3 V；所有模块共用GND。本模拟前端不再使用5 V轨。
- 电磁撞针不接ESP32，本次只测试传感器采集。
- 没有万用表时必须分阶段上电；任何阶段出现0、4095、发热、重启或异味，立即断电。

## 固定节点

| 节点 | 所有连接 |
|---|---|
| `3V3` | ESP32 3V3、MPU VCC、OLED VCC、TLV9062 8脚、BAT54S孔2、基准上侧10 kΩ |
| `GND` | ESP32 GND、MPU GND、OLED GND、TLV9062 4脚、BAT54S孔1、压电铜片侧、基准下侧10 kΩ |
| `VREF_RAW` | 两只10 kΩ中点、TLV9062 5脚、1～10 µF正端；电容负端接GND |
| `VREF_BUF` | TLV9062 7脚、TLV9062 6脚、输入偏置1 MΩ的一端 |
| `SIG_IN` | 压电经100 kΩ后的节点、BAT54S孔3、1 MΩ另一端、TLV9062 3脚 |
| `OP_OUT` | TLV9062 1脚、TLV9062 2脚、输出串联2.2 kΩ的一端 |
| `ADC_NODE` | 输出串联2.2 kΩ另一端、ESP32 GPIO4；不要再接4.7 kΩ到GND |

TLV9062采用标准双运放8脚定义：1=OUT1、2=IN1-、3=IN1+、4=GND、5=IN2+、6=IN2-、7=OUT2、8=3.3 V。按转接板标出的1～8脚接线；芯片顶面文字方向不能代替1脚标记。
在8脚与4脚之间就近接一只100 nF（104）电容。

BAT54S转接板底部孔从左到右标为`2、3、1`：孔2接3.3 V、孔3接`SIG_IN`、孔1接GND。

MPU连接固定为：VCC接3.3 V、GND接GND、SDA接GPIO17、SCL接GPIO15；其他脚不接。

## 上电顺序

### OLED（新增独立总线）

- 断电后按模块丝印接线：GND→GND、VCC→3V3、SDA→GPIO8、SCL→GPIO9；不可按排针位置猜顺序。
- 使用SSD1306 128×64四针I²C模块，地址自动探测0x3C/0x3D，不接MPU的17/15总线。
- 先上传`firmware/tinggu_oled_test/tinggu_oled_test.ino`检查显示，再换回正式端侧固件。测试屏幕的DEMO ONLY不是模型结果。
- 串口460800出现`#OLED,READY`；NOT_FOUND时断电检查电源、SDA/SCL和板上GPIO丝印。接好后重启。
- 詳细说明见`docs/oled_setup.md`。

### A. MPU

- [ ] 压电红线保持断开。
- [ ] 上传`firmware/tinggu_signal_validator/tinggu_signal_validator.ino`。
- [ ] 串口460800，上电静置一秒。
- [ ] 出现`#MPU,FOUND,0x68`或`0x69`。

### B. 模拟前端静态基准

- [ ] 保持压电红线断开，重新上电。
- [ ] `baseline`应接近1.65 V对应值；当前ESP32实测约1870，不长期为0或4095。
- [ ] 静置`noise_rms`稳定，没有连续自动触发。

### C. 压电

- [ ] 断电后将压电中心侧经100 kΩ接`SIG_IN`，铜片侧接GND。
- [ ] 上电静置，发送`RECALIBRATE`。
- [ ] 先轻敲；ADC不长期为0或4095。

### D. 双传感器工作台

- [ ] 关闭Arduino串口监视器，打开`tools/TingguSignalWorkbench/TingguSignalWorkbench.exe`。
- [ ] 连接COM口，重新标定，先做一次“手动采集”。
- [ ] 点“下一次采集 / ARM”，只敲一次。
- [ ] 压电SNR≥10 dB、MPU SNR≥6 dB、丢点率<1%。
- [ ] `data/signal_validator`中生成事件CSV、`summary.csv`和`manifest.csv`。

### E. 双核固件

- [ ] 上传`firmware/tinggu_edge_runtime/tinggu_edge_runtime.ino`。
- [ ] 发送`STATUS`，确认`mpu=1`、采集核心为0、处理核心为1。
- [ ] 发送`ARM_MEASUREMENT`，5秒内完成三次敲击。
- [ ] 三次特征均输出，无DMA错误或死机；`MODEL_UNTRAINED`属于预期状态。

## 增益规则

默认将TLV9062 1脚与2脚直接连接，保持1倍缓冲。只有压电绝对峰值低于400～500 ADC计数、无饱和且接线固定后仍偏小时才改2倍：断电后取消1/2脚直连，1脚经10 kΩ接2脚，2脚再经10 kΩ接`VREF_BUF`。一旦出现0或4095，恢复1倍。
