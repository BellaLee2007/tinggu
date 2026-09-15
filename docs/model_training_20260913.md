> **2026-09-15 正式接入更正：** 当前 `decision_tree_full.json` 的特征与树实现以 `models/20260913/train_waveform_14features.py` 为准（完整事件、100Hz分频、自定义 double Tree）。旧 `scripts/extract_features_20260913.py` 和 `scripts/train_decision_tree_20260913.py` 与该模型不匹配，不可用于生成部署特征或复现该模型。以下旧脚本说明仅保留作历史记录。部署验证见仓库 `docs/model_integration_20260915.md`；本次未重新训练或重做 OOF 评估。

# 2026-09-13 决策树三分类模型

这组模型使用 2026-09-11 批次的事件特征表，分别训练完整清洗数据集和零异常、零丢帧严格子集。原始 CSV 不随仓库提交，默认本地路径为 `data/processed/training_dataset_20260911.csv` 和 `data/processed/training_dataset_20260911_perfect_strict.csv`。

## 已提交内容

- `scripts/extract_features_20260913.py`：把工作台导出的逐事件波形转换为固定顺序的 14 项特征。
- `scripts/train_decision_tree_20260913.py`：分组验证、最终拟合、模型导出和混淆矩阵生成。
- `models/20260913/decision_tree_full.json`：完整清洗数据集模型。
- `models/20260913/decision_tree_perfect_strict.json`：严格子集模型。
- `models/20260913/results_full.json` 和 `results_perfect_strict.json`：逐折与总体指标。
- `models/20260913/confusion_matrix_full.png` 和 `confusion_matrix_perfect_strict.png`：分组 OOF 混淆矩阵。
- `models/20260913/summary.json`：两组结果的机器可读汇总。

## 训练协议

两组模型均使用最大深度 4、最小叶节点样本数 3 的 CART 决策树。验证使用 5 折 `StratifiedGroupKFold`，按 `measurement_id` 分组，确保同一次测量的多次敲击不会跨越训练集和验证集。固定随机种子为 `20260913`。

分类输入严格限定为模型 JSON 中列出的 14 项信号特征，不使用文件名、时间戳或质量等级作为分类特征。

## 波形到 14 项特征

特征脚本以 `time_us=0` 为触发点，使用触发后 0–300 ms。压电基线来自触发前样本；0–100 ms 和 100–300 ms 分别计算 RMS。频域特征对 0–300 ms 压电信号去均值、加 Hann 窗后计算功率谱；主频和谱质心忽略 5 Hz 以下频率，低高频能量分界为 250 Hz。衰减时间常数对峰值之后的 5 ms RMS 包络作对数线性拟合。

MPU 特征只使用 `mpu_valid=1` 的触发后样本，分别计算三轴加速度模长和三轴角速度模长的均值及总体标准差；`mpu_valid_ratio` 是有效 MPU 行数除以触发后总行数。

manifest 至少包含 `measurement_id`、标签列（`label`、`tightness_label` 或 `state_label`）和波形路径列（`file` 或 `sample_file`）。旧契约的 `firm/slight/loose` 会显式映射为模型使用的 `tight/medium/loose`。严格子集由 `perfect_strict` 标记，或由 `anomaly_count=0` 且 `drop_rate_percent=0` 生成。没有 MPU 列的波形，其四项 MPU 统计量和有效率均输出 0。

## 本地复现

在仓库根目录安装 `numpy pandas scikit-learn matplotlib` 后运行：

```powershell
$env:TINGGU_ROOT = (Get-Location).Path
python scripts/extract_features_20260913.py
python scripts/train_decision_tree_20260913.py
```

如特征 CSV 不在默认位置，可显式指定：

```powershell
python scripts/train_decision_tree_20260913.py `
  --full-csv D:\path\to\training_dataset_20260911.csv `
  --perfect-strict-csv D:\path\to\training_dataset_20260911_perfect_strict.csv
```

脚本会覆盖 `models/20260913` 中同名 JSON 和 PNG，因此复现前建议先通过 Git 检查当前基线。数据集变化时指标随之变化属于预期行为。
