# DSGA M2c：数组访问、旧提示、替换器与中断清理

日期：2026-10-19。功能分支：`codex/dsga-pg17-20261019`。本批仍未开放在线 resize；
目的是先使实际 PG/Neon Buffer 路径具备回收前所需的访问保护，不能将本批视为
完整 DSGA 或 VM 内存在线伸缩已经交付。

## 修改的路径与保护责任

| 路径 | 生命周期保护 | 原有语义保护 |
|---|---|---|
| 普通 Hash 命中重新 pin | 原有 mapping 锁直到取得 pin；不加数组读锁 | mapping/header/pin 协议 |
| victim/策略环选择 | 取得数组 Guard，选择到成功 pin 后立即释放 | Strategy spinlock/header/pin |
| freelist 归还 | Guard 内先按稳定地址算索引，再检查 H/L，才解引用 | Strategy spinlock |
| 旧 recent-buffer 提示 | 私有 pin 保护已有引用；否则 Guard 内检查 H 再查看描述符 | tag 再校验及 header/pin |
| checkpoint | Guard 覆盖扫描、排序 workspace 与保存的旧 Buffer ID 使用期 | 原有 WAL/dirty/flush 协议 |
| BG writer | Guard 覆盖数组访问；按 L 扫描并在 L 改变后重置旧统计 | 原有刷脏和 replacement 协议 |
| Drop/Flush 的全数组扫描 | 实际扫描前才取得 Guard，早返回路径不持锁 | 原有关系锁、mapping/header/pin |
| pg_buffercache/autoprewarm | 在 Guard 内复制私有快照，复制完即释放 | 原有 header/state 检查 |

Hash 重新 pin 的安全性依赖回收者在原 mapping/header 协议下移除 tag 并再次检查
refcount/dirty/IO；generation ACK 不会替代该协议。本批没有强制清 pin、没有绕过
Neon 的 smgr/WAL/dirty-page 路径。

## 后续发布者必须遵守的锁序

- L/H 的后续改变须在数组独占保护下发布，不能让正在选择 victim 的读者跨越边界变化。
- 取得数组独占保护前不得持有 mapping/header/Strategy 锁；取得独占后只做短的发布
  和策略边界处理，不在其中等待 IO、pin 排水或进程 barrier。
- 需要等待读者的协调者须采用可取消、可超时的独占取得方式。读者自身的 tag/dirty
  等语义仍由原 PG 锁负责，不能把数组 Guard 当成页面内容锁。
- checkpoint 可能长时间持有读侧 Guard；后续缩容可等待或拒绝，不能为了快速完成
  而释放其 workspace。性能影响仍须单独验收。

## 真实测试暴露的两个问题

1. **旧提示范围检查前的私有 pin 查询也会断言。** 原 `GetPrivateRefCount` 会按当前
   Buffer 有效范围断言。纯私有键查询现在只要求固定 M 范围；真正共享访问继续要求
   pin 或 Guard 内 H 范围。`xlogutils` 也不再先对可过期提示做当前 H 的有效性断言。
2. **直接长持原生 LWLock 会屏蔽取消。** LWLockAcquire 增加 interrupt holdoff。只读
   数组生命周期保护不能因此让 query cancel/FATAL 一直延迟。Guard Begin 只抵消
   本锁增加的 holdoff，End 成对恢复后再释放；其他 PG 锁的中断保护保持原值。
   原生 `LWLockReleaseAll` 会自己平衡清理调用，并保持错误恢复设置的 holdoff。
   已对照 `lwlock.c`、`xact.c`、`ipc.c` 的真实实现，并执行 ERROR/取消/FATAL 验证。

## 本地验证项目

- 真实 C 模块拒绝 H+1 和 INT_MAX 的旧提示，覆盖尚未提交的预留尾部及远超 M 的输入。
- SQL ERROR 发生在 Guard 持有期间，后续独占试锁成功；保留已有嵌套 Guard 和真实
  双 Backend 共享映射测试。
- 新隔离测试通过故障注入暂停**真实 pg_buffercache 扫描**，另一 Backend 独占试锁
  必须失败；正常唤醒、query cancel、连接 FATAL 终止三种情况结束后，独占试锁均成功。
  终止排列置最后，避免测试器在已经终止的连接上继续执行。
- 模块回归 1 组、隔离测试 2 组通过；新增隔离测试含上述 3 种实际排列。
- 断言构建、pgindent、diff 检查通过。完整开启/关闭两种配置的 check-world
  **分别通过 81 组、663 项报告检查**，日志 `build/check-world-dsg-access-on.log`
  与 `build/check-world-dsg-access-off.log`。每种配置的数字单独统计，不合计冒充覆盖率。

当前构建未启用 TAP，不将 check-world 报告数字解释为 TAP 通过。Linux 物理回收、
HugeTLB/KVM、完整 Neon 存储端到端和性能门槛仍未验收。

## 有意保留的 NBuffers

`NBuffers` 继续代表启动 GUC，不能批量替换。buf_init 初始化前缀、SLRU/WAL buffers
和 checkpointer 固定队列等启动期分配仍据各自既有规则设置；动态适配只针对 Buffer
Pool。在线容量统计/算法阈值改读 H 或 L。Neon 外围适配及动态启动预算仍是后续批次，
没有完成这些适配前不得开放线上扩缩入口。
