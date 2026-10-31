PG18 最早原生AS/R小批（2026-11-01，默认关闭，非完整NativeFinal/normal）

原始问题：先前18四数组在PG内存系统初始化后才预留VA，logger的默认脱离会腾空VA。此批把原生hard=soft RLIMIT_AS/真实NNP与未提权检查接到main最早，再以准确18编译元素大小checked预留四PROT_NONE R；首次共享初始化只导入一次原R，后继Park/Rearm保持同VA/几何；early模式logger等非共享子进程Park弃旧backing但保R。原AIO/completion末访问函数/IO worker均保留。

统一合同：沿Birth ABI1的原字段/四数组；NEON_DSGA_EARLY_ARRAYS_PROFILE=test-v1:M为统一输入。旧18私有TEST_ONLY env仍兼容，但同时存在必须匹配原M，不能覆盖它。AS原生纯早期解析/读回先于get_progname/startup_hacks/save_ps_display_args/MemoryContext、配置和插件。该AS/R本身不是Source/持续物理包络，source_bound/root_enrolled仍false。root-v1在完整18filter/198/ptrace最终停点前明确拒绝，不能借事实造资格。未改任何17gitlink/发行锁/公共wire。

来源：Linux18固定3d6ac6f0+独占9源；17代码只从已push b95b1421 Git对象读取。独立只读/src准确7377输入全部实际hash OK。实际GNU12+固定68c6工具，4CPU5GiB/禁网/1GiB tmpfs、原ccache、私有复制build输出；无prune/Host网络/Node/K8s修改。PG18原头c.h已把17 pg_attribute_noreturn()改pg_noreturn，首个真实compile2日志保留，只适配新EnvelopeC该声明后build/source全0，64.95秒、零编译warning。
实际PG ELF91de676e56c3ca200afb5dc4e986f3e0dd3679ec437fb4683e42ee45278d6d31，测试扩展a29daf9536505757ac5ce2e1fcb80d00b8257462def58fba290c9800fe7ce0eb。generic TEST_ONLY extra-version固定，源身份仍以精确base/7377manifest/9hash为准，不靠version字符串猜源码。

验证：同binary原始41根真实PASS/零skip：新增2U4S+旧Root4+原region/arrays/resize/failure30+精确uring1。新根实际检查无profile真实PG、canonical/overflow/页不齐/R>=AS/缺AS/root-v1拒、NNP0拒、originalM旧env冲突；原PM/后台连接/logger/两IO worker真实AS2GiB与NNP1、共享R/parked R；logger重启和三次真实共享恢复保原R/AS/数据，旧backing不留logger；新接口默认REVOKE和原current/session管理员校验。
随后仅U001增加真实root0+NNP1负控，8生产/测试API源、ELF和其余方法完全不改；该根新源精确重跑1PASS/外层0，其他40保同binary原真收据和逐方法hash相等证明，不笼统说最终Python全体重跑。稳定新增仍6根，旧35不计新增/E0。

额外失败必须按真实事实理解：preexec设ruid1000/euid0/NNP1并不意味着PG入口仍混合UID。新增测试最初误要求其拒绝，真实--version返回0，故该FAIL保留。独立同PG真实诊断通过pipe记录preexec [1000,0,0]、NNP1，exec后/proc真实Uid四项全1000、Gid全1000、NNP1、AS2GiB、PG ready，root_enrolled仍false。不能把已降权安全入口误判特权绕过，也不能据这个样本保证所有内核/构造器路径。仅删除失真负向假设，保真实root0拒绝，C的root/ruid!=euid/NNP门禁一字未放宽。该诊断不算新U/S或Root资格，原FAIL/诊断/最终PASS分档保存。

边界：本批没有seccomp范围保护、可信fork/childReady登记、198握手/Root独立复核与WAL停点，也没有原Source/物理K/I/FS连续资格、normal/VM许可。AS+R事实不能替代实际PG工作域normal硬降限、无新增OOM持续证明。tmpfs/initdb --no-sync只是PG协议/机制夹具，不是磁盘断电持久验证。KVM和三节点K8s未跑。

下一批：保18PMChild实际4处pid落位才清pending，CreateSharedMemory完成后且maybe_adjust_io_workers之前结束重建门；保B_IO_WORKER和completion/access/resize锁序。完整filter/198/Root最终停点沿同中性ABI1。原安全deny io_uring_setup/enter/register不删；default-off老uring保留，受保护18先sync/worker，uring须独立I/Source资格后才可支持。9源本批freeze由Root独审/统一提交。
