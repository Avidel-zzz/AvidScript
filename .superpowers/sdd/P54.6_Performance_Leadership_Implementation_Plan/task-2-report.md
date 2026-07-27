# P54.6B Generated Binding IR 与确定性 Source Emitter 实施报告

## 状态

已在任务 owned paths 内完成 P54.6B 的产品实现与窄测试代码，未运行 UE
全构建、Automation 或正式 benchmark。

提交 SHA：`6aaff016eeed8ec88fb6670137a6dcab66552c5b`

## 已实现

- 新增后端中立的 Generated Binding IR，Core 公共头不依赖 UObject、Editor、
  文件系统或 Wasmtime。
- class rule 新增 `GeneratedNativeFunctions`，resolver 对非 functions 子集、
  通配符和 direct/generated 冲突执行稳定分类拒绝。
- descriptor 新增 `generated_native_s1`，canonical identity 与 package hash
  纳入 shape、receiver、确定性 import 和 semantic fallback ordinal。
- S1 import 使用语义 canonical SHA-256 的前 16 位：
  `avid_s1_<16 lowercase hex>`，最终 generated canonical identity 再纳入该
  import，避免哈希自引用。
- 当前资格证明支持 native instance `int32(int32,int32)`，并建模
  `PropertyI32GetSet`、`VectorValue`、`StableObjectRoundtrip`。RPC、event、
  interface、custom thunk、Blueprint owner、非 value 引用方向和缺失
  module/header 身份均 fail closed。
- C# renderer 继续使用 descriptor host import，因此 facade 不变；
  manifest 保留 generated dispatch 元数据和 semantic fallback ordinal。
- 新增确定性项目 Source emitter，输出到
  `<Project>/Source/AvidScriptGeneratedBindings/`，不写入 Intermediate。
- emitter 对 owner include、标识符、SHA-256、import 命名和重复 stable id
  做写盘前校验；owner module 依赖排序去重，并在写盘前检查直接依赖环。
- `.uproject` Modules 数组采用局部文本插入，不重排其他字段；重复执行先
  解析 Modules 识别已有模块。
- module 文件与 `.uproject` 使用同卷 staging、备份替换和失败回滚。
- 生成的四类调用点全部先 `Cast<Owner>`，失败返回 `Rejected`；entry 按
  `I32PairCall`、`PropertyI32Call`、`VectorValueCall`、
  `StableObjectRoundtripCall` 四槽互斥初始化，不生成非对应 shape 的空 thunk。
- 新增确定性、输入顺序无关、单字段 identity 变化、恶意 include、profile
  授权拒绝、生成源码生命周期与四槽函数指针 token 测试。

## 窄检查

- `git diff --check`：通过。
- 产品源码扫描：未发现 benchmark/Puerts/PerfHarness 标识或 TODO/TBD；
  `CastChecked` 只出现在“生成结果不得包含 CastChecked”的测试断言中。
- `Build/CheckAvidScriptArchitecture.ps1`：未通过。6 项中包含并行 P54.6A
  的 Wasmtime 隔离与 dirty evidence；另有旧静态规则只接受
  `avid_ue_<stable-id>`，尚未识别本阶段的 `avid_s1_*`，不能据此回退新
  descriptor 契约。

## 集成关注

1. profile JSON 字段实际由
   `Source/AvidScriptEditor/Private/AvidScriptEditorCSharpProfileService.cpp`
   解析；该文件不在本 brief 的 owned paths 内，因此本提交只完成
   selection model/resolver 语义，未越权接入 JSON
   `generated_native_functions`。阶段集成者需在该唯一解析点补 schema
   授权和字段读取。
2. 生成模块按生命周期调用
   `FAvidScriptGeneratedBindingRegistry::Get()`；Task4 应把该进程内唯一
   registry accessor 与四函数指针 entry 一并冻结，避免生成模块和 runtime
   使用不同 registry 实例。
3. 未运行 UE 编译，四类生成 thunk 的最终编译验证应并入阶段末统一构建门。
