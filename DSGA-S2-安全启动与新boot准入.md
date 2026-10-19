# S2：安全 bootstrap、共享布局预算与新 boot 准入

更新：2026-10-20（Asia/Shanghai）。这是 PG17/Neon V3 的候选子模块，
不是完整在线缩容或生产认证。动态开关默认关闭。

## 约束与实现

1. 本次 Postmaster 的 `shared_buffers` 始终是最低合法 Guest 可容纳的
   bootstrap；运行时 Grow 只修改共享状态，不写回启动配置。内部 crash recovery
   沿用该值重新初始化，不能重新建立先前较大的 H。
2. `dynamic_shared_buffers_startup_budget` 是明确声明的 PG 共享布局预算，
   单位 kB；它不代表 Guest 总量，也不限制查询/维护/扩展的私有内存。
   `CalculateShmemSize` 在收集完 preload 扩展请求之后核对主段、固定 M hash/目录
   及四个独立数组的实际 bootstrap 元素尺寸、页界成本；固定项只计一次。
   主段按普通页/可能成功的 huge_pages=try 取整上界计费，mmap 的 SysV 身份页另计。
   Linux 主段巨页不是四数组 HugeTLB Provider 的实现证明。
3. 启用 `dynamic_shared_buffers_require_admission` 必须提供正预算。新共享内存
   boot 将普通工作准入初始化为关闭、revision=0；内部 PG 超级用户保留管理通道。
   同一 Postmaster 重建共享内存也换 boot、回到小池、重新关闭准入。
4. 简单 Query、Parse、Bind、Execute 和 FastPath 在执行入口检查准入；简单 Query
   的每条语句再查，覆盖同包 `SET ROLE`。权限使用 PG 随角色切换维护的内部只读标志，
   不在尚未开始事务的消息入口访问系统表。拒绝是 55000 ERROR，连接可以恢复使用。
   Sync/Close/Terminate 保留原生协议清理语义。
5. 管理核对需要当前 boot、非零且递增的准入 revision、当前 capacity generation；
   相同 revision/相同状态可重试，过期或冲突请求拒绝。普通角色无权开门。
   这里仍是内核协调 API，正式请求身份、回执/历史、认证和跨系统预算链由 S4 实施。

## Neon 接入约定

ComputeSpec 的可选 `dynamic_shared_buffers` 字段包含协议版本、bootstrap/max bytes、
最低 Guest bytes、明确 non-buffer reserve bytes。缺省保持官方静态路径。
Rust 检查单位、范围、溢出、预算向下取整；PG 根据真实结构和扩展布局作最终拒绝。
Compute 将受管值同时写配置和传给 Postmaster `-c`，防止恢复来的 auto.conf/include
覆盖安全值。运行中重配/配置刷新不能改变启动契约；需要受控重启。

该门禁不是 R02 的全部运行时排空：仍需已执行事务、游标、并行/DSM、维护和扩展工作
的可执行预算约束与外层生命周期协议。当前不能启用自动 VM 缩容，也不能把始终关闭
准入当作完整功能。S3/S4 尚待继续实施。

## Mac 已执行验证

环境：macOS arm64；Xcode clang、MacOSX14.4 SDK；debug/assert/depend/injection-points；
真实共享 mmap、Unix socket、独立临时 initdb，加载配套 test_dsg_region 与 neon_rmgr。

- 全部真实集群用例 29 项通过，59.600s。新增 6 项覆盖预算不足、严格模式缺预算、
  命令行优先于 auto.conf/include、多语句角色切换、简单/扩展/FastPath 的同连接
  拒绝与恢复，以及真实 Backend SIGKILL 后同 Postmaster 新 boot 小池/准入重置、
  旧 boot 许可拒绝和 1000 行持久数据校验。没有伪造 boot 或修改共享许可字段。
- Rust compute_api 12 项通过；compute_tools 全包 22 项通过（9 个库、3 个二进制、
  4 个配置集成、6 个帮助函数）；零项目标不计入业务通过数。
- 动态关闭/开启 check-world 各 81 组、665 项报告检查通过，零 not ok。
  TAP 尚未启用，本机缺少 IPC::Run；这些套件未执行，不计入通过范围。
- 日志：工作区 `build/v3-bootstrap-{lifecycle-final,compute-api-test-final,compute-tools-full}.log`
  与 `build/check-world-v3-bootstrap-{off,on}.log`。Git 提交绑定最终源码；主仓还须更新
  gitlink/两份锁，重新核对完整二进制 SHA 并实跑集成。

## 必须后验

Linux 实际小 VM 内部恢复、真实 preload 扩展成本、普通页物理回收/巨页计费、
Neon Pageserver/Safekeeper、完整准入/后台预算与 Monitor/Agent/Controller/Runner，
以及 KVM/K8s 故障/性能均未被上述 Mac 结果证明。按 V3 G2–G5 单独签收。
