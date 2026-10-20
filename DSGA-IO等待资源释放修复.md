# DSGA 无 pin IO 等待与事务资源释放

日期：2026-10-20。默认关闭、不改变内核ABI或WAL格式；当前验证事实与剩余项
由本模块记录及主仓实施状态共同保存，不能据局部通过声称完整功能已交付。

## 全量测试发现的实际缺陷

动态开启的完整 check-world/TAP 中，`recovery/027_stream_regress` 在真实
主库执行 `mvcc.sql` 的 ROLLBACK 时失败：先报
`ResourceOwnerEnlarge called after release started`，随后重复中止触发
`ProcArrayEndTransaction` 断言，Postmaster 按 `restart_after_crash=off` 退出。
不是预期的 SQL 负例，也不是环境不支持或可以忽略的随机失败。

回滚删除关系时也会在资源释放回调中走 DropRelationBuffers → InvalidateBuffer
→ 无自身pin的WaitIO。原来的临时pin登记到CurrentResourceOwner，但该owner已经
进入releasing阶段，原生接口正确拒绝新增资源。独立专项过去没有构造这个入口，
正常SQL和已有ERROR/FATAL测试通过，不能证明资源释放阶段也安全。

失败日志、regression.diffs、mvcc结果和Mac崩溃报告已保存到工作区
`build/audit-dsga-rollback-waitio-crash/`；原整轮日志为
`build/neon-pending-wal-world-on.log`，整体退出2。对应关闭组退出0、354.630s。

## 生产改动及不变量

只改变动态模式、无自身pin的WaitIO：

1. 在已有Guard下建立堆上清理上下文；已有自身pin/静态模式不增加该分配。
2. 用无父节点的专属ResourceOwner登记本次真实 `PinBuffer_Locked` 的pin。
   不更改正在释放的事务owner，不向正在遍历的owner树添加子项，不直接构造
   私有/共享引用计数。切换CurrentResourceOwner仅包住原生pin登记，随后恢复。
3. 正常/ERROR/FATAL都先退出ConditionVariable，再移除本次owner登记、删除空
   owner，最后通过原生UnpinBufferNoOwner释放真实引用。FATAL即使已放Guard，
   pin仍保护Descriptor/IOCV直到撤队完成。
4. 不完整调用ResourceOwnerRelease，避免递归触发扩展全局资源回调。最后unpin
   前清除本次归属标记；探针报错重入不能重复释放或访问已经失去pin的对象。
5. owner创建或private refcount预留失败也经过已注册清理；只清理本次真实存在
   的owner/pin。SIGKILL仍由Postmaster重建共享boot，不能在旧boot伪报清理完成。

## 用例与实际进度

测试模块通过原生ResourceOwnerRelease建立真实releasing状态，在回调中执行
DropRelationBuffers；不写owner私有标志、不伪造IO/pin/CV。新增完成、取消、FATAL
三项，沿用真实writer和独立invalidator交错。原有五项仍覆盖原owner广播前后，
CV审核点移到“生产清理已撤CV、尚未放本次pin”，不能在外层已失去pin后读取对象。

候选48项完整内核专项通过92.050s，零skip。新生产C与测试C通过pgindent --check，
Ruff check/format及git diff --check通过。仅将WaitIO恢复为旧版后，新增资源释放
用例在15.544s内确定性失败，并记录原始ResourceOwnerEnlarge错误；随后按备份
逐字节恢复bufmgr.c并重编，八项IO专项通过16.282s。真实主备027回归9断言通过
51s，包含在主库实际跑完整regress、主备追平及数据/catalog dump比较。

日志 `build/neon-waitio-{negative,restored}-tests.log`、
`build/neon-waitio-stream-regress-on.log` 与
`build/neon-waitio-releasing-negative-control.log`；反向脚本
`build/verify_waitio_releasing_negative.py`。最终完整开/关门禁结果记在主仓，
不把一次定向027通过扩大解释为全部TAP、所有并发窗口或最终生产认证。

后续仍须准确SHA重编、完整开/关回归、完整Neon扩展验证，再按依赖顺序提交推送。
目标Linux物理回收/HugeTLB/VM验收与尚未实现的生产API/预算/控制链是独立剩余项。
