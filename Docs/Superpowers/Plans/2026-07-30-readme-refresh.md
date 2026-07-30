# AvidScript GitHub README Refresh Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将仓库 README 升级为带技术徽章、正式性能图表、现代架构图和准确接入说明的 GitHub 项目首页。

**Architecture:** `README.md` 负责叙述、导航、表格和 Mermaid 图；一个独立 SVG
负责 Phase 56 性能可视化。所有性能数字只读取已经冻结的 Phase 56 紧凑 evidence，
不改 benchmark、运行时或样例代码。

**Tech Stack:** GitHub Flavored Markdown、内嵌 HTML、Shields.io、SVG 1.1、Mermaid
`flowchart`、PowerShell 7。

## Global Constraints

- 面向使用者的文字使用中文，技术名词和 API 保留英文。
- Win64 主后端写为 Wasmtime 45 Cranelift；WAMR 只写兼容回退和移动端候选。
- 性能协议固定为 5 个进程、每进程 5 次 warmup 和 30 次 timed sample。
- 不宣称全面领先；必须公开 12/18 门禁通过和 semantic reflection `5.819x` 缺口。
- 不写入本机用户名、绝对工作区、私有令牌或未发布能力。
- 不修改、暂存或提交三个受保护用户文件。
- 不增加 JavaScript、外部图表服务、动态 coverage 或虚假 release 徽章。

---

### Task 1: Phase 56 性能 SVG

**Files:**
- Create: `Docs/Assets/README/phase56-gameplay-performance.svg`
- Read: `Docs/Phase56/P56.5_Fused_Call_Frame_Benchmark_Evidence.json`

**Interfaces:**
- Consumes: Phase 56 的 `small_vs_best_puerts`、`dense_vs_best_puerts` 和
  `callback_vs_puerts` 正式比值。
- Produces: README 可通过相对路径嵌入、自带中性深色背景的 SVG。

- [x] **Step 1: 读取冻结 evidence 并核对三个比值**

Run:

```powershell
Get-Content Docs/Phase56/P56.5_Fused_Call_Frame_Benchmark_Evidence.json -Raw |
  ConvertFrom-Json |
  Select-Object -ExpandProperty headline_metrics |
  ConvertTo-Json -Depth 6
```

Expected:

- Small ratio `0.46941458441395845`
- Dense ratio `0.5128887643597646`
- Callback latency `67.88359375 ns`

- [x] **Step 2: 创建横向归一化性能图**

使用 `apply_patch` 创建 SVG，画布为 `960x430`，包含：

- 标题“Phase 56 游戏逻辑性能对比”；
- 副标题“相对 Puerts 最快对应路径，越低越好”；
- Small、Dense、Callback 三组；
- 每组灰色 Puerts `1.000x` 条和绿色 AvidScript 条；
- AvidScript 标签分别为 `0.469x`、`0.513x`、`0.391x`；
- footer 标注“5 processes x 30 timed samples | UE5.8 Win64 Development”；
- `<title>`、`<desc>` 和不依赖页面主题的稳定高对比度样式；
- 禁止 `<script>`、外链、嵌入字体和个人路径。

- [x] **Step 3: 验证 SVG 合法性和安全边界**

Run:

```powershell
[xml](Get-Content Docs/Assets/README/phase56-gameplay-performance.svg -Raw) |
  Select-Object -ExpandProperty svg |
  Select-Object width, height
```

Expected: `width=960`、`height=430`。

Run:

```powershell
rg -n "<script|(?:href|src)=[\"']https?://|[A-Z]:\\\\Users\\\\" Docs/Assets/README/phase56-gameplay-performance.svg
```

Expected: exit code `1`，除标准 SVG XML namespace 外不引用外部资源。

### Task 2: 重写 GitHub README

**Files:**
- Modify: `README.md`
- Read: `Docs/Superpowers/Specs/2026-07-30-readme-refresh-design.md`
- Read: `Docs/Phase56/P56.5_Fused_Call_Frame_Implementation_Report.md`
- Read: `Docs/Phase56/P56.5_Fused_Call_Frame_Benchmark_Evidence.json`

**Interfaces:**
- Consumes: Task 1 的 SVG 相对路径
  `Docs/Assets/README/phase56-gameplay-performance.svg`。
- Produces: GitHub 首页、C# 示例、性能表、架构图、快速开始和文档导航。

- [x] **Step 1: 记录当前 README 的过时断言**

Run:

```powershell
rg -n "主线使用.*WAMR|Phase 53|当前 WAMR interpreter" README.md
```

Expected: 至少匹配三处，证明 README 仍是旧状态。

- [x] **Step 2: 重写标题和项目定位**

使用 `apply_patch` 重写 `README.md`：

- 居中显示 `AvidScript` 和“面向 Unreal Engine 的现代 C# + WebAssembly 游戏脚本框架”；
- 放置 UE5.8、C#、WebAssembly、Wasmtime 45、Win64、Phase 56、MIT 七个徽章；
- 用醒目引用块写明 `0.1.0 开发者预览`、当前平台和未完成平台；
- 用四个平级要点说明 Generated Binding、WASM VM、UE 生命周期和证据驱动性能。

