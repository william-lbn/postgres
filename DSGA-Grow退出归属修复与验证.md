# Grow 退出清理：操作归属修复与永久回归

日期：2026-10-19。基线 c47f959。本片修复完整性复核中实际复现的旧 FATAL 回调
改写新操作问题；不代表完整 Shrink、控制链或生产验收已完成。

## 修复不变量

- 清理上下文复制本次请求，且只有本调用真正建立 operation 后才允许清理。
- 普通 ERROR 局部展开时沿用已持 resize 独占锁；FATAL 已经释放锁时，只条件重取。
  取得失败立即保留事实返回，不等待新操作、不修改其 Granule/phase/reason。
- 重获串行权后仍核对完整 boot/revision/operation/generation/target，且仅对自身
  PREPARING 执行 DENIED。已有 COMMITTED 不回退，新操作状态不被旧回调污染。
- 清理上下文在 PG_TRY 前分配到事务内存，指针不变，避免 longjmp 后读可变自动变量。
  正常注销回调后释放；错误/退出由后续原生内存清理回收，不在回调内重新分配。
- 所有 DsgGrowDeny 调用现在显式断言持 resizeX；不改 PG 通用退出顺序、容量/WAL
  或 Provider 保守预算语义，也不只清空 reason 掩盖竞态。

## 永久真实集群用例

1. 旧操作发布前 FATAL，退出放锁后暂停；新操作持真实 resizeX、PREPARING暂停，
   旧清理运行后新状态必须完全不变，再完成 COMMITTED/NONE。
2. 旧操作发布后 FATAL；新操作发布前暂停，覆盖旧 COMMITTED 不应污染下一操作。
3. 新操作已提交并放锁，旧回调随后重获锁，仍须按身份拒绝清理。
4. FATAL 无竞争者时正常重获锁清理自身；原 ID 重放 DENIED，新 Revision 可重试。
5. 尚未建立操作就因 generation 冲突拒绝的调用，不得清理另一 PREPARING 操作。

门禁只调度现有真实 Grow/退出路径，不改被测 phase/refcount/锁；消息和最终状态
均校验。首次发布后用例把新协调者也暂停在发布后，已补独立发布前注入点后重跑，
不将该测试调度错误算作实现通过。

## 验证

23项真实集群组合通过（49.377s），含新增5项和原18项。动态开启/关闭 check-world
各81组/665项报告检查通过，零not ok；TAP未启用。pgindent/diff/Ruff检查通过。
日志：build/v3-grow-cleanup-build.log、build/v3-grow-cleanup-lifecycle-final.log、
build/check-world-v3-grow-cleanup-{off,on}.log。CI prepare和最终主仓SHA绑定另记主仓。

共享结构/公开API未变，内部能力版本仍为2。产品默认关闭；完整Shrinker、其他
版本和Linux/K8s/Neon存储/VM/HugeTLB/性能/长稳仍按V3独立验收。
