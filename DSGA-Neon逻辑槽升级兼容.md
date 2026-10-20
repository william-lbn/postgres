# Neon 逻辑槽升级与内部文件 WAL

日期：2026-10-20。功能分支修复，不发布、不合主分支；不改变 DSGA ABI 或 WAL 格式。

## 根因与修复边界

Neon 将 AUX 文件和复制槽状态持久化为 `neon-file:*` 非事务逻辑消息。真实
`pg_waldump` 显示：刚创建的槽，其 confirmed_flush 后已经有它自己的状态快照。
普通和流式逻辑输出一直按 `neon-file` 前缀过滤这些记录；fast-forward 升级检查
却将它们设置为 processing_required。因此空槽也被误判有未消费数据，甚至挡在
新集群的槽数量、输出插件校验之前。消费后保存槽又可能产生同类消息，不能靠测试
提前推进槽、调整错误信息或跳过 pending 检查解决根因。

本批新增共享判定 `IsNeonFileMessage`，保留原有整个 `neon-file` 名称空间：

- 普通/流式输出使用同一判定，实际过滤语义不变。
- fast-forward 仅不再因**内部非事务消息**设置 pending；应用非事务消息仍设置，
  COMMIT/ABORT 的原生事务判定不变，不借插件过滤、payload 或 Buffer 状态放宽。
- SQL `pg_logical_emit_message` 的 text/bytea 两个重载均明确拒绝保留前缀，错误
  SQLSTATE 22023。防止应用误以为此消息可交付，或伪装内部消息绕过升级保护。
  这是有意的兼容收紧：之前这些 SQL 调用可返回 LSN，但消息本来就不会送到插件。
- 内核仍直接调用 `LogLogicalMessage` 写 AUX WAL；未改变消息格式、数据库归属、
  复制槽保存、fsync 或关机顺序。该前缀判定不是资源承诺或 VM 执行许可。

不扩大到相似名称 `neon-fil`、`Neon-file`、`xneon-file`。不忽略任意非事务消息，
不设置“整个槽已追平”的捷径，不推进检查者的 confirmed_flush。

## 实际验证与反向证据

扩展原生 `src/bin/pg_upgrade/t/003_logical_slots.pl`，保留原作者现有校验：

1. 三类保留前缀 × 事务/非事务 × text/bytea，真实 SQL 错误与原因均核对。
2. 三类相似应用前缀显式 flush，经真实 test_decoding 实际交付。
3. 仅剩内部快照的槽能够到达真实槽数量/插件限制校验；未消费 DML 和普通非事务
   消息仍阻止升级。最终执行完整 pg_upgrade、迁移 two_phase/failover 槽，并验证
   原有订阅切换到新集群后继续收到全部数据，不仅检查进程退出码。

动态开/关两组各45断言通过。临时仅将 fast-forward 判定恢复为旧版，保留所有其他
修复及新 SQL 用例，真实失败3/35断言（槽数量与插件检查被误判抢先挡住）；随后
按备份逐字节恢复 decode.c 并重编，两组45断言再次通过。故障代码未提交。

日志（相对工作区根）：`build/neon-pending-wal-{negative,restored-off,restored-on}-tap.log`、
`build/neon-pending-wal-negative-control.log`；反向脚本
`build/verify_neon_pending_negative.py`。`pgindent --diff` 的格式差异仅在原有官方
代码，新增/改变的函数、语句和注释无格式差异；保留无关原代码，`git diff --check`
通过。候选完整 check-world/TAP 开、关结果由主仓实施状态与真实终态日志记录。

本批只修复实际 Neon 基线兼容，不能代替 DSGA 生产请求/回执/历史/取消、预算、
控制链 fencing、其他 PG 版本以及真实 Pageserver/目标 Linux 平台验收。
