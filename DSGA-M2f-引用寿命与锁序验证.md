# M2f：最后 unpin、原生等待者与分配栅栏

日期：2026-10-19。基线：`b2f05855ca7feaa800cf1491c5366d0a33e7fdef`，已包含
Neon PG17 稳定头与社区 `63de1f8fa42060e10326d2d9a0d48e559594ab86`。
范围：V3 的 S1/R05 前置修订；尚不是完整 Shrink，不授权 VM 缩容。

## 本批实际改变

1. `UnpinBuffer` 在自身 pin 和 ResourceOwner 登记仍有效时尝试 Neon test evict。
   不再执行“unpin→重新 pin→递归 unpin”。ERROR 的 NoOwner 清理不执行测试 IO。
2. `UnpinBufferNoOwner` 遇到原生 pin waiter 时，在自身共享 pin 仍有效时锁 header，
   完成减引用、清位和通知目标复制。之后只清私有记录、发送保存的通知，不再访问
   Descriptor。普通无 waiter 的 CAS 路径保留，不对每次 unpin 增加全局 Guard。
3. `StrategyFreeBuffer` 不再取得 access 锁。先以稳定地址计算索引，在 strategy 锁
   内检查 L，符合范围后才解引用；读取屏障与 Grow 初始化后的发布屏障配对。
4. 内部 `StrategySetAllocationLimit` 串行降低/恢复 L、过滤 freelist 并重置 clock。
   下限为启动 NBuffers，上限为 H；不发布 H、代次或物理回收结果。整链先摘下，
   节点逐一短取 strategy 锁重接，避免在 spinlock 下做大容量整链遍历。

## 寿命与锁序清单

| 路径 | 实际保护 | 释放/异常规则 |
|---|---|---|
| ReadRecentBuffer 的无私有 pin 分支 | access 保护 H 检查和描述符访问，取得正常 pin 后交接 | stale hint 在解引用前拒绝 |
| GetVictimBuffer / clock / ring / freelist | access 保护选择到 PinBuffer_Locked；后续由 pin 保护 | 新 victim 受 L 约束，已有 pin 不按 L 判无效 |
| UnpinBuffer 的 test evict | 自己尚未释放的共享 pin；保留原 mapping/header/content 协议 | 异常时 ResourceOwner 仍能释放原 pin |
| NoOwner 的最后引用 | CAS 或 header 锁内的减引用；waiter 目标提前保存 | 最后 pin 后不再读 Descriptor；发送通知前无新增可报错点 |
| StrategyFreeBuffer | strategy 锁内 L 校验，无 access 获取 | 与所有 L 下降同锁串行；尾部不重新插入 freelist |
| DropRelationBuffers / DropRelationsAllBuffers / DropDatabaseBuffers | access→header/mapping；保留原 InvalidateBuffer 语义 | InvalidateBuffer 禁止自身持 pin，不机械改为“先 pin 再调用” |
| FlushRelationBuffers / FlushRelationsAllBuffers / FlushDatabaseBuffers、BgBufferSync | 现有 Guard 与正常 pin/content/IO 协议 | 寿命保守；长扫描/IO 占 Guard 的活性问题后续继续收窄 |
| pg_buffercache 的 pages/summary/usage/evict；autoprewarm dump | 从读 H 前到最后解引用持 Guard | 嵌套 Guard 不重复持锁；ERROR/FATAL 原生清理 |
| PrintBufferDescs / PrintPinnedBufs / EvictUnpinnedBuffer | 受保护扫描/边界验证 | 不能凭 H 快照持有悬空地址 |
| CheckPointBuffers / workspace | 现有全程 Guard，仍有 V3 R04 活性缺口 | 本批不删除保护；下一切片分离工作区寿命 |
| StartBufferIO / TerminateBufferIO | 原生 pin 覆盖 IO 状态、ResourceOwner IO 登记及 CV broadcast | IO 完成不等于可以在通知前放弃 pin |
| AbortBufferIO / WaitIO | M2f 时依赖 pin/外层 Guard；后续审查发现 ERROR/FATAL 先放 Guard 的窗口，已由 R05 子片增加临时原生 pin 与 CV 清理 | 见 `DSGA-R05-IO等待与恢复快速路径验证.md`，不能仅凭 Guard 覆盖退出路径 |

