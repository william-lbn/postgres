# PG17 issuer 代次接管栅栏

日期：2026-10-21。ABI6，实际源码仍为Neon vendor/postgres-v17子模块。本模块
扩展现有DSGA内核，不改变Neon远程页/WAL、Buffer ID或固定地址数组架构。

## 不变量与接口

共享issuer_floor_epoch在同boot只增不减。新操作在resize独占锁内，并在同一次
status短锁内比较代次、归档旧终态、提升floor和登记新意图；不能检查之后无条件
覆盖。低代次请求不能开始新的Grow/Shrink，已有完整意图仍允许只读历史重放。
旧内部epoch0只在未见过非零代次时兼容；接管以后不能绕过正式管理链。

独立DsgBufferFenceIssuer(boot,epoch,response)不要求构造resize或改变H/generation。
匹配boot且代次不低时，先在status短锁推进floor，若旧resize尚在可取消窗口则
请求其取消。随后只条件取得resize独占权：

- PENDING：floor已推进，旧resize/同步Provider尚未确认退出。不能当成完成。
- FENCED：已取得resize、收敛FATAL遗留并复查floor未被更高代次取代；确认的是
  PG resize/Provider串行执行者退出，不是普通SQL排空或物理释放/VM完成。
- STALE：boot不符或已有更高代次，不降低floor、不重新执行旧操作。

逻辑COMMITTED不等于串行权已释放，Shrink的Provider可能仍在运行。不能按phase
单独生成FENCED。旧执行在BeginCommit前再检查floor/取消；已经关闭提交窗口的
操作完成原发布与释放，不伪装取消，也不回滚H。取消/异常保留未知分配的成本。

锁序仍为resize→access→status。接管先取得/释放status，再条件取得resize，不
反向持status等待其他锁；禁止同调用者持access/resize递归进入。短锁内只有固定
大小复制/单条归档，不分配、IO、遍历目录或报可恢复错误。LWLock的holdoff与
RESUME/HOLD成对，异常通过PG_FINALLY释放实际持有的串行权。

floor在真正新boot归零，旧boot请求失效；同boot内到UINT64_MAX不会回绕。该数值
不认证领导权。调用方必须使用可信持久owner/租约，并保存原意图/预算和跨进程
对账；函数不清外层历史或客户端pending，不发放VM许可。准入/全资源预算属于
后续独立完整实现，不能把本栅栏描述为全部R02/R03已经完成。

## ABI与验证

DsgBufferSnapshot新增issuer_floor_epoch，控制结构亦增字段，ABI从5升级6。所有
引用该结构的Neon扩展/测试库必须用匹配头清理重编；正式SQL接入为Neon1.8，
客户端协商要求ABI6和完整6个管理函数。不混用旧.so与新结构，不在线热替换
PG二进制。父仓gitlink、两份版本锁与实际二进制SHA必须统一。

本提交前Mac候选真实验证：80个生命周期/控制用例通过151.387s（旧69加11）；
扩缩开/关各完整check-world与TAP通过，各48份TAP报告、231报告文件/18410
报告断言，包含平台/可选skip，不把它们记作业务零skip。Neon完整52项77.814s、
原生Rust SDK10项14.32s另验证正式入口，但父仓实现与验证记录独立提交。

两项确定性反向：移除登记的原子floor比较，增长与收缩各错误接受旧请求（2失败）；
按COMMITTED直接返回FENCED，旧Provider仍持串行锁时错误成功（1失败）。脚本
finally逐字节恢复Control源码并重编，SHA256为
9dbbc8b41d1ee54938d567bedb6fed2e3b0df6dac1817d352b146050a4d85f0e。
恢复后的全量通过，未保存故障版本到Git。新增桥只转换真实内核结果/安排交错，
不伪造锁、状态、pin或Provider结果。

工作区build日志neon-issuer-{candidate-control.log,candidate-full-control.log,
native-negative.log,candidate-extension.log,candidate-sdk.log}；world完整日志在
build/neon-integrated/v17/dsga-issuer-world-candidate/{off,on}.log。候选明确记录
未提交源码diff摘要，提交后还须重编准确SHA并通过父仓严格门禁，才能推父仓引用。
Linux/KVM/HugeTLB/VM物理回收/迁移/K8s/性能后验，不构建镜像、不部署、不合主分支。
