# 2026-09-03 三分类模型

这组模型只使用 2026-09-03 批次：tight 106、medium 105、loose 101，共 312 个事件。原始 CSV 不随仓库提交；`outputs/advanced_recrop_20260903/advanced_recrop_features.csv` 是按 240 ms 固定窗口提取的特征缓存。

## 已提交内容

- `models/20260903/candidate_advanced_ensemble.joblib`：离线三模型概率融合模型。
- `models/20260903/candidate_spectral_lda.joblib`：线性紧凑候选模型。
- `models/20260903/candidate_spectral_lda40.joblib`：40 特征候选模型，适合后续 ESP32-S3 移植。
- `models/20260903/spectral_lda40_export.json`：固定特征顺序、缺失值填补、缩放参数和分类系数。
- `scripts/validate_advanced_recrop.py`：5 折分组验证。
- `scripts/fit_advanced_candidates.py`：用当前批次拟合并导出模型。
- `scripts/plot_new_batch_confusion.py`：生成混淆矩阵热力图。

## 当前验证结果

最佳配置为三模型 soft-vote、每 4 个相邻 measurement 分组：measurement 级准确率 83.44% ± 0.61%，macro-F1 83.11%；完整三敲子集准确率 81.46% ± 1.34%。medium/loose 仍是主要混淆对，结果需要后续独立 session 盲测确认。

## 本地复现

在仓库根目录安装 `numpy pandas scipy scikit-learn joblib matplotlib` 后运行：

```powershell
$env:TINGGU_ROOT = (Get-Location).Path
$env:PYTHONPATH = (Resolve-Path scripts).Path
python scripts/validate_advanced_recrop.py
python scripts/fit_advanced_candidates.py
python scripts/plot_new_batch_confusion.py
```

脚本默认读取 `outputs/advanced_recrop_20260903/advanced_recrop_features.csv`，不会读取文件名、时间戳或质量等级作为分类输入。
