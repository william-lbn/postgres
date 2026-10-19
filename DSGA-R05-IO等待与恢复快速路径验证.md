# R05：IO 等待与恢复删除快速路径

日期：2026-10-19。基线为 S2 checkpoint `9aebedf238b8c890b8d972e9d2377f537cef9458`。
本片只补齐两个具体引用缺口；完整 Shrink/物理回收尚未实现，不宣称 R05 所有路径已认证。

## 发现与设计

### 快速删除

`DropRelationBuffers` / `DropRelationsAllBuffers` 在恢复期间利用精确 size cache 进入
`FindAndDropRelationBuffers`，绕过全池扫描已有的 Guard。hash 查找得到 Buffer ID 后
释放 mapping 锁，随后还要解引用 Descriptor，甚至在 `InvalidateBuffer` 内等待 IO。
静态池只存在页面复用问题；动态尾部回收还必须保证 backing 寿命。

修复在 helper 入口取得数组 Guard，保持到该次失效处理结束，锁序为
access→mapping→header。查找后仍保留原生 tag/header 复核，不因 Guard 而省略页面身份
检查。不能先持 mapping 再取 access，也不能 pin 后直接调用原样 InvalidateBuffer。
本 helper 仅用于小范围快速路径；较长扫描的活性优化另审。

### 未持自身 pin 的 WaitIO

`StartBufferIO` 的等待者已有自身 pin；`InvalidateBuffer` 的等待者则没有。
原先只依靠外层 Guard，但事务 ERROR 清理先 `LWLockReleaseAll`、后
`ConditionVariableCancelSleep`。FATAL 更特殊：`shmem_exit` 在执行任何
before_shmem_exit 回调之前就已释放 LWLocks，因此单独添加退出回调仍不能保护
IOCV backing。IO owner 完成并放下 pin 后，CV 的本地 target 可能还没清空；广播将
进程从队列取出也不等于 target 已退出。

动态模式的 WaitIO 在 Guard 内检查自身引用；没有时，通过原生
`ReservePrivateRefCountEntry` / `ResourceOwnerEnlarge` / `PinBuffer_Locked` 取得一个
临时 pin。该 pin 不增加 usage_count。随后登记 ERROR/FATAL 清理，等待仍沿用原生
先登记 CV、检查 IO 谓词、等待、退出 CV 的协议：

- 正常：退出 CV、注销回调、放临时 pin，再返回 InvalidateBuffer 复核。
- ERROR：局部清理先退出 CV，原生 ResourceOwner 随后释放 pin。
- FATAL：先放 LWLocks，但自身 pin 保留；最新登记的退出回调退出 CV，再由后续
  ShutdownPostgres/事务资源清理放 pin。不能把已经释放的 Guard 当成安全依据。

已有自身 pin 的等待者不增加引用；关闭动态模式时使用原生等待实现，不增加临时
pin 或退出回调。不改变通用事务/进程退出顺序，不引入手工 refcount 或 CV 队列改写。
共享控制结构和公开 ABI 未变，内部 API 仍为2。

## 确定性交错与证据

| 阶段 | 原生写入者 | InvalidateBuffer 等待者 | 观察点 |
|---|---|---|---|
| 1 | ReadBuffer，持 pin/content lock，实际 StartBufferIO 后暂停 | 尚未开始 | 正常 PG IO 状态，不伪造 busy 位 |
| 2 | 仍暂停 | 持关系排他锁，实际 DropRelationBuffers→WaitIO，CV 登记后暂停 | 入口确有自身 pin 和数组 Guard |
| 3 | 实际完成写入、广播、放内容锁及 pin，SQL 返回 | 仍暂停 | header 显示仅等待者一个引用，IO 已结束 |
| 4A | 完成 | 正常唤醒、CV 退出、放临时 pin、完成失效 | 原生自身 pin 检查没有报错，数据仍可重读 |
| 4B | 完成 | 查询取消，局部清理与审核回调 | audit=7：自身 pin、CV 已退队、Guard 仍在 |
| 4C | 完成 | 连接终止，shmem_exit 与审核回调 | audit=3：自身 pin、CV 已退队、Guard 已释放 |

