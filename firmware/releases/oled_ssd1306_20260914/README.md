# SSD1306 OLED 完整发布版（2026-09-14）

ESP32-S3 + SSD1306 128×64 四针 I²C OLED。包含独立验屏程序和已接入显示的三次有效敲击测量程序。

## 接线

断电后按模块实际丝印接线。GPIO编号不是排针序号。

| OLED | ESP32-S3 |
|---|---|
| GND | GND |
| VCC | 3V3 |
| SDA | GPIO8 |
| SCL | GPIO9 |

OLED使用Wire1独立总线，自动探测0x3C/0x3D。MPU保持SDA=17、SCL=15；压电模拟输出保持GPIO4。电磁撞针独立供电、独立控制，不连接ESP32功率回路。

## Arduino上传

1. 安装Arduino ESP32开发板包，以及库管理器中的 **U8g2**（验证版本2.36.19）。
2. 选择 **ESP32S3 Dev Module** 和实际COM端口。
3. 先打开 [tinggu_oled_test/tinggu_oled_test.ino](tinggu_oled_test/tinggu_oled_test.ino) 上传。保留同目录display_interface.h。
4. 串口监视器设置 **460800**。出现`#OLED,READY`后应能看到轮换显示；`DEMO ONLY`仅为演示，不是预测。
5. 然后打开 [tinggu_edge_runtime/tinggu_edge_runtime.ino](tinggu_edge_runtime/tinggu_edge_runtime.ino) 上传，必须保留本目录全部h文件。
6. 上电静置完成校准，发送`ARM_MEASUREMENT`并换行；看到`STRIKE NOW`再敲。累计三次有效A档后显示分类及置信度。

支持`ABORT`中止、`RECALIBRATE`重新标定、`STATUS`查看状态。无效敲击不计数，会重试当前序号。

## 屏幕与故障处理

- KEEP STILL：校准；READY：等待启动；PREPARE：准备；STRIKE NOW：可以敲击。
- Valid hits n/3：有效次数；最终显示TIGHT/MEDIUM/LOOSE或UNCERTAIN。
- 结果保留到下一次测量、取消或重新校准。置信度不是模型准确率。
- NOT_FOUND：断电检查VCC/GND、SDA/SCL、实际GPIO引出；重新上电。屏幕未连接时采集仍可运行。
- 掉线后停用显示，接好后重启。只有Arduino loop执行OLED刷新，采集任务不直接访问显示总线。

## 版本与验证

这是独立、完整的OLED发布快照，正式使用原部署的`spectral_lda40_20260903`，尚未切换20260913新树模型。仓库其他固件目录有各自版本，请直接打开本发布目录内的ino与配套头文件。

包含本地已有的三次有效A档累积及串口波形预览逻辑。该版本已通过ESP32-S3编译：正式版395892字节程序/64992字节全局RAM，验屏版351604/24712字节。显示状态模拟检查通过；尚未进行本次实物OLED联调，需现场验屏与合盖后检查。

[详细接线与上传说明](接线与上传说明.md) · [硬件引脚](hardware_pinout.md) · [上电检查](tonight_bringup_wiring.md) · [验证记录](验证记录.md)