保持 `access→mapping/header` 的现有访问规则，消除归还路径的反向边；不在持 mapping
锁的热路径增加 access 获取。协调器 `resize→access→strategy→status`，status 只做
短发布，任何 spinlock 内不执行 IO、分配或长遍历。IO/AIO 的完整跨版本证明仍归 R05/R10，
不能将上表的 PG17 同步路径结论移用于 PG18 AIO。

### freelist 过滤与并发归还

协调者持 accessX 阻止新 victim。L 下降和旧链根摘除在 strategy 锁内完成；旧链尚未
处理节点保留“已在链表”标记，归还者不会重复插入或改写其 next。逐节点取 next 后，
在同一 strategy 锁内标记移除，并只将低于 L 的节点接入新链。并发归还同样在该锁内
按 L 决定，不能让尾部重新进入新链。H 未改变，整个旧链 backing 此时仍然有效。

## 确定性验证与覆盖边界

- 分配栅栏：先 Grow 至512，拒绝低于启动256或超过H的目标；降 L 到256而保留H512，
  用普通 CTAS 写1200行，核对内容及该关系实际 Buffer ID 均低于256，再恢复L512。
- mapping/access：returner 持真实 mappingX 后暂停；fencer 持 accessX 后暂停；先唤醒
  returner，要求其正常归还并结束，然后才能唤醒 fencer；最后检查数据。由真实锁交错
  检查反向取锁，未修改 isolationtester 的阻塞判定。
- 原生 BufferPin：Neon 主仓真实多客户端测试先等待 holder 的正常 pin，再确认另一个
  Backend 真实进入 `BufferPin`。分别唤醒/cancel/FATAL，再次取得 cleanup lock，检查
  数据与 boot 未变。没有直接修改 refcount、waiter 位或假造原生等待。
- 最后访问探针：普通 SQL 使用真实扩容后的干净 Buffer；最后 unpin 的测试点将本进程
  对应 Descriptor 页改为 PROT_NONE，ReleaseBuffer 返回即恢复。检查后续数据一致。
  这是本进程访问寿命探针，不是 Linux 跨进程物理释放或 Host 回收证明。
- 既有锁释放测试显式检查调用者未遗留 access 锁，再作有界条件重试，以免后台短扫描
  干扰单次试锁；固定注入点暂停的读者仍必须报告不可独占。

最终通过数、开关完整回归及版本绑定由 Neon 主仓模块记录更新。TAP、Linux Provider、
KVM/HugeTLB、全 Neon smgr/Pageserver/Safekeeper 端到端和性能不由本批 Mac 结果覆盖。

## Mac 实际验收结果

- Apple clang15 / SDK14.4，Assert/debug/depend/injection-points，Neon 主仓集成构建
  `build/neon-integrated/v17`，安装 `build/neon-integrated-install/v17`。
- 生产改动最终组合：动态关闭和开启的完整 check-world **各81组/665项报告检查通过**，
  日志 `build/check-world-v3-m2f-off.log`、`build/check-world-v3-m2f-on.log`，零 not ok。
- 后补测试入口对临时关系的拒绝断言（不改生产路径），模块最终复验 **1项regress +
  4项isolation通过**，包含 mapping/access 交错；日志 `build/v3-m2f-module-accepted.log`。
- 最终测试模块安装后，Neon 主仓真实临时集群测试 **9项通过，20.956s**，包含
  native pin 三种路径、最后访问探针、Grow/恢复/禁用/非法配置；日志
  `build/v3-m2f-lifecycle-final.log`。这不是完整 Neon smgr 存储端到端。
- pgindent check、diff check 通过；Neon Python 的 Ruff检查/格式通过，CI prepare
  12项逻辑检查通过、9项环境测试skipped，后者已经按上一条另行真实执行。

验证中修正了测试自身的两个问题：原 native pin spec 无法被 isolationtester 正确识别，
改用真实客户端；新增 spec 不再假设前面 Grow spec 未改变共享 H/代次。没有通过降低
业务断言或伪造物理回执来取得通过结果。
