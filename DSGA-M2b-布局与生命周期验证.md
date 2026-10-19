# DSGA M2b：接入 Neon PG17 Buffer Pool 的初始布局

日期：2026-10-19。状态：本地模块验证通过；没有在线 resize 入口，不代表整个
Neon 伸缩功能已交付。需由 Neon 主仓 `vendor/postgres-v17` gitlink 引用本提交。

## 修改范围

- `buf_init.c` 在实验开关开启时通过真实 DsgRegion 管理 BufferBlocks、Descriptor、
  IOCV、checkpoint workspace 四数组。按 M 预留地址，只 populate 启动容量。
- `dynamic_shared_buffers.c/.h` 在不可回收的主共享内存建立容量目录、L/H/M、
  每次共享内存重建的新 boot 身份和读侧 Guard；Hash 固定按 M 预算。
- `enable_dynamic_shared_buffers` 默认关闭；`max_shared_buffers=0` 表示启动初值。
  初期仅支持 fork 继承的普通页映射。EXEC_BACKEND/Windows/显式 HugeTLB 拒绝。
- 主共享段的 `shared_memory_size` 不含四个独立映射；每数组目录记录预留长度和
  保守提交预算，不将该预算冒充 RSS 或物理回收证明。
- Postmaster `on_shmem_exit` 在主段销毁前解除额外映射并清空目录指针，适配同一
  Postmaster 在 Backend crash 后重建共享内存的流程。

## 实际发现与修复

首轮集成暴露：Buffer Pool 初始化早于 injection point 共享目录，早期执行注入
钩子会读空目录。Provider 现在仅在 Backend 阶段执行钩子，Postmaster 初始化不调用。

`sysviews` 原测试记录 planner enable 参数，新 Postmaster 开关不属于该集合。
查询限定 `context='user'`，保留原有 25 项用户级规划/预取开关及其预期值。

## Mac 本地结果

- 动态布局开启：完整 `check-world` 81 组/662 项报告检查通过。
- 默认关闭：完整 `check-world` 81 组/662 项报告检查通过。
- 模块验证：真实目录/四数组指针、预留 M/初始预算、末尾描述符初始化、嵌套 Guard
  释放责任通过；保留 12 个 Provider 场景和双 Backend 共享隔离测试。
- Neon `ci/test_dsg_postgres.py`：4 个真实集群测试通过，包含同一 Postmaster 连续
  两次 Backend SIGKILL 后重建、boot 身份改变、2 万行数据恢复、B-tree amcheck、
  最大值不足/巨页配置拒绝、关闭时原路径兼容。恢复使用配套 neon_rmgr，并开启
  `wal_consistency_checking=neon`，不是普通 PG WAL 的替代测试。
- pgindent、Ruff、diff 检查通过；当前 PG 构建未启用 TAP，不宣称 TAP 通过。

日志：`build/pg17/check-world-dsg-layout-on.log`、`check-world-dsg-layout-off.log`、
`dsg-layout-lifecycle.log`。原 M0/M1 记录说明编译器和 SDK 配置。

后续先完成所有遍历/策略/提示访问边界，再开放受限在线扩缩。Neon Monitor、
Compute 启动预算、Autoscaling/NeonVM fencing、Linux/HugeTLB 验收均尚未完成。
