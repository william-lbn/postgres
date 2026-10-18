# DSGA M2a：普通页 Provider 与 Mac 本地验证

日期：2026-10-18。提交范围仅为共享映射 Provider；尚未接入 Buffer Manager，
不代表在线 Buffer 扩缩容已经完成。默认 PG 行为不变。

## 实现及不变量

- 在 Postmaster fork 前预留 `MAP_SHARED | MAP_ANONYMOUS` 稳定地址；子进程继承
  同一 backing。不做各 Backend 私有 remap，不声称支持 EXEC_BACKEND/HugeTLB。
- 访问上限与按系统页向上对齐的映射长度分开记录；用减法校验范围，拒绝溢出。
- Populate 只用于尚未发布的区间。Linux 可用时调用 `MADV_POPULATE_WRITE`，失败
  明确标记可能部分分配；Mac 对真实映射触页，不改动区间外仍活跃的前缀对象。
- Release 按完整页向内取整，保留两端边界页。只有实际 `MADV_REMOVE` 成功才返回
  对应区间的物理回收证明；Mac 返回 UNSUPPORTED，保持页面原值与零回收证明。
- Destroy 只解除调用进程映射，不能当成多进程物理释放证明。正常组件排水、
  pin/IO/tag 安全以及 VM 缩容许可都由上层负责。
- 测试扩展的管理函数同时采用 SQL REVOKE 与 C superuser 校验。错误清理确保
  测试错误路径不泄漏本进程映射。

## 已执行的本地验证

平台：Mac arm64；PG17.11 + Neon，断言、debug、injection points 构建。
编译器/SDK 与 M0/M1 相同，构建目录 `build/pg17`。

1. 真实临时 Postgres 回归：12 个范围、对齐、前缀保护、重复预留/销毁、
   无映射与溢出场景，全部通过。
2. 隔离测试：两个分别 fork 的 Backend 先后写入 17、34，对方读取值均匹配。
   测试映射在 Postmaster preload 时创建，证明的是真实跨进程共享。
3. 加载配套 Neon rmgr 后运行完整 `check-world`：81 组摘要、662 项报告检查，
   全部通过；包含原有 PG regress 225、isolation 123、test_decoding 19+15。
4. pgindent 检查通过。Makefile、Meson 均加入新模块；Windows 不运行 Unix mmap
   测试，API 在不支持的预留路径明确报 FEATURE_NOT_SUPPORTED。

可复现入口（从工作区根目录执行）：

```sh
make -C build/pg17 -j4
make -C build/pg17/src/test/modules/test_dsg_region check
bash repos/neon/ci/test-postgres.sh build/pg17 build/pg17-install
```

完整日志：`build/pg17/check-world-dsg-provider.log`。
当前配置未启用 TAP，不能把上述数字解读为 TAP 全通过。

## 尚未验证/尚未实现

Linux syscall 的真实成功、ENOMEM 部分分配、跨进程 MADV_REMOVE、HugeTLB 与
Host 资源归还均等待后续 Linux 环境验收。Mac 的拒绝路径不能替代这些测试。
Buffer Manager、Granule 状态、协调操作、预算协议将在后续独立批次接入。
