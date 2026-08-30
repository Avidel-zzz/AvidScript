# Runtime 与生命周期策略

## 所有权

- Session 是脚本实例、VM context、callback、continuation、reload transaction 和资源 token 的生命周期 owner。
- BeginPlay、Tick、EndPlay、UE event、timer 和 async completion 进入统一 dispatcher，不各自维护私有执行模型。
- world teardown、object destruction、reload、cancel 和 module shutdown 必须幂等清理，禁止悬挂回调重新进入失效 Session。

## 对象与线程

- Guest 只持有 `ObjectHandle`；每次跨边界解析都验证 registry、代际、类型和可访问性。
- UObject 交互默认在 Game Thread；后台编译和纯数据工作通过明确队列回到 owner 线程。
- Runtime 模块不依赖 Editor；packaged/mobile 不可用能力在加载时明确拒绝。

## 执行

- Tick 热路径避免动态分配、反射重建和全局锁；按 Session 批量调度并保留预算/超时观测。
- 异常、trap、fuel、超时和 host error 映射到稳定诊断，不允许跨 C ABI 展开异常。
- reload 使用 prepare/validate/commit/rollback 事务；旧实例在新实例可用前保持一致状态。
