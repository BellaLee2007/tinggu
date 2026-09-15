> 2026-09-15更新：已找到正确原始脚本，完成正式接入；以下为历史排查记录。当前结论见[新模型接入说明](model_integration_20260915.md)。

# 最新：特征脚本已收到，但与模型/波形不一致，暂不切换

2026-09-14检查提交ab53542新增的extract_features_20260913.py，按原函数计算本地全部289条清洗波形。节点数分布与导出JSON不符：根节点左/右记录124/165，实际33/256。回放命中116/289（40.14%，不是独立测试准确率），tight仅1/116。Python/C++对289条特征推断完全一致，double/float输入的分类也一致；模型文件与GitHub字节一致。

因此目前阻碍已从“缺少代码”变为“波形、特征提取脚本、模型版本不能复现一致”。需模型同学提供实际训练的特征表（带sample_file），与本次重算特征逐条对齐，确认配套版本。没有改变正式Arduino/OLED固件，没有重训、没有修改原始数据。

完整证据与复现说明：[一致性核查](../deliverables/新模型一致性核查_20260914/README.md)。

以下为之前的历史记录：

---

# 新决策树Arduino移植：训练脚本已核对，仍缺波形特征提取

## 最新检查（2026-09-14，提交92d65d56576fe031cf616ddf7d9278c8df74b259）

新增脚本`train_decision_tree_20260913.py`开头明确写明输入是feature tables，而非raw waveforms。load_dataset只读取已有14个特征列，train_dataset直接将这些列送入DecisionTreeClassifier；仓库及本地均未找到生成这些特征列的实现。data/processed和tests/golden_vectors仍没有可用验证数据。

已完成新增工作：
- 类别映射明确：0=TIGHT、1=MEDIUM、2=LOOSE；没有Scaler或Imputer，缺失值在训练加载时拒绝。
- 根据sklearn预测输入转float32的行为修正C++候选树的比较精度，阈值保留double。2087组测试覆盖12叶路径，与独立Python JSON遍历0处不一致；这不是原始波形端到端验证。
- 正式OLED测量版继续使用已部署LDA；候选新树仍保持PIPELINE_VERIFIED=false。
- 未重训、未覆盖正式模型。现有JSON仍缺叶节点各类计数/概率，不能直接产生旧界面所需的分类置信度。

参考：[sklearn DecisionTreeClassifier.predict输入转float32](https://scikit-learn.org/stable/modules/generated/sklearn.tree.DecisionTreeClassifier.html#sklearn.tree.DecisionTreeClassifier.predict)。

## 请模型同学补交的具体文件

请上传**生成`data/processed/training_dataset_20260911.csv`的脚本及其依赖**：必须能从原始事件CSV计算出该表的14项特征。这一步不是本次已上传的训练脚本。

同时给几条原始事件CSV和对应的14项特征行用于核对（最好每类至少一条）；若要保留OLED置信度显示，请同时导出叶节点各类别计数或predict_proba所需信息。无需重新采集，使用训练时已有文件即可。

原因：例如窗口起止、基线扣除、频带边界、衰减拟合、IMU去重力和有效率分母不同，同名特征会有不同数值。不能自行猜公式后套用已训练阈值。

---

以下为首次接入记录（缺项状态以上述最新检查为准）：

# 新决策树Arduino移植：等待特征定义

来源：用户指定仓库https://github.com/BellaLee2007/tinggu.git，提交cc23e55ebc4a8fff7ebe5819f9f3671c6fc0254b。

按仓库推荐采用models/20260913/decision_tree_full.json，14特征、23节点、12叶节点。源JSON存入本地models/20260913；未重新训练。原firmware/tinggu_edge_runtime的40特征LDA和采集固件均未覆盖。

已完成：generate_tree_header.py自动生成C++树表；阈值保留double，非有限输入或长度错误返回无效。test_tree_transport.py完成2087组Python/C++树遍历一致性检查，覆盖12条叶路径、阈值及相邻浮点边界和随机输入，0处不一致；另检查空指针和错误长度。当前按CART惯例“<=走左”，仍需模型作者预测脚本确认。这不是端到端golden-vector验证。

firmware/tinggu_tree_candidate/tinggu_tree_candidate.ino已通过ESP32-S3编译：程序292951字节，静态RAM22204字节。该文件只是串口输入14维FEATURES的树调试工具，不采样、不驱动撞针，默认MODEL_UNTRAINED，仅输出raw_class_id。不能用于正式测量，尚未烧录。原有固件可继续使用。

## 阻碍正式接入的缺项

仓库新目录只有树JSON与评估结果，scripts仍是旧LDA脚本，tests/golden_vectors只有.gitkeep。仓库与本地未找到这14特征的计算真值实现。

需要模型同学补交：

1. 本版特征提取代码：压电是原始ADC还是去基线值、去基线算法、各特征时间窗口及触发对齐、缺失数据处理。
2. low_high_energy_ratio的传感器来源、频带边界、FFT长度、窗函数、直流与零能量处理。
3. decay_tau_ms的包络与拟合方法；加速度/陀螺仪模长是否去重力/去偏置、时间范围和标准差定义。
4. 预测脚本：分支等号规则、缩放/缺失值填补、0/1/2类别映射。报告顺序暗示tight/medium/loose，但不能替代明确导出映射。
5. 若干原始CSV、对应14维特征与预期类别，作为端到端golden vectors。

建议直接上传训练这两个JSON所用的完整训练、特征提取与预测脚本，避免文字转述遗漏。

叶节点只有pred与总样本数n，没有各类比例，不能沿用旧LDA概率或虚构100%置信度。三敲融合也需要明确；未擅自用投票替换旧概率平均。

仓库87.89%是按measurement_id分组的OOF报告，不等同项目规定的按session_id/setup_id验证，也不是端侧实机准确率。本次仅移植，不重训、不更改评估划分。

补齐后将迁移14维提取器，通过Python/C++端到端验证，再切换正式edge_runtime模型并同步特征契约。不得把旧40维特征直接送入新14维树。
