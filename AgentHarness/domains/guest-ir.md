# Guest IR 与 ABI 策略

## 分层

- Guest IR 是语言前端与 WASM backend 之间的稳定中间合同，不直接引用 UE 类型。
- IR 类型、指令、布局和 capability 都必须版本化并由 validator fail-closed。
- WASM 导入导出、线性内存、复合值和 callback metadata 使用规范化 ABI；host 与 guest 共享同一 schema owner。

## 变更清单

- 新 IR 能力同步更新：模型、布局解析、验证器、序列化、后端 emitter、兼容读取和跨层 fixture。
- 新 descriptor 字段同步更新 canonical round trip、哈希/identity、旧版兼容和 future-version 拒绝测试。
- 内存 offset、length、alignment、ownership 和 endian 必须显式；边界检查在读取前完成。
- object/reference 只传 handle 或受控引用，不把宿主地址编码进 Guest ABI。

## 性能

- 优先静态布局、预计算 codec program、批量 crossing 和无分配 fast path。
- 复合容器采用显式生命周期的 heap/token，递归深度、元素数和总字节有上限。
- benchmark 区分纯 WASM 执行、ABI crossing、codec、registry 和 UE reflection 成本。
