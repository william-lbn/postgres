# 2026-10-21 PG17 上游维护同步

本次从DSGA功能6b7c3655合并完整社区REL_17_STABLE至
`467bb7ea7247d2657470bb8fba1d306d8cf7a57d`，新增一条官方提交，非挑选部分补丁。
Neon官方REL_17_STABLE_neon仍为`56692dfb680281a963c7470fc7f0fec7f65ecfd4`，
已包含。保留自有Neon与DSGA功能，不合自有main。

官方修复把`IS DOCUMENT`视为后缀测试，避免在另一个操作符/后缀测试中反编译
时遗漏必要括号，以及由此导致dump/restore重新解析失败或语义变化；同时取
唯一实参而非整个参数列表。官方ruleutils实现、作者说明与回归原样保留，合并
无冲突，没有另写一套中文替代算法，也没有改DSGA布局/锁/预算/Provider。

候选在Mac原生断言/调试PG17中完整重编安装，核心回归225/225通过，包含新增
create_view用例。日志为工作区build/pg17-upstream-20261021-candidate-{build,
regress}.log。候选构建仍显示合并前HEAD，不能据其版本字符串宣称准确新提交
二进制。合并提交后必须独立按准确SHA重编，再由Neon主仓将gitlink及两份锁
一并更新，执行实际DSGA/SQL/Go链路及相应验证，结果写入主仓恢复记录。

此维护修复不代表完整弹性功能完成；中心凭据/Agent入口、完整预算/Monitor/
Runner、多PG版本等继续实施。Linux/KVM/HugeTLB物理回收与最终性能验收后验；
不构建镜像、不准备部署、不发布生产。
