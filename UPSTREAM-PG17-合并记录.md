# PG17 官方维护补丁与 Neon 差异合并记录

状态：本地集成与验证中，尚未提交。动态共享缓冲池功能不包含在本批次。

## 固定输入

- 我们的 Neon PG17 基线：`56692dfb680281a963c7470fc7f0fec7f65ecfd4`。
- PostgreSQL 官方稳定分支：`65e944d92b082cc16e2265a91bab8b73934a0e74`。
- merge base：`6af885119b52a2a6229959670ba3ae5e36bf9806`。
- 更新后的版本标识：17.11；并非 PG18 接入或 PG 大版本升级。

本批次是官方维护历史合并，所以会包含相应的文档、测试和维护修复文件。
我们的功能实现另外拆分，不能将官方修复的文件数当作新功能补丁面积。

## 人工解决与额外审查的语义差异

| 文件/路径 | 冲突原因 | 合并决定 |
|---|---|---|
| hash.c | 官方增加无锁区间的 vacuum 节流，Neon 增加预取 | 同时保留，在无 buffer lock 时节流 |
| hash_xlog.c / xlogutils.c | 官方提取 init fork 刷盘函数 | 使用新函数，保留 `am_wal_redo_postgres` 不刷本地 init fork 的限制 |
| heapam.c | 官方登记 VM 页，Neon 使用自定义 WAL 与 speculative pin | 保留 Neon RMID/结构大小/事务命令字段/pin；纳入 VM block reference 与锁顺序 |
| nbtsearch.c | 官方新增空索引复查，Neon endpoint 接口多一个父页参数 | 复查同样传递父页，保持 Serializable 谓词锁修复 |
| walsender.c | Neon 拆分消息解析/位置处理，官方维护前次位置 | 将历史位置放到共用处理入口，删除已废弃的状态变量 |
| read_stream.c | 临时关系访问检查与 Neon 禁用 OS readahead 重叠 | 两者保留 |
| postgres.c | slot sync 通知与 Neon interrupt callback 重叠 | 先处理 slot sync，再执行原 Neon callback/重试 |
| pgstat.c | 引用不同复制接口头文件 | 保留所需接口，编译验证依赖 |
| injection_point.c | 官方清零 private_data；Neon 检查非空长度 | 先完整清零，再按合法长度复制 |
| injection_points.c | varatt 与测试声明头文件变化 | 保留两者 |
| encoding SQL/expected | 新增非法多字节输入回归 | 保留官方测试及预期 |

## WAL 兼容是跨仓依赖

VM block reference 会使“UPDATE 最后一个 block 总是旧 heap 页”的旧解码假设失效。
因此 Neon 主仓需同时适配：

1. Rust 解码器只从主分支选择旧 heap 页。
2. 按关系和 VM 页判断显式引用，只为未登记页生成旧格式的隐式清位元数据。
3. neon_rmgr 通过恢复 API 处理新 VM 引用与 FPI，保留旧 WAL 的隐式清位路径。
4. 同一个 VM 页上的新旧 heap 位在同一次 LSN 判定下清理。

发布时先部署能够读取新旧 WAL 的 Pageserver/redo 组件，再部署新 Compute。
单独合并 PG 写入端，不代表成套发行可以发布。

## Mac 本地验证记录

环境：macOS arm64，Apple clang 15 + 对应 MacOSX14.4 SDK；断言构建，
关闭 ICU/readline/zlib，开启 injection points。编译日志在当前开发工作区 build 下，
不把大型生成物提交到源码仓库。

- PG 核心 regress：225/225 通过。
- PG isolation：123/123 通过。
- neon_rmgr 编译：通过。
- 新增真实 crash/recovery：3/3 通过，包括跨两张 VM 页的 UPDATE。
- 加载配套 neon_rmgr 的 `check-world`：79 组、660 项报告检查全部通过。
- Rust WAL 解码模块：13/13 通过。
- 旧 PG17.8 二进制生成 WAL，新 PG17.11 二进制恢复：3/3 通过。
- PG16 共享扩展分支：编译、逻辑解码模块和 3 项实际恢复通过，详细结果在 Neon 记录中。
- 本构建未启用 TAP 测试；以上全量结论只适用于已配置的检查范围。
- Linux、Neon Pageserver 端到端、NeonVM/HugeTLB 实机：未验证。

完成本批次的本地验证后再形成提交；下一批次才加入动态 Buffer 代码。
