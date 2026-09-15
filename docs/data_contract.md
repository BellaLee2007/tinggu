# 数据契约

## 单次原始波形

每次敲击保存一个CSV。压电保持实际采样率时间轴，MPU只有真实读取行写值，不插值。

```csv
event_id,label,time_us,piezo_raw,piezo_delta,mpu_valid,ax_g,ay_g,az_g,gx_dps,gy_dps,gz_dps
```

## manifest.csv

```csv
measurement_id,strike_index,session_id,setup_id,trial_id,state_label,specimen_id,bolt_setting,striker_setting,strike_method,sampling_rate_hz,notes,file,quality
```

- 同一次测量的三次敲击共享`measurement_id`，`strike_index`为1、2、3。
- 更换传感器位置、撞针位置、固定方式或试验板后必须更换`setup_id`。
- 训练集、验证集和测试集按`session_id`或`setup_id`整体划分，禁止随机拆分同一批相邻敲击。
- `state_label`在训练前统一为`firm/slight/loose/invalid`；尚未确认的样本留空，不能猜标签。

## 当前端侧模型接口（2026-09-15）

正式模型：`waveform_tree14_20260913_verified_20260915`，原始参数为`decision_tree_full.json`。算法唯一来源是提交4859fa6的`models/20260913/train_waveform_14features.py`，不要使用之前不配套的`extract_features_20260913.py`。

固定14维顺序：

```text
peak_abs, peak_time_ms, rms_0_100ms, rms_100_300ms,
dominant_freq_hz, spectral_centroid_hz, low_high_energy_ratio,
decay_tau_ms, piezo_std, accel_mag_mean, accel_mag_std,
gyro_mag_mean, gyro_mag_std, mpu_valid_ratio
```

- 2kHz压电/1kHz MPU。使用完整变长事件，不再用旧240ms LDA特征，也不能只取0–300ms。V16事件约1400–2600行，含确认触发的约195–197行前触发数据。
- `piezo_delta`使用前触发原始ADC中位数去基线，与C#导出一致。峰值、峰时间使用全部非负时间；RMS分别为[0,100)、[100,300)ms。
- FFT：全部触发后压电去均值，长度N的对称Hann窗，补零至不小于max(256,N)的2次幂，最多4096点。频域5–1000Hz；主频取幅值最大bin，质心按幅值加权；能量比为[5,100)Hz平方幅值之和除以[100,1000]Hz平方幅值之和+1e-12。
- 衰减：绝对峰后第4个采样点起，最多500点；abs+1e-6，保留大于max(峰包络×0.02,1e-5)的点，按0.5ms间隔拟合log包络；至少10个保留点且斜率<−1e-5时取−1/斜率，否则9999ms。
- `piezo_std`用全部事件行；四个MPU统计量用整个事件中有效的三轴模长，不去重力/去偏置，标准差为总体标准差。有效率分母是事件所有压电行，正常值约0.5，不是1。
- 自定义Tree使用double输入与double阈值，`<=`走左，不经过Scaler或float32转换。类别0/1/2=TIGHT/MEDIUM/LOOSE。
- 23个节点的训练样本数和多数类别均已通过289条回放核对，由匹配数据恢复叶节点各类频率。保留原三次有效A档概率平均及0.50置信度门限，不足输出UNCERTAIN；频率不是校准后的准确率。

历史串口的`f1_hz,tau_ms,e_ratio,consistency_c`四项仍是诊断指标，不作为新树的四维输入。原始CSV格式不变；新增`#MODEL_FEATURES`日志按上述14维顺序输出便于现场对照。

所有模型变更必须同步特征、生成参数与golden验证；训练/验证拆分仍遵循前述session_id/setup_id规则。本次只移植现有模型，没有重新训练或重新划分数据。
