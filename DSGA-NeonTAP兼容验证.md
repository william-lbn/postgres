# Neon PG17 TAP fixture 与持久化断言适配

日期：2026-10-20。此提交只改变测试，生产 restartpoint 修复是独立的 `e02a09ad8c`。
不以关闭 Neon WAL、忽略失败、减少业务校验或更改生产序列语义获得通过。

## 实际修改

- `append_shared_preload_library` 用真实 `postgres -C shared_preload_libraries`
  解析配置，再追加简单库名；保留 TEMP_CONFIG、include 与字符串转义。解析器
  命令失败直接失败，不能回退到空列表。test_custom_rmgrs 与 stream_regress
  fixture 不再覆盖 Neon rmgr。没有 rmgr 134 的节点不能解释 Neon WAL。
- 流复制的序列断言采用 Neon `SEQ_LOG_VALS=0` 的精确值 `1|0|t`；两层真实备库
  都校验相同状态。保留逐次 WAL 语义，不改生产常量去迎合社区预写32个值的预期。
- 关机槽检查继续要求“实际保存的 confirmed_flush 与重启后的解码起点精确相等”，
  并检查关机阶段确实保存、确认点不超过持久 checkpoint。Neon 的 neon-file
  消息在预阶段增加 WAL，不能要求该确认点等于后来的 checkpoint 地址；也不能
  只放宽成允许任意旧 LSN。保存日志只从关机前窗口提取，不接受重启后的新保存。

## Mac 实测

- `test_custom_rmgrs/t/001_basic.pl`：1文件/2断言通过，3s。
- recovery `001_stream_rep`、`027_stream_regress`、`038_save_logical_slots_shutdown`：
  3文件/75断言通过，61s；027内还真实运行标准回归和备库数据比对。
- 038反向验证临时省略 `CheckPointReplicationSlots` 的全部关机保存，真实用例
  2/4断言失败，明确检出保存缺失和确认点不匹配。随后根据备份逐字节恢复
  `slot.c`、重编，4断言通过（8s）。未修改/提交这个故障版本。

日志分别为 `build/neon-tap-preload-custom-rmgr.log`、
`build/neon-tap-fixtures-positive.log`、`build/neon-slot038-{negative,restored}.log`。
反向脚本、节点数据及备份只保存在本地 build，不进入发行源码。

最初8个失败文件中，004/009/040由生产持久化修复恢复通过；本提交处理上述4个
fixture。`pg_upgrade/t/003_logical_slots.pl` 仍未通过：Neon关机内部文件消息被
pending-WAL检查视为待消费变更，尚须进一步确认和安全处理。未声称全部TAP或
完整生产功能已完成。后续主仓还须按最终源码SHA重编集成，Linux能力认证后验。
