# 14维波形树正式接入（2026-09-15）

## 来源与纠正

拉取提交4859fa6f6dcd922428eebf71da07880164fafb04。最新增加的是原始`models/20260913/train_waveform_14features.py`；模型JSON参数本次没有变动。这份脚本的完整波形、100Hz分频及自定义double Tree与旧导出模型匹配，先前0–300ms、250Hz、sklearn脚本不能用于本模型。

全部289条训练波形回放中，23个节点样本数及多数类别均匹配。分类命中273/289=94.46%；这是训练波形回放，不是独立测试准确率，也没有重做交叉验证。

## 正式程序

`firmware/tinggu_edge_runtime`现使用`tree_model_parameters.h`及`waveform_features.h`；保留OLED Wire1 SDA8/SCL9、MPU17/15、压电4。旧LDA的`model_parameters.h`不再被引用，旧OLED发布包作为回退版本保留。

- 仅支持2kHz压电/1kHz MPU，14维double，波形完整事件长1400–2600行。
- 原始ADC先取负时间样本中位数，得到与工作台CSV一致的piezo_delta；MPU采用g/dps单位、原始模长和全事件统计。
- 对齐V16确认触发（至少3活动样本及幅值总和要求）、最小阈值20、最短采集600ms、最长1200ms；50ms双路安静可提前结束。MPU6500加速度滤波寄存器设置为0x01，与训练采集版一致。
- DMA和独立MPU任务仍属于端侧实现，时间调度与旧逐tick采集不同。用过去或同一时刻的MPU样本判断安静，不使用未来样本。DMA溢出仍判无效；模型禁止更换采样率后沿用。
- 从一致数据回放恢复叶节点各类样本数，逐节点验证n和argmax；未改树结构/阈值/类别，没有重新训练。保留三次有效A档概率均值和0.50不确定门限。
- 串口新增14维#MODEL_FEATURES。上电用内置真实波形做特征/树路径自检，只有PASS才能开始测量；校验失败输出FAULT。模型概率为叶频率，非校准的实际准确率。
- 处理任务在发布结果前重新核对measurement_id，并锁住状态，防止计算期间ABORT后发布旧结果。

## 验证

1. 289条原始CSV通过生产方Python/C++提取器14维数值对照，最大绝对误差约1.8e-11，原始树分类零差异。
2. 209条500us时间连续记录通过固件EventBuffer适配器对照，含前触发中位数、ADC原始值、MPU原始计数转单位，分类零差异。另80条有时间缺口，仅完成CSV提取器对照，不宣称DMA可复现它们的缺口。
3. 主机通过内置真实波形启动自检、非有限/空输入、三次融合多数/平票/无效输入检查。
4. 树遍历2087组边界/随机/非法输入测试覆盖12叶路径。
5. 使用ESP32-S3目标编译，最终结果见交付包build.log。未上传硬件，没有宣称实物验收已完成。

现场首轮应确认#MODEL_SELFTEST,PASS，MPU正常、三次计数正确、OLED与串口结果一致，并在当前装置三档各试几组。旧模型与新模型、不同截窗的数据不要直接混用作同一批模型评估。

## 复现工具

`scripts/verify_waveform_tree.py`读取原始脚本函数并运行C++对照，仅读原始数据、不训练。`generate_waveform_tree_header.py`读取匹配的producer_features.csv，核对节点分布与多数类别后生成参数。测试C++在`scripts/tests/`。


在仓库根目录复现（需原始数据、NumPy/Pandas/Pillow及支持C++11的g++）：

```powershell
python scripts/verify_waveform_tree.py --dataset /path/to/training_dataset_20260911 --cxx /path/to/g++
python scripts/generate_waveform_tree_header.py --features tmp/tree_verification/producer_features.csv
```

验证脚本直接导入 `models/20260913/train_waveform_14features.py`，不会训练或改写原始测量。旧 `model_parameters.h` 是历史LDA参数，当前程序不引用它。启动自检波形已经嵌入 `model_selftest.h`，无需现场提供CSV。
