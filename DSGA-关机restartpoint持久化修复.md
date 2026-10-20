# PG17：修复关机 restartpoint 遗漏脏 Buffer 持久化

日期：2026-10-20（Mac arm64）。这是额外启用 TAP 后查实的基线持久化缺陷，
不属于只能在 Linux 验证的项目，也不是通过修改预期结果规避失败。

## 根因与边界

Neon 的主库关机/end-of-recovery checkpoint 在确定 redo 之前调用
`PreCheckPointGuts`：复制状态和 Buffer 写回可能产生 Neon 的额外 WAL，必须提前。
因此后面的共同 `CheckPointGuts` 按 SHUTDOWN/END_OF_RECOVERY flags 跳过这些工作。

但 `CreateRestartPoint` 没有提前阶段，也带 SHUTDOWN flag。原代码在备库快关机时
跳过脏 catalog/heap/index 写回和复制元数据保存，随后仍推进控制文件 redo。重启
跳过本该重新执行的 WAL，热缓存中可见的表就可能不存在。加载完整 Neon 扩展也
没有消除该遗漏；不能把 LSN 已追平当作本地持久化完成。

## 修复

给内部 `CheckPointGuts` 显式传入 `is_restartpoint`，两个调用点分别传 false/true。
只有主库特殊 checkpoint 可依赖已经执行的预刷；restartpoint 始终执行共同的复制
元数据、SLRU、Buffer、fsync、2PC 顺序。不能改成观察某一时刻的全局 recovery
状态，因为 promotion 可以和 restartpoint 交错。

保留主库提前写 WAL 的原有顺序和注释，不修改 WAL 格式、不更改 VM 许可或任何
DSGA 预算事实。动态 Buffer 的实际写回仍走已有 checkpoint workspace 租约和 pin/
content/smgr 协议，不在短 accessX 内执行；本补丁不增加新的 Buffer 锁序。

## 反向证明与正向检查

新增 `src/test/recovery/t/061_neon_shutdown_restartpoint.pl`：在 basebackup 后创建
真实表/主键并写入数据，16MB 池与禁用后台驱逐保留脏页；主库生成新 checkpoint，
备库重放后验证数据，再快关机/重启并验证控制文件 redo 已推进、表/内容 hash/
主键索引仍可使用。各查询使用独立 Backend，不靠原会话 relcache。

- 修复前：前 3 个断言通过，确认重启前数据可见、仍为备库、控制文件 redo 已推进；
  随后真实查询报 `relation "restartpoint_data" does not exist`，退出码非零。
  日志 `build/neon-restartpoint-negative.log`，完整节点日志另存
  `build/audit-restartpoint-negative/`，未覆盖反向证据。
- 修复后：该用例 5 个断言通过；原 `004_timeline_switch`、`009_twophase`、
  `040_standby_failover_slots_sync` 也通过。动态关闭、开启各4文件/75断言通过，
  每组29s，日志 `build/neon-restartpoint-focused-{off,on}.log`。
- 全部 45 项 DSGA 真实集群回归通过，93.534s；动态关闭、开启的基础 check-world
  各 81 组/666 项报告检查通过，退出码均 0；这一组基础检查未启用 TAP。

日志 `build/neon-restartpoint-{fix-build,fix-tap-first,dsga-lifecycle,world-off,world-on}.log`。
最初混跑的 5 文件 TAP 命令仍因 `038_save_logical_slots_shutdown` 退出非零，不能
把该命令写成全绿。038 另已定位为 Neon 在预阶段写入文件 WAL 导致 checkpoint
位置不同于确认点；其测试适配需验证实际关机保存值与恢复值精确相等，不得只放宽
成允许任意回退。其余 TAP 失败继续独立处理。

## 仍需完成

最终提交 SHA 的主仓绑定、重编和集成在主仓记录；其他 PG 大版本需按其真实维护
源码单独移植、验证同一缺陷，不能用 PG17 结果代替。普通页物理回收/HugeTLB/
NeonVM/K8s/迁移/性能后验；Mac 逻辑测试不构成目标环境认证。完整 DSGA 控制链
仍在实施，不能据本修复发布生产或合并主分支。
