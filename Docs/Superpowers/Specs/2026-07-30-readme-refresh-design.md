# AvidScript GitHub README 美化设计

## 目标

将仓库根目录 `README.md` 从 Phase 53 时期的开发记录式说明，升级为面向首次访问者、
潜在使用者和贡献者的项目首页。页面必须同时做到：

- 第一屏能识别项目名称、技术路线、成熟度和许可证；
- 三分钟内理解 AvidScript 解决的问题、当前能做什么、怎样运行 C# 游戏逻辑；
- 用可复核的 Phase 56 正式证据说明性能，不挑选数据制造全面领先印象；
- 展示 Runtime、Bindings、VM、Editor 和 C# Guest 的边界；
- 在 GitHub 深色与浅色主题下保持清晰，不依赖 JavaScript 或外部图表服务。

## 受众

1. 正在比较 Puerts、UnLua、AngelScript 与 C# WASM 方案的 UE 开发者；
2. 希望用 C# 编写 `BeginPlay`、`Tick` 和 UE 事件逻辑的 Demo 开发者；
3. 需要判断项目架构、性能证据和贡献入口的引擎程序员；
4. 关注 PC 现状与移动端规划的技术决策者。

## 页面结构

README 按以下顺序组织：

1. 居中的项目标题、中文定位语和技术徽章；
2. 开发者预览状态提示；
3. “为什么是 AvidScript”：生成式 Binding、WASM 沙箱、生命周期闭环和性能证据；
4. 一个短小但真实的 C# 游戏脚本；
5. 当前能力矩阵；
6. Phase 56 正式性能对比；
7. 整体架构与 UE 调用路径；
8. 环境要求与快速开始；
9. 示例索引；
10. 当前边界、路线图、验证与许可证。

第一屏不使用大型营销横幅。项目仍处于开发者预览阶段，视觉层级应专业、克制，
避免让成熟度看起来高于实际状态。

## 标题与徽章

标题使用 GitHub Markdown/HTML 居中排版。标题下方放置 Shields.io 徽章：

- Unreal Engine 5.8 Source；
- C# Guest；
- WebAssembly；
- Wasmtime 45；
- Win64 Development；
- MIT License；
- Phase 56。

徽章只表达稳定事实，不显示未经自动化维护的虚假 coverage、release 或 download
数字。徽章使用固定文案，不依赖 GitHub Actions 状态。

## 视觉资产

### 性能图

新增 `Docs/Assets/README/phase56-gameplay-performance.svg`，内容为三组横向对比：

- Small gameplay：AvidScript Generated S1 与 Puerts static；
- Dense gameplay：AvidScript Data-Oriented 与 Puerts static；
- Lifecycle callback：AvidScript 与 Puerts 对应路径。

每组以 Puerts 为 `1.00x`，AvidScript 分别为 `0.469x`、`0.513x` 和 `0.391x`。
图中必须标明“越低越好”，并同时显示绝对纳秒值或在紧邻表格中提供绝对值。

SVG 使用透明背景、浅深主题兼容的 CSS、自带 `<title>` 与 `<desc>`，不加载字体、
脚本或外部资源。图表只展示正式 evidence 中存在的数据。

### Mermaid 图

README 内保留两张 Mermaid 图：

1. 架构图：C# -> Roslyn/Guest IR -> WASM -> Wasmtime/WAMR -> Runtime -> UE；
2. 调用路径图：Generated S1 快路径与 semantic reflection fallback 的分流，
   显示 descriptor/profile、prepared plan 和 `ProcessEvent` 的位置。

Mermaid 节点不使用实验性主题指令，确保 GitHub 默认渲染器兼容。

## 内容更新

现有 README 中以下内容必须修正：

- PC 主后端从 WAMR 更新为 Wasmtime 45 Cranelift；
- WAMR 描述为兼容后端和移动端候选，不再作为当前 Win64 主线；
- benchmark 从 Phase 53 更新为 Phase 56；
- 增加 callback-epoch fused host cell、prepared export 和 hot lifecycle result；
- 性能结论更新为 12/18 门禁通过；
- 明确 semantic reflection 当前为 Puerts reflection 的 `5.819x`；
- 保留 C# 子集、类型覆盖、Cook/Shipping 和移动端尚未完成的边界；
- 不宣称“全面领先 Puerts/V8”。

## 性能口径

性能段必须写明：

- 被测候选 commit 为 `d82ed7aa997758fa7f6983c6a6996999a467d283`；
- 5 个独立进程，每进程 5 次 warmup、30 次 timed sample；
- lower is better；
- Small Generated S1 P50 `65.72 ns/op`，Puerts static `139.996 ns/op`；
- Dense Data-Oriented P50 `121.88 ns/op`，Puerts static `237.638 ns/op`；
- lifecycle callback P50 `67.88 ns`，相对 Puerts 为 `0.391x`；
- Wasmtime/V8 P50 几何均值 `0.977x`，P95 `0.996x`；
- Generated scalar `27.08 ns`，property `54.31 ns`；
- typed empty crossing `4.08 ns`；
- 18 项门禁通过 12 项，失败项链接到 Phase 56 报告。

README 中所有舍入值必须能从
`Docs/Phase56/P56.5_Fused_Call_Frame_Benchmark_Evidence.json` 反推。

## 快速开始

快速开始只描述当前可执行的 Win64 开发路径：

1. 安装并验证锁定的 Wasmtime 45 依赖；
2. 将插件放入 UE 项目的 `Plugins/AvidScript`；
3. 使用 UE5.8 源码版构建 Editor Target；
4. 启用插件；
5. 从 C# 样例和 binding profile 开始生成 Guest。

命令使用通用示例项目名和路径，不写入本机用户名、绝对工作区或私有信息。
WAMR 构建命令移到兼容后端说明，不作为默认步骤。

## 链接与导航

README 使用仓库相对链接，至少连接：

- C# TypedProjectApi、DynamicProjectile、PlayablePickup；
- Phase 56 中文实现报告；
- Phase 56 机器可读证据；
- Gate 摘要；
- `AGENTS.md`；
- MIT `LICENSE`。

不得出现不存在的路线图、贡献指南或发布包链接。

## 验收标准

1. `README.md` 不再把 WAMR 写成 Win64 主后端，也不再引用 Phase 53 作为最新性能；
2. SVG 为有效 XML，不含脚本、外部资源或个人路径；
3. 所有 README 仓库相对链接存在；
4. Mermaid 块使用 GitHub 支持的 `flowchart` 语法；
5. 性能数据与 Phase 56 紧凑 evidence 一致；
6. `git diff --check` 对 README、SVG 和本设计范围通过；
7. 原有三项受保护用户文件不修改、不暂存、不提交；
8. 最终提交只包含 README、视觉资产和本次规格/计划文档。

## 非目标

- 本次不设计独立品牌 Logo；
- 不制作网站、宣传视频或动态 benchmark 徽章；
- 不改运行时代码、样例代码或 Phase 56 原始证据；
- 不把尚未完成的 Cook、Shipping、Android 或 iOS 写成已支持；
- 不启动新的正式性能矩阵。