审核回调在 Drop 前登记，WaitIO 回调后登记先执行；审核只读原生私有 pin 与 CV 队列，
不替被测实现清理。测试只将相同数据页再次标脏以制造真实写回，不删文件、修改数据、
伪造 refcount/flags/CV，或用 sleep 猜测是否已进入目标状态。ERROR/FATAL 客户端的
实际错误消息也须匹配；最终 boot 不变，100行/sum=5050，布局和后续 checkpoint 正常。

快速路径采用另一个真实临时物理备库：pg_basebackup、流复制、正常 INSERT 重放形成
size cache，随后重放 DROP TABLE。注入点断言访问 Descriptor 前已持 Guard，并记录
真实命中次数大于零。未伪造 InRecovery 或 smgr 缓存，不将主库全池扫描当成快速路径。
这证明本机原生 WAL/恢复调用链，不等于 Neon 远端存储端到端。

## 相关原生引用清单

| 入口 | 引用保护与清理顺序 | 本片边界 |
|---|---|---|
| FindAndDropRelationBuffers | hash查找前取得Guard；tag/header复核；失效完成后放Guard | 实际备库WAL重放命中 |
| InvalidateBuffer→WaitIO | Guard内临时原生pin；CV先退出、pin后释放；返回后仍按旧tag复核 | 本片5个真实Backend交错 |
| StartBufferIO→WaitIO | 调用者已有自身pin；不重复取得；使用同一CV清理 | 保留原生IO谓词/资源协议 |
| TerminateBufferIO | IO owner自身pin覆盖清IO标志、ForgetBufferIO及CV广播全程 | 真实writer路径已执行 |
| AbortBufferIO | ResourceOwner按优先级先清IO（100）、再放pin（200）；不靠LWLock | 设备故障/所有辅助进程退出仍须专项验证 |
| IO CV 初始化 | BufInit/Grow只处理未发布或可安全初始化的前缀 | Shrink/regrow必须重新证明无旧引用 |

本轮搜索内核的 BufferIOCVArray/BufferDescriptorGetIOCV 直接访问仅涉及初始化、
WaitIO和TerminateBufferIO；其他Descriptor/Blocks/工作区和扩展引用另列审计，不因
此搜索就称整个系统已穷尽。

## 验证状态

首轮新增4项与原12项合计16项通过（38.441s），含真实物理备库；随后补充 owner
尚未广播时的取消/终止两项，最终18项组合通过（48.098s）。此两项要求 CV 仍在队列时
由实现主动退队，防止把广播的效果误当作显式清理的证据。
日志 `build/v3-io-lifetime-lifecycle.log`、`build/v3-io-lifetime-lifecycle-final.log`；
源码与测试模块编译完成，最终模块编译无告警。
动态开启/关闭完整 check-world **各81组/665项报告检查通过，零 not ok**，TAP未启用。
日志 `build/check-world-v3-io-lifetime-off.log`、`...-on.log`。
pgindent、Ruff、diff检查通过；CI prepare为13项逻辑通过/18项环境skipped，后者
已另行真实执行，不与prepare重复计数。主仓更新绑定后还须再次核对完整SHA并实测。
当前编译过程中测试模块首次使用了非公开 BufferIsPinned 宏及缺少指针转换，已改为
公开 CheckBufferIsPinnedOnce 并修正转换后重编；不把首次失败算成通过。

Linux Provider 物理释放、Shrinker 与 CV 并发回收、跨 Granule/真实 Shrink-regrow、
IO 设备故障、TAP、KVM/HugeTLB、Neon Pageserver/Safekeeper 端到端、压力与性能仍待验收。
其余长 Guard 扫描和所有扩展/进程引用还须按 V3 逐路径核对，不据本片关闭整个 R05。
