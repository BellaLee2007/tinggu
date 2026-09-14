# 2026-09-13 决策树三分类模型

这组模型使用 2026-09-11 批次的事件特征表，分别训练完整清洗数据集和零异常、零丢帧严格子集。原始 CSV 不随仓库提交，默认本地路径为 `data/processed/training_dataset_20260911.csv` 和 `data/processed/training_dataset_20260911_perfect_strict.csv`。

## 已提交内容

- `scripts/train_decision_tree_20260913.py`：分组验证、最终拟合、模型导出和混淆矩阵生成。
- `models/20260913/decision_tree_full.json`：完整清洗数据集模型。
- `models/20260913/decision_tree_perfect_strict.json`：严格子集模型。
- `models/20260913/results_full.json` 和 `results_perfect_strict.json`：逐折与总体指标。
- `models/20260913/confusion_matrix_full.png` 和 `confusion_matrix_perfect_strict.png`：分组 OOF 混淆矩阵。
- `models/20260913/summary.json`：两组结果的机器可读汇总。

## 训练协议

两组模型均使用最大深度 4、最小叶节点样本数 3 的 CART 决策树。验证使用 5 折 `StratifiedGroupKFold`，按 `measurement_id` 分组，确保同一次测量的多次敲击不会跨越训练集和验证集。固定随机种子为 `20260913`。

分类输入严格限定为模型 JSON 中列出的 14 项信号特征，不使用文件名、时间戳或质量等级作为分类特征。

## 本地复现

在仓库根目录安装 `numpy pandas scikit-learn matplotlib` 后运行：

```powershell
$env:TINGGU_ROOT = (Get-Location).Path
python scripts/train_decision_tree_20260913.py
```

如特征 CSV 不在默认位置，可显式指定：

```powershell
python scripts/train_decision_tree_20260913.py `
  --full-csv D:\path\to\training_dataset_20260911.csv `
  --perfect-strict-csv D:\path\to\training_dataset_20260911_perfect_strict.csv
```

脚本会覆盖 `models/20260913` 中同名 JSON 和 PNG，因此复现前建议先通过 Git 检查当前基线。数据集变化时指标随之变化属于预期行为。
