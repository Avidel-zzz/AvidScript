# 性能策略

## 目标

- AvidScript 的目标是对游戏脚本关键路径形成可证明的领先，而非只在单项 microbenchmark 追平 Puerts 或 AngelScript。
- 分别度量纯计算、Host/Guest crossing、一般 UFunction、property、struct/container、BeginPlay/Tick/event 和批处理工作负载。

## 方法

- 冻结 workload、输入、构建配置、runtime mode、warmup、样本数、CPU 亲和和统计口径后再比较。
- 报告 median、p95、吞吐、分配和 crossing 次数；保留原始 JSON、Git tree、引擎/runtime 版本与硬件。
- Puerts/V8、AngelScript、WAMR、Wasmtime 的比较标注 JIT/AOT/interpreter、优化级别和语义差异。
- micro 数据用于定位，不单独证明真实 UE 游戏流程领先。

## 优化顺序

1. 消除热路径 reflection/name lookup，缓存 prepared route 与 codec program。
2. 减少 Host/Guest crossing，支持 batch、fused call 和 direct property cell。
3. 消除临时分配、字符串转换、重复 handle 解析和不必要 memory copy。
4. 优化 Session 调度、event fan-out 和 Tick 批量执行。
5. 再评估 backend JIT/AOT、CPU feature 和 serialized artifact。

## 门禁

- correctness、identity 和安全检查不能为了 benchmark 被关闭。
- 性能阈值由受控 profile 与 evaluator owner 管理；不能在实现代码里散落 magic number。
- 回归必须能定位到 route counter 或成本分层，失败时保留样本，不用一次噪声结果修改阈值。
