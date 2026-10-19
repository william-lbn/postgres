# DSGA M2d：真实扩容、Granule 目录与请求重放

日期：2026-10-19。基于 M2c `d792ba944b0c3c27bce84d912827f7d752bdd1e0`。
本批实现普通页 Buffer Adapter 的真实 Grow 内核路径；仍不实现 Shrink、物理释放、
HugeTLB、Monitor/Agent/VM 和生产 SQL 控制入口，不能据此宣称完整功能完成。

## 实际源码及顺序

源码仍在 Neon 的 `vendor/postgres-v17`，不是另建独立数据库。新增
`src/backend/storage/buffer/dsg_buffer_grow.c`，由 Make 与 Meson 接入。

1. 按 PG boot 身份、单调 revision、128 位 operation ID、预期 generation 校验。
   同一请求返回现存结果；同 ID 改参数、旧 revision/boot/generation 均拒绝。
2. 取得独立 resize 协调锁，不影响普通 Buffer Hash 命中。永久共享目录记录原 H、
   目标、阶段、拒绝原因；状态不随 SQL 事务回滚，最新操作之外的旧重试会被拒绝。
3. 四数组分别按 Granule 分批预提交；系统调用前先登记保守占用，失败或中断不扣除。
   尚未发布的分配失败可留下额外成本，不能靠“Grow 被拒绝”把预算归零。
4. 初始化新描述符、header state、content lock、IOCV 及 freelist 链。整个阶段
   位于数组独占锁外，已有 Buffer 继续工作，新增 ID 不可被选择器取到。
5. 可取消、按 monotonic 时钟限时等待数组独占保护；取得后仅追加 freelist、发布
   Granule 与 H/L/generation/结果。先 H 后 L，发布区间不分配、不 IO、不等待。

这条路径没有调用“强制清 pin”或绕过 Neon smgr/WAL。Grow 不能批准 VM 缩容。
外层调用者还必须先证明 Host/VM/Guest 实际资源已增加，并核验外层身份和预算。

## 锁、目录和生命周期

- 锁序为 resize → access → strategy/status。不允许持读侧 Guard 升级到 Grow，
  不允许协调器递归调用自身；外围适配仍须遵守不得带入 mapping/header 锁的纪律。
- resize 锁在协调期间允许取消；和 M2c Guard 一样成对平衡原生 interrupt holdoff。
  条件取锁配合 Latch，协调锁/发布锁竞争均有超时，等待可取消。
- status spinlock 只保护固定大小状态复制，不执行 Provider、分配、等待或 ERROR。
  Granule 长目录受 resize 锁保护，不能拿 status spinlock 遍历整个 M。
- Granule 初值为每段 2048 个 Buffer（16 MiB 数据页），首尾允许部分有效。目录在
  永不回收的主共享段；`active_buffers` 和 `prepared_buffers` 分开。RESERVED 只说明
  逻辑未发布，绝不表示已获得物理释放证明。Granule 大小可配置和 Shrink 状态后续补齐。
- 普通页映射在 fork 前覆盖全部 M，Grow 不改变各进程地址/映射身份；本批不需要
  重新 attach，尚未实现后续 Shrink 所需的 barrier/排水协议。

ERROR/FATAL 由 `PG_ENSURE_ERROR_CLEANUP` 处理：发布前改为 DENIED、恢复待发布目录，
保留预算；发布后保留 COMMITTED，不能因为响应丢失而把共享状态当作 SQL 事务回滚。
SIGKILL 由 PG 的 Postmaster 崩溃恢复重建整个共享内存，新 boot 使旧请求失效。

## 未知成本与较小重试

每个数组独立记录按 Provider 页向上对齐的 `committed_upper_bytes` 和
`allocation_unknown`。它们是保守预算，不是 RSS，也不是精确物理驻留量。
在系统调用可能部分成功、或中断发生在调用前后时，保留未知状态。较小 Grow 成功
不能抹掉先前较大失败请求的尾部；只有连续预提交覆盖全部未知尾部，才清除该数组的
unknown。逻辑 COMMITTED 可能同时存在未知成本，VM 层仍必须保守拒绝缩容。

## 本地业务验证

测试通过真实 `DsgBufferGrow` 调用，未直接写 H/L 或伪造 Provider 结果：

- 同一 Backend 从 2 MiB 扩到 4 MiB，再跨 Granule 扩到 18 MiB；普通 SQL 写读
  3000 行、3072000 字节 payload，确认新增 Buffer ID 实际被 BufferAlloc 使用。
- 相同请求重放不重复扩容；同 ID 改参、旧 revision/boot/generation、超 M 和减小
  目标均拒绝；同容量请求不增加 generation；SQL ROLLBACK 不回退已提交容量。
- 真实发布前暂停时其他 Backend 仍看到旧 H，竞争协调者超时；取消后容量不变，
  已预提交的尾部预算仍保留，失败请求可重放。
- 暂停真实 pg_buffercache 数组扫描，Grow 发布等待超时并拒绝；扫描结束后用新请求
  正常扩容，不能为完成扩容而越过读侧保护。
- 注入 Provider ERROR 后保留未知预算；较小重试成功仍保留未知，更大覆盖后才清除。
- 发布后注入 ERROR/终止响应连接，检查共享 COMMITTED 保留，另一 Backend 幂等重放。

模块回归 1 组、隔离测试 3 组全部通过。断言构建、pgindent、diff 检查通过。
重新清理并编译与候选 PG 头文件匹配的真实 neon_rmgr 后，关闭/开启两种配置的
check-world **各 81 组、664 项报告检查通过**；当前构建未启用 TAP，不能计为 TAP 通过。
日志为工作区 `build/dsg-grow-module.log`、`build/check-world-dsg-grow-off.log` 和
`build/check-world-dsg-grow-on.log`。Neon 主仓新候选 SHA 的构建及独立集群验证另记于
主仓 `docs/DSGA-M2-主仓集成验证记录.md`，不把尚未运行的主仓用例计入本记录。

Linux 预提交/物理释放、HugeTLB/KVM、完整存储端到端与性能仍未验收。
