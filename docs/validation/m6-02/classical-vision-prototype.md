# M6-02 传统视觉原型合同

## 状态

- 插件 ID：`classical-vision`
- 实现版本：`1.0.0-prototype`
- 模型版本：`none`
- 验收状态：原型；M6-00 仍为 `blocked`

本实现用于验证 M6-01 插件替换边界、结果语义和传统视觉处理链。仓库尚无冻结标注数据集、批准阈值、目标 ROI/像素格式、目标工控机资源预算或责任人审签，因此本文中的默认值不是生产批准值，不得据此宣称正式断纸算法已通过验收。

## 输入合同

- 仅接受完整的 `PixelFormat::mono8` 帧；Mono10、Mono12 和 Bayer 的打包/转换合同尚未批准，当前返回 `ALGORITHM_PROCESS_FAILED`。
- 相机 ID 必须与检测器配置一致，序号严格递增，单调时间不得回退。
- 输入 `FrameView` 以带 stride 的只读 OpenCV 视图包装，不复制原始整帧，也不修改帧内存。
- ROI 宽高同时为 0 表示整帧；否则宽高必须同时为正并落在当前帧内。
- 有效 ROI 最多 4,194,304 像素。背景、差分和二值掩码各最多占用一份 ROI 大小的 8 位工作区；超限配置或帧被拒绝。

## 参数

参数通过 `DetectorConfig.parameters` 提供。名称未知、类型不符或越界均返回 `SYS_CONFIG_INVALID`。

| 参数 | 类型 | 默认值 | 范围/语义 |
| --- | --- | ---: | --- |
| `roi_offset_x` | int64 | 0 | `[0, UINT32_MAX]` |
| `roi_offset_y` | int64 | 0 | `[0, UINT32_MAX]` |
| `roi_width` | int64 | 0 | 0 表示整帧；显式 ROI 必须大于 0 |
| `roi_height` | int64 | 0 | 0 表示整帧；显式 ROI 必须大于 0 |
| `paper_grayscale_threshold` | int64 | 128 | `[0, 255]`，像素大于等于该值计为纸幅 |
| `minimum_paper_ratio` | double | 0.75 | `[0, 1]`；低于该值触发纸幅缺失 |
| `maximum_mean_grayscale_change` | double | 0.20 | `(0, 1]`；相邻帧 ROI 归一化均值差阈值 |
| `maximum_background_change` | double | 0.15 | `(0, 1]`；相对背景的归一化平均绝对差阈值 |
| `background_pixel_change_threshold` | double | 0.10 | `(0, 1]`；计算背景变化面积时的单像素差阈值 |
| `background_learning_rate` | double | 0.02 | `[0, 1)`；仅在当前帧未异常时更新背景，0 表示固定背景 |
| `enable_paper_ratio` | bool | true | 是否启用纸幅占比触发 |
| `enable_mean_change` | bool | true | 是否启用相邻均值变化触发 |
| `enable_background_compare` | bool | true | 是否建立背景并启用背景比较触发 |

## 处理和结果语义

每帧计算 ROI 归一化均值、相邻均值变化、纸幅占比、背景平均变化和背景变化像素比例。首个满足纸幅占比条件的有效 ROI 建立背景；缺纸帧不会成为背景。非异常帧按 `background_learning_rate` 更新背景。初始化、reset 和成功热更新都会清除背景、上一均值、序号及时间状态。

同一帧满足多个条件时，主触发原因按以下顺序选择：

1. 纸幅占比不足：`RoiPaperRatio` / `paper_missing`；
2. 背景平均变化超限：`BackgroundChange` / `paper_break`；
3. 相邻均值变化超限：`MeanGrayscaleChange` / `paper_break`。

`confidence` 是未经数据集校准的归一化原型异常分数：纸幅缺失使用 `1 - paperRatio`，背景或均值变化分别使用对应归一化变化量。纸幅缺失的 `areaRatio` 为缺失比例，背景变化为超过单像素差阈值的像素比例，均值变化作为全 ROI 异常时为 1。`changeScore` 为均值变化和背景平均变化的较大值。

结果始终包含检测区域、纸幅占比、灰度/变化量、实现/模型版本以及以下有界调试指标：

- `meanGrayscale`
- `meanGrayscaleChange`
- `paperRatio`
- `backgroundMeanChange`
- `backgroundChangedRatio`
- `minimumPaperRatio`
- `maximumMeanGrayscaleChange`
- `maximumBackgroundChange`
- `backgroundPixelChangeThreshold`
- `backgroundLearningRate`
- `roiPixels`

实际 `processingTime` 由 M6-01 `DetectorHost` 在同步调用边界测量并写入结果。

## 已知限制

- 默认阈值和置信分数未在真实纸机数据上校准；
- 不包含连续帧确认、冷却、队列积压、连续故障报警或服务降级，这些属于 M6-03；
- 不包含 UI、生产配置选择或当前帧测试入口，这些属于 M6-04；
- 未执行实体相机、六路 360 frame/s 目标机性能或真实断纸准确性验证。
