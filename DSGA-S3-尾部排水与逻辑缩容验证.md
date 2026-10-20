# S3：尾部排水、逻辑 Shrink、逐数组回收与 regrow

更新：2026-10-20（Asia/Shanghai）。PG17 / V3 内核候选，内部能力 API4。
这是完整 PG/Neon 交付中的一个模块；正式 API、跨层预算/执行 fencing、版本移植
和 Linux 认证仍须继续完成。功能默认关闭，不授权 VM 缩容。

## 状态与线性化

`NBuffers = B_boot <= L <= H <= M`。Grow/Shrink 共用身份和单调 revision，操作 kind
区分两者；同一 operation ID 不能换方向或参数。重复当前请求只返回既有终态，不能
重复发布 generation；容量不变的回收重试使用新 revision，不增加 capacity generation。
旧请求历史与正式独立 receipt 留给后续 API 模块，当前仅有当前操作快照。

1. 持 resizeX 建立 PREPARING；短 accessX 内发布 L 并从 freelist 原子摘下旧链。
2. 旧链游标在固定共享策略区，在 accessX 外按 128 个节点过滤。每节点在 strategy
   spinlock 内推进共享游标/解除旧标记/重接低于 L 的节点。Clock 可以继续工作；
   新 victim、ring 和并发归还者遵守 L。游标不是退出进程栈上的临时指针。
3. 遍历 `[target, old H)`，每个对象依次使用短 Guard/header 核验与真实 pin。
   pin、IO、竞争 content/mapping 锁就跳过重试；不强清引用。dirty 走原生
   content lock、StartBufferIO、WAL flush、checksum 和 smgrwrite，保留 Neon 写回入口。
   nowait 只避免等待其他 IO owner；本次已取得 IO 的同步写仍是页间检查的软期限。
4. tagged-invalid 失败读残留也按 mapping->header 复核并撤 hash/tag。持 header
   不等待 mapping；成功撤 tag 后，在 L 栅栏下该 ID 不能再承接新的 victim/tag。
   一整轮成功排水是单调证明，最终 accessX 排空旧 Guard/CV 引用，发布时不重扫全池。
5. 短 accessX 下发布新 H/generation/COMMITTED，并将四数组置为绑定本 revision/
   generation 的物理 pending。逻辑提交之后，SQL ERROR、ROLLBACK、取消或丢响应
   不撤销 H。目录更新、实际数据 IO 与 Provider 均不在短 H 发布锁中执行。
6. 保持 resizeX，在 accessX 外逐数组 ReleaseTail。checkpoint 仅条件取得独占租约；
   忙则保留其 prepared 容量、backing 与预算。未报告/失败/未支持的释放均保留原成本。

## 预算与失败

数组 `committed_upper_bytes` 是保守占用上界，不是 RSS。每次回收记录 revision、
generation、状态及 Provider 原始回执；起始边界页保留，只有全安全尾部实际释放证明
才能降低上界。不存在已计费尾部时可返回已对账 NOOP，但 physical_release_verified
仍为 false；消费者不能把 NOOP 冒充一次物理释放。

- H 发布前失败：恢复 L=H，正常驱逐的页面由存储承载，无须重新装回。旧链过滤收尾
  不等待 accessX/IO；已摘下且处理过的尾部节点仍可由 Clock 正常选择。
- FATAL 原生先放 LWLock：清理只条件重获 resizeX，并核验原身份/kind/PREPARING。
  新协调者发现遗留 Shrink PREPARING 时主动恢复 L/共享游标，再处理新请求。
- H 发布后失败：保持较小 H；物理 pending/未支持/未知成本保留，新 Grow 串行重准备。
  旧清理回调绝不继续 Release，更不能回收新 Grow 刚使用的范围。
- checkpoint 租约忙：工作区仍按旧 prepared 前缀保留；Grow 从该前缀之后准备，
  不清零旧 checkpoint 正在排序/读取的条目。新 revision 可在租约释放后重试回收。
- Mac 普通页 Release 不支持：逻辑 H 可以下降，成本保持；不能据此发 VM 许可。

## hash 冲突路径的引用边界加固