- [x] **Step 3: 加入真实 C# 闭环与能力矩阵**

保留可编译风格的 `BeginPlay`/`Tick` 示例，并将能力分为：

- C# 与生命周期；
- UE 类型与对象；
- 生成式 Binding；
- Runtime 安全与热重载；
- 工具链与诊断。

不得把 arbitrary `UStruct`、容器、delegate、Cook/Shipping 或移动端写成已完成。

- [x] **Step 4: 加入正式性能段**

嵌入：

```markdown
![Phase 56 游戏逻辑性能对比](Docs/Assets/README/phase56-gameplay-performance.svg)
```

紧邻图表提供绝对值与比值表，至少包含：

| 场景 | AvidScript | Puerts | 比值 |
| --- | ---: | ---: | ---: |
| Small gameplay | 65.72 ns/op | 139.996 ns/op | 0.469x |
| Dense gameplay | 121.88 ns/op | 237.638 ns/op | 0.513x |
| Lifecycle callback | 67.88 ns | 由正式 ratio 对比 | 0.391x |

再列出 Wasmtime/V8 P50 `0.977x`、P95 `0.996x`、typed empty `4.08 ns`、
scalar `27.08 ns`、property `54.31 ns`、门禁 `12/18` 和 semantic
reflection `5.819x`。文字明确“组合游戏逻辑热路径领先，不等于全面领先”。

- [x] **Step 5: 更新架构、调用路径和快速开始**

新增两个 GitHub Mermaid `flowchart`：

1. C# -> Roslyn/Guest IR -> WASM -> Wasmtime/WAMR -> Runtime -> UE；
2. Descriptor/Profile -> Generated S1 或 Semantic fallback -> Runtime/ProcessEvent。

默认快速开始命令：

```powershell
pwsh -NoProfile -File Build/InstallWasmtimeDependency.ps1 -Mode Install
```

UE 构建命令只使用 `$env:UE_ROOT`、`YourProjectEditor` 和
`C:\Path\To\YourProject.uproject` 通用示例。WAMR 构建放在兼容后端说明中。

- [x] **Step 6: 更新边界、路线与文档导航**

链接 Phase 56 实现报告、机器证据和 Gate 摘要。路线图按顺序列出：

1. semantic invocation plan；
2. Generated S1 绝对成本与 Wasmtime P95；
3. Cook/Shipping；
4. Android/iOS AOT。

### Task 3: GitHub 兼容性与内容验收

**Files:**
- Validate: `README.md`
- Validate: `Docs/Assets/README/phase56-gameplay-performance.svg`
- Validate: `Docs/Superpowers/Specs/2026-07-30-readme-refresh-design.md`
- Validate: `Docs/Superpowers/Plans/2026-07-30-readme-refresh.md`

**Interfaces:**
- Consumes: Task 1 和 Task 2 的最终文档。
- Produces: 可提交的链接、数据、格式和隐私检查结果。

- [x] **Step 1: 验证旧叙述已移除**

Run:

```powershell
rg -n "主线使用.*WAMR|Phase 53|当前 WAMR interpreter" README.md
```

Expected: exit code `1`。

- [x] **Step 2: 验证关键新叙述存在**

Run:

```powershell
rg -n "Wasmtime 45|12/18|5\\.819x|0\\.469x|0\\.513x|0\\.391x|phase56-gameplay-performance\\.svg" README.md
```

Expected: 所有关键口径均至少出现一次。

- [x] **Step 3: 验证 README 相对链接**

从 `README.md` 提取非 HTTP、非锚点的 Markdown 链接与图片路径，去除可选锚点，
逐个基于仓库根目录执行 `Test-Path -LiteralPath`。Expected: missing count `0`。

- [x] **Step 4: 验证 Mermaid 和围栏配对**

Run:

```powershell
(Select-String -Path README.md -Pattern '^```mermaid$').Count
```

Expected: `2`。

Run:

```powershell
(Select-String -Path README.md -Pattern '^```$').Count
```

Expected: 至少 `3`，与代码及 Mermaid 围栏结构人工复核一致。

- [x] **Step 5: 验证变更质量与隐私**

Run:

```powershell
git diff --check -- README.md Docs/Assets/README/phase56-gameplay-performance.svg Docs/Superpowers
```

Expected: exit code `0`。

Run:

```powershell
git diff -- README.md Docs/Assets/README Docs/Superpowers |
  Select-String -Pattern '[A-Z]:[\\/]+Users[\\/]+' -CaseSensitive:$false
```

Expected: 没有匹配。

- [x] **Step 6: 精确暂存并检查受保护文件**

Stage only:

```powershell
git add -- README.md Docs/Assets/README/phase56-gameplay-performance.svg Docs/Superpowers/Plans/2026-07-30-readme-refresh.md
```

Run:

```powershell
git diff --cached --name-status
```

Expected: 仅 README、SVG 和实施计划；三个受保护路径不在 index 中。

- [x] **Step 7: 提交并推送**

Run:

```powershell
git commit -m "docs: refresh github project overview"
```

Run:

```powershell
git push origin main
```

Expected: `main -> main`，随后 `git status --short --branch` 与 `origin/main`
同步，只保留三个受保护用户改动。
