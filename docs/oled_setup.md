# OLED接线与显示验收（2026-09-14）

适用：ESP32-S3，SSD1306，128×64，四针I²C OLED。使用Arduino库管理器安装 **U8g2**（本机2.36.19）。不适用于SH1106或SPI屏。

## 接线

拔掉USB及相关供电后，按OLED上实际丝印连接，不按四针的排列位置猜测。

| OLED丝印 | ESP32-S3 |
|---|---|
| GND | GND |
| VCC | 3V3 |
| SDA | GPIO8 |
| SCL | GPIO9 |

这是GPIO编号，不是排针第8/9脚；须确认开发板上对应引脚引出且空闲。OLED独占Wire1；MPU保持SDA17/SCL15、压电保持GPIO4。电磁撞针继续使用独立控制器，不接ESP32功率回路。不要让屏幕线牵拉钢片和传感器。

## 先验屏，再运行测量

1. 打开完整的`tinggu_oled_test`文件夹内同名ino，保留同目录的display_interface.h。选择ESP32S3 Dev Module和当前COM端口，上传。
2. 串口460800查看`#OLED,READY,SDA=8,SCL=9,address=0x3C`（或0x3D）。屏幕每2.5秒轮换READY、PREPARE、STRIKE NOW、ANALYZING、DEMO ONLY。演示不采传感器、不做预测。
3. 出现NOT_FOUND时，断电核对VCC/GND及SDA/SCL，确认GPIO8/9可用；重新上电。地址自动探测，不需修改代码。程序不会因缺屏停止运行。
4. 验屏后打开`tinggu_edge_runtime`的ino，**保留文件夹内全部h文件**，上传正式程序。上电保持钢片静止，等校准完成和READY。
5. 通过已有串口工具发送`ARM_MEASUREMENT`并换行；等待STRIKE NOW再敲。收满三次有效A档后显示结果。仍支持ABORT、RECALIBRATE、STATUS，串口460800。

## 屏幕含义

| 内容 | 含义 |
|---|---|
| KEEP STILL | 正在校准，保持静止 |
| READY | 已就绪，等待串口启动 |
| PREPARE | 准备下一击；尚未到敲击时机 |
| STRIKE NOW | 可以敲击 |
| CAPTURING / ANALYZING | 采集或分析中 |
| Valid hits: n/3 | 本次测量累计接受的有效敲击；无效敲击不会增加 |
| TIGHT / MEDIUM / LOOSE | 紧 / 中 / 松 |
| UNCERTAIN | 模型不确定，不能强行归入三档 |
| MODEL NOT READY | 未就绪的模型，不给出松紧判断 |
| INVALID / RETRY | 本次测量无效 |
| DEVICE FAULT | 查看串口故障信息 |

最终结果保持至下一次测量、取消或校准。Confidence是模型输出置信度，不是准确率。屏幕英文用于避免汉字字体占用及拥挤。

## 实现与边界

- 显示调用只更新短状态快照，临界区内不做I²C。实际显示仅在Arduino loop中执行；变化时至多每150ms刷新一次。短暂状态可能来不及显示，以串口记录为准。
- 缺屏不阻止采集；更新前探测发现掉线后打印DISCONNECTED并停用显示，重新接好后重启。I²C通信超时设置为10ms。
- 2026-09-15：当前firmware/tinggu_edge_runtime已切换14维波形树，详见model_integration_20260915.md。20260914的OLED历史发布包仍为旧LDA；请整套使用对应日期程序，不混用头文件。
- 编译通过不能替代接线实测：开盖验屏后合盖重测，检查屏幕稳定、MPU正常、三次敲击流程完整以及丢点/错误记录。不要在采集中插拔模块。