原 BufferAlloc hash 插入冲突分支先 UnpinBuffer，再 StrategyFreeBuffer。后者已经
通过稳定 VA 计算索引，在 strategy 锁内检查 L 后才解引用 Descriptor；因此仅恢复
旧调用顺序的反向测试仍通过，不能宣称原版本在该交错存在悬空读取。本批仍改为先
归还策略链、再释放 pin，与相邻原生扩展路径顺序一致，减少对特殊保护契约的依赖。
确定性交错由两个真实 ReadBuffer 制造 hash 冲突：旧 victim 在尾部，新读者在 L
以下装入同页；原 victim 最后 unpin 后，仅本测试 Backend 对其 Descriptor 页执行
mprotect(PROT_NONE)，ReadBuffer 返回后立即恢复权限。该探针不是物理回收证明。
反向验证同时恢复旧顺序并将安全 VA 索引改成直接读取 `buf->buf_id`，真实 Backend
被信号终止，测试如预期失败；随后按备份逐字节恢复两份源码、重编，正向用例通过。
这是探针对显式引入错误的灵敏度验证，不是声称未改动的旧版存在同样错误。

## Mac 验证证据

环境：macOS arm64，Xcode clang + MacOSX14.4 SDK，debug/assert/depend/injection-points，
真实共享映射、独立临时集群、数据 checksum，配套 test_dsg_region/neon_rmgr。

- 16 项新增 Shrink 专项通过，27.081s。
- 全部 45 项真实集群回归通过，91.379s；包含原来的 Grow、准入、IO/CV、物理备库恢复。
- 原生模块：1 项 regress + 5 项 isolation 通过。Shrink spec 从实际共享 Postmaster
  快照建立 revision/generation 基线，验证 L/H 两阶段、竞争超时、业务数据及 regrow。
- 动态关闭、开启 check-world：各 81 组、666 项报告检查通过。
  该轮 TAP 尚未启用，不计入已通过范围。
- 临时集群使用 PLAIN 大页面表，工作集确实超过 bootstrap；Shrinker 写净尾部后，
  执行正常 SQL 校验 count、sum、字节数和内容 hash，跨 Granule 反复扩缩后再次校验。
- 覆盖真实 pin 等待/释放、取消恢复 L、IO owner 被暂停但 accessX 仍可取得、失败读
  tagged-invalid、hash miss 重载仅落在 L 以下、旧 checkpoint/regrow、逐数组第二次
  Provider 调用前错误、提交后错误、Provider 阶段取消，以及三处 FATAL/后继 Grow。

日志在工作区 `build/v3-shrink-{lifecycle-final,all-lifecycle,native-final}.log`、
`build/check-world-v3-shrink-{off,on}.log`。最终提交还须主仓精确源码锁/二进制重编验证。
反向与恢复日志为 `build/v3-shrink-collision-{negative,restored}.log`。
上述结果基于社区 `63de1f8` 维护快照；新到 `91246c7` 官方维护合并及组合测试单独记录，
不能将旧测试结果视为已覆盖新代码。

### 最新官方维护组合复验

独立维护 `4a39412f154796461f404785d369a3dc9424fcdc` 已包含 Neon `56692df` 与
社区 `91246c7` 的全部维护历史；由 `873715d889` 合入本功能分支后，只恢复一次新
stash `f5c4dfc54b3600a4191b4a7f06d2437db3b8403b`，恢复补丁与保存版本逐字节一致。
旧 stash `6f002a3` 未再次 apply。组合代码重新构建并执行：

- 45 项真实集群专项通过，86.968s。
- 原生模块 1 项 regress + 5 项 isolation 通过。
- 动态关闭、开启 check-world 各 81 组/666 项报告检查通过，退出码均为 0；TAP 未启用。
- pgindent 检查与 `git diff --check` 通过。

日志 `build/v3-shrink-91246-{build,module,lifecycle,native}.log` 与
`build/check-world-v3-shrink-91246-{off,on}.log`。主仓绑定最终提交后的准确 SHA
重编/真实专项结果在主仓文档另记，不能提前宣称已经执行。

独立未含 DSGA 的维护版本还额外运行了启用 TAP 的 check-world，整体未通过，
8 个文件失败；原因包含 Neon rmgr 预加载被 fixture 覆盖、序列 WAL 语义差异及
尚未定位的恢复/逻辑槽问题。详细清单在主仓 `docs/DSGA-TAP基线验证-20261020.md`。
它们不是“只能 Linux 验证”的问题，后续继续处理，不能据本批基础回归通过发布生产。

## 仍须完成的门槛

正式 capabilities/request/status/cancel/receipt/history、认证、全部扩展引用与长期扫描活性、
完整非 Buffer 预算/后台准入、Compute/Monitor/Agent/Scheduler/Controller/Runner，
以及 PG14–18 各版适配均不由本模块通过代替。Linux 实际物理回收、HugeTLB、KVM/QMP、
迁移、完整 Neon 存储链、目标性能与长稳按 V3 G2–G5 后验。仍非生产可用发行。
