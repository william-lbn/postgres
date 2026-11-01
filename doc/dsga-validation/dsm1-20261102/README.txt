PG18 DSM1：真实外部payload分配前预留与生命周期独立模块

基线3d44746，原codex/dsga-pg18-v1-20261031。9生产+4测试，不改PG17、gitlink、发行锁或
公共上层合同。新增DSM1快照嵌Prepared1/Connection1/Work2，既有三者和Buffer ABI6结构不变。
固定登记表计入原启动共享段；原DSM控制段格式不改，真实maxitems由唯一容量函数导出。
原18 DSM与去掉17 DSG增量后除版权一致，逐hunk移植且保18 native源/版权，非整文件替换；
三注入点按真实18双参数(name,NULL)，不误拷尾部未授权函数。

正数GUC dynamic_shared_buffers_dsm_budget为本boot持续外部payload硬额，原17同名/默认0/
kB/0..INT_MAX；0只观察，不签硬cap。正数缺native或standalone/不支持实现直接启动拒绝。
仅真实enable&&IsUnderPostmaster fork backend参与跟踪；wrapper/creating/after-create和两处
publish同谓词，PM/early child控制段附加/清理保持原无PGPROC、无backend锁路径。零额
standalone保原DSM行为但accounting_valid=false，不假报有效观察。

backend外部CREATE在原OS调用前checked页舍入预留，pending也占cap；真实created尚未
publish持续占，只有原native控制发布才转resident。原3实现CREATE false在ERROR级只代表
EEXIST无本次效果（SYSV保留IPC_PRIVATE也无OS动作，原candidate0跳过），其余错误抛出；
只撤本次撞名预留、不销毁原对象。OS开始后异常/失败DETACH或DESTROY转unknown保留费用；
即便unlink成功，mapping未知也不得释放。真DESTROY且mapping生命周期可解释才扣费。
固定DynamicSharedMemoryControlLock→status_lock，快照只status不反锁，同点复制工作/
门禁/两阶段/DSM事实。主段DSM已在固定主段预算，外部不重复计费；控制段upper另独立计费。
不覆盖PTE/内核/THP/Host，不由本值签跨boot Root Held/Original或normal许可。

最终9稳定目标=1U-native+8S，5子例(3方法+2错误点)，零skip，只新改相关功能：默认off/
原session ACL/ABI/0未强制/正数错误模式拒；POSIX、SYSV、MMAP实际页舍入/总cap严格超额
拒及真实OS O_EXCL碰撞原payload保持/新candidate重试；OS前暂停pending占额并发第二创建
被拒；pin跨创建者退出保留，另一真实backend attach不双计，真末次unpin/destroy回基线；
OS前/OS后未publish注入ERROR由原ResourceOwner真实清理；真destroy错误后unknown保留
并拒新超额；main2MiB只固定主段计费；同PM真实crash/reinit清exact已发布旧payload而保持
新boot关闭；新增zero-cap standalone实际DSM无PANIC、正数standalone启动拒。
碰撞fixture以原dsm_impl_op创建精确自有对象，原payload不变且仅精确清理自身；它明确是
非Source测试夹具，不以绕开包装的该对象来声称产品预算或任意C代码强制隔离。

首78005a build通过91.145秒，195473八根全失败保留：测试错误假定DSM启动为0，把原生
必要保留排除在8/16/64KiB cap外。实际startup PID133在恢复统计时真实FATAL budget exhausted，
0额单位根发现raw1MiB而create8KiB后raw1,056,768。没有豁免原生分配或放宽core。具体链为
StartupXLOG→pgstat_restore_stats→pgstat_read_statsfile→共享pgstat DSA增长→dsm_create；
原stats in-place256KiB，原DSA默认首个外部segment1MiB。新测试逐cluster直接dump真实
pgStatLocal.dsa，总area必须1,310,720且snapshot外部1MiB/1segment/unknown0；不动态学习
未知占用。总cap固定为已证明1MiB+8/16/64KiB，始终核原始总reserved<=实际正cap、页舍入
和OS前并发超额；不是只减baseline后遗忘总额。main2MiB承载原统计和小payload，实际external
基线0并验证无双计费；不得拿该配置推断普遍DSM零占用。

独立审查同时发现新hook enable-only publish与非fork wrapper bypass不一致的静态PANIC
路径；按Root批准只统一跟踪谓词并新增直接standalone反例，不把问题藏成不支持零额。
新d071f3 build通过44.080秒，PG7dfec415bfcfc4297392142a85c44e099c2dcfacead29f5cd159abb837827687、
DSOcada2bc8f6b62de9e92be5b3863c9603dc2ef099ae53966d1e27b16c065c5b49。自动4ef00f九根
8PASS，唯一driver仍错误预测crash新boot为0，实际重新建立原统计DSA1MiB。保原失败，
未改C；12编译/安装输入与ELF保持，除该方法外Python AST全同，只重跑该根b15797，通过
0.890秒。旧八PASS不重复，最终稳定ID去重替换，不累加失败/重跑次数充测试数。

真实恢复证据：同PM21，boot14263427977892277277→17907052423067492942；旧raw1,056,768/
2segment→新raw1,048,576/1segment/unknown0且closed ack。exact旧/dev/shm/PostgreSQL.
2529522698消失；新1MiB对象221848286、control1427749280=13776B；新backend66直接stats
area dump仍1,310,720。不是凭总额相同推断旧对象已释放，只证明此确切已知发布对象。
初始化CHECKPOINT沿原RM134夹具边界，不声称任意新增heap WAL恢复或HA完成。

所有原始失败/旧source/driver、命令nonce/spec/unit终态、实际ELF/锁和cpu.stat/memory.peak/
io.stat/memory.events保留，无binary入Git、不prune。原通用runner少数scope文本继承PM_GATE
模板，准确范围以本目录catalog/源码锁与实际DSM目标为准，不据此冒PM或normal验收。
独立最终13源审查无新明确阻断，全文和原native调用点列表一并归档，未重复运行测试。
原PG17相同standalone边界仅在本批只读审计，不修改/测试PG17；其独立复现与同步修复由
Root另分所有者推进，不纳本PG18实测计数。

剩余：MemoryContext工作域硬预算、任意orphan/crossboot Root Held完整证明、PG18完整
Root绑定Source/force/I/持续normal安全降限、生产enable/max配置、完整Neon新heap WAL扩展、
PG17 Linux与三worker真实Helm K8s端到端。所有DSM计数/硬额仅本boot该域事实，不替代上述
条件或VM许可；本批Source=false、normal=false、E=0。
