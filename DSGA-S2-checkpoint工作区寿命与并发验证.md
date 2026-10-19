# S2：checkpoint 工作区寿命与容量发布

日期：2026-10-19。基线 `b5785f810de56b583b6548a8697561989cd9aa8a`；本片解决 V3 R04
的 checkpoint 全程持数组 Guard 问题，不代表完整 Shrink 或 VM 缩容已经交付。

## 生命周期拆分

- `CheckPointBuffers` 长期持独立 `checkpoint_lock` 共享租约，覆盖排序工作区的完整
  读写及错误清理。该租约不参与 Grow 的数组发布，因此 IO/节流不再长期占 access。
- 初始标记按最多128个 Descriptor 的批次持 Guard；每批重新校验当前 H。工作区容量
  按 checkpoint 启动快照固定，随后 Grow 新增的对象不扩展本轮扫描。
- 排序项只保存候选 Buffer ID。写入时 `SyncOneBuffer` 在短 Guard 下重新检查 H 和
  `BM_CHECKPOINT_NEEDED`，再取得原生 pin；释放自己取得的 Guard 后才等内容锁和 IO。
  不能靠旧 ID、旧 H 或工作区租约直接解引用 Descriptor。
- `checkpoint_prepared_buffers` 表示工作区 backing 已准备的连续对象前缀，不是
  活跃 Buffer 数或额外内存用量。Grow 跳过该前缀，避免对仍被旧 checkpoint 使用的
  工作区再次 memset。实际预算仍由对应数组的 committed_upper/unknown 记录，不重复计费。
- 控制结构变化将 `NEON_DSG_BUFFER_API_VERSION` 提升到2，配套扩展必须重编，不能只按
  PG17 大版本号假设内核 ABI 一致。

## 与后续 Shrink 的强制约定

1. H 下降前，尾部旧脏页必须按原 WAL/smgr 协议写净、撤销 tag、排空 pin/IO/遍历。
   因此 checkpoint 若在下一批看到较小 H，可跳过已退役尾部；旧数据不能依赖其后续写入。
2. 已写净并被替换的旧候选可以跳过；regrow 初始化的新 Descriptor 不继承旧 checkpoint
   标志。若 ID 仍合法则按当前状态/原生 pin 协议处理，不能凭排序项中的旧标识操作页面。
3. 工作区回收必须条件取得 `checkpoint_lock` 独占租约。不得持 accessX 等待该租约；
   失败即保留工作区 backing/预算并报告 pending。任何在用的旧工作区都不能被物理回收。
4. 成功回收或部分回收后，必须相应收紧 prepared 的已证明前缀；物理失败/未知范围继续
   保守计账。新 Grow 与旧 Release 必须由原 resize 锁串行，禁止晚到回收破坏新内容。

第3/4项在完整 Shrink 中接入并验证。当前没有伪造较小 H 来替代真实 Shrink/regrow 测试。
普通 BG writer/FlushRelation 等长扫描还保留原 Guard，继续列为活性/性能审查项，不能
把本片扩大解释为所有慢 IO 都已解除保护。

## 确定性交错

| 步骤 | checkpointer | Grow/控制会话 | 必须成立 |
|---|---|---|---|
| 1 | 正常标脏页并结束短批次，持工作区租约后暂停 | 观察真实等待状态 | 没有数组 Guard，但工作区不可独占 |
| 2 | 继续暂停，排序项和 backing 保留 | 1秒期限内 Grow 1024→2048 | H/代次正常提交，工作区旧前缀未清零 |
| 3 | 唤醒并完成真实 checkpoint | 校验数据/索引及租约可独占 | 没有错误写入或遗留租约 |
| 错误A | 标记扫描后注入 ERROR | 再尝试 Grow/checkpoint | 原生错误清理后租约释放，boot 不变 |
| 错误B | 已持真实 pin/content lock、写入前 ERROR | 再尝试 Grow/checkpoint | 非末尾 LWLock 与 ResourceOwner pin 清理正确 |

测试通过预加载模块的共享门禁决定真实 checkpointer 何时继续，不修改脏页标志、pin
计数或 checkpoint 完成状态。错误B不是设备 IO 故障模拟；真实 Linux/Neon 存储 IO
故障仍属于后续端到端验收。临时集群使用8MB启动池并关闭 BG writer 清理，以确保
真实脏页进入 checkpoint，而不是等待一个没有工作量的空调用。

## 验证记录

首轮11项真实集群测试通过。增加持 pin/content lock 后 ERROR 后，最终组合12项
通过（31.939s），含三个新 checkpoint 场景及原有 M2f/恢复/禁用路径。
日志：`build/v3-checkpoint-build.log`、`build/v3-checkpoint-lifecycle-final.log`。
动态开启/关闭完整 check-world **各81组/665项报告检查通过，零 not ok**；TAP未启用。
日志 `build/check-world-v3-checkpoint-off.log`、`build/check-world-v3-checkpoint-on.log`。
pgindent/diff及Neon Python Ruff检查通过，CI prepare逻辑测试另记，环境测试skipped
不与上述12项实际执行混记。提交后主仓还须更新三处版本绑定并核验二进制完整SHA。

TAP、Linux Provider、KVM/HugeTLB、真实 Pageserver/Safekeeper 存储端到端与性能仍待验收。
