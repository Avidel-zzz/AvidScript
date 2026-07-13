# Phase 39 Roslyn 正式前端收尾

状态：完成
日期：2026-07-13

## 阶段成果

Phase 39 已把 C# 的词法、语法、声明边界和诊断真相从 PowerShell 正则迁移到 SDK 内置 Roslyn。构建链固定为：

~~~text
C# Source
  -> AvidScript.CSharpFrontend
  -> versioned frontend JSON
  -> AST-selected script type / fields / method spans
  -> transitional statement emitter
  -> direct ABI WASM + manifest + build report
~~~

正式前端离线构建，不引入 NuGet 编译器依赖；正常源码与错误源码都产生确定性 JSON。语法错误会阻断 WASM 生成并清除同名旧工件，不再回退到正则或 WASI publish。

## Editor 诊断闭环

FAvidScriptFrontendReportReader 现在同时支持旧版字符串 source 和 C# 结构化 source，并读取：

- 源文件、SHA-256 与脚本类型。
- frontend artifact、schema version 与 frontend version。
- diagnostic file、UTF-16 start/length、0-based 起止行列。

底层数据保持 Roslyn 坐标约定，后续 Editor 表现层再转换为用户可读的 1-based 坐标，避免工具链之间重复换算。

## 契约修正

完整回归首次发现两个 UE 测试仍断言 actor_lifecycle_v12，而正式 AST adapter 已发布 actor_lifecycle_v13。根因是生产契约升级时遗漏跨层消费者。现已同步 Runtime、Editor 测试和构建报告文案，并把契约版本全局检查写入工作规范。

## 验证结果

| 验证项 | 结果 |
| --- | --- |
| AvidScriptRuntime 模块增量构建 | 成功，重新链接 Runtime DLL |
| AvidScriptEditor 模块增量构建 | 成功，重新链接 Editor DLL |
| AvidScript.Editor.Report | 4/4 成功 |
| AvidScript.Editor.CSharpBuildService | 2/2 成功 |
| 完整 AvidScript automation | 138/138 成功，Fail 0 |

日志位于项目 Saved/Logs：

- AvidScript_P39_4_Report.log
- AvidScript_P39_4_CSharpBuildService_Rerun.log
- AvidScript_P39_4_All_Final.log

## 当前边界

Phase 39 解决了正式语法前端和诊断产物，但尚未声称 C# 语义完整。当前语句 emitter 仍是迁移期实现，只能处理既有 gameplay 子集。下一阶段必须建立类型绑定、符号、转换、控制流和确定性语义诊断，随后由 Guest IR 彻底替换 PowerShell statement emitter。

## Phase 40 输入

Phase 40 直接消费 avidscript.csharp.frontend：

1. 建立 AvidScript 自有 symbol/type 模型，不把 Roslyn 对象泄漏给后端。
2. 实现局部变量、字段、调用、赋值、数值转换与控制流语义。
3. 以稳定 semantic JSON 作为 Phase 41 Guest IR 输入。
4. 对不支持语义给出源码范围准确的确定性诊断，不做文本猜测。
