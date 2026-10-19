# DSGA M2e：完整尾部回收与末页填充边界

日期：2026-10-19。初始开发基于 M2d `a3bc37375ea337d041e716c243fa172274b091f4`；
持续同步发现官方新修复后，先完成独立维护验证及功能合并，最终基于
`823644b93da60235465b4875259817296a04fccf`，已包含社区维护头
`94035972af3f10cdaf9ff2b5d232ad5f45463469`。
本批只是 Provider 能力，不会直接缩小 Buffer Pool，也不产生 VM 缩容许可。

## 为什么需要单独接口

普通 `DsgRegionRelease(offset, bytes)` 保留请求两端的半页，保护可能与其他活跃对象
共页的数据。如果 M 的末端不满一页，按该接口回收尾部会留下末页，即使末页内的全部
逻辑对象已经排水，也不能直接把成本记为只剩 H 的前缀。

新增 `DsgRegionReleaseTail(region, offset)`，调用者须证明 `[offset, M)` 全部排水。
它仍把起始边界向上对齐，保留与活跃前缀共用的页；末端则使用这个独立 mmap 实际拥有的
`mapped_bytes`。M 之后的 padding 不能放其他对象，因而可以纳入完整尾部回收。
普通 Release 的向内对齐契约保持原样，不能将 Tail 用于中间空洞。

回执的 requested 是逻辑区间，affected 是实际完整页范围，可以因末页 padding 大于
requested；只有内核实际回收成功才设置 physical_release_verified。失败新增
release_may_be_partial，不能把系统调用错误当成“整个区间均未变化”或扣除原预算。
不支持时返回 UNSUPPORTED，内容保留、affected 为零、物理证明为 false。

## 边界与验证

- 请求前先检查 offset，再做 M-offset 减法，避免越界绕回；已销毁 Region 拒绝操作。
- 对 M=3页+1字节的真实共享映射，先执行普通 Release，检查末页合法字节仍在；
  再执行 Tail，检查活跃起始边界保持不变。Linux 回收分支须检查完整 affected 页区间及
  逻辑尾部清零；Mac 分支检查两次均明确拒绝且原内容保留，不伪造物理证明。
- offset=M 是空尾部，不能丢弃仍含最后一个活跃字节的末页。
- 保留已有映射继承、真实 Grow、取消、幂等和恢复测试，避免只测试新 helper。

Mac 断言构建、模块回归 1 组及隔离测试 3 组通过，包含已有真实 Grow/取消/错误恢复。
pgindent、diff 检查通过。在旧 a3bc 基线完整开/关各 81 组/664 项通过后，发现新官方
修复，先独立同步验证，再以最终 823644 基线重新构建并运行完整开/关回归：
**各 81 组、664 项报告检查通过**。没有混用旧基线结果；当前构建未启用 TAP。

最终日志：工作区 `build/dsg-tail-latest-build.log`、
`build/check-world-dsg-tail-latest-off.log`、`build/check-world-dsg-tail-latest-on.log`。
主仓依赖锁和新候选二进制的独立集群恢复另见 Neon 主仓集成记录。
Linux MADV_REMOVE、HugeTLB/KVM 与 Host 实际回收仍按用户要求后续实测；Mac 通过不等同于
这些平台已经验收。完整 Shrink/排水/barrier 和跨层缩容许可尚未实现。
