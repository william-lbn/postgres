PG18 Native 完整C链准确369 KVM Guest多平台证据（2026-11-01）

实际同一源码/产物：Host冻结1197c33+24精确源，PG ELF65107494edf4b376c3dbe7960247d37d84c96b2e546bd791e3d3aefe5344d530、测试DSO3ac1cab277327eb5fe75637d7bb30810d3a2e80c9ca0affbbfdaf1f1e5aa74e1。Guest实际sha256sum再次核相同，返回全部Python test输入SHA逐项等于冻结Host snapshot；未取活动未提交PG17/Mac业务源码，未复用旧PG18二进制当新产物。

真实准确内核三元组：vmlinuz3693200b、resolved config11344757、notes7cf45ced，完整hash见closure/input-lock.json；release6.12.26-william-normal-split-v1-e210f5e3ee94。两次Guest实际release/notes/PG/DSO/pid硬源512均核验。QEMU直接取既有Runner镜像的工具二进制8d8d8ee8，不运行旧Runner业务；强制-accel kvm无TCG fallback，实际QMP query-kvm enabled/present true。无NIC，Guest2CPU/2GiB、外层2CPU4GiB硬限/无额外swap，超时保护/Guest与QEMU终态独立。

最小TEST_ONLY rootdisk仅从固定68c6工具读取Python3.11.2/stdlib及130可信测试ELF实际依赖闭包，625运行文件38.7MB tar；准确本轮PG18 install/share与测试DSO、既有BusyBox672ec674/musl4e5f9d95、冻结test脚本。没有打包全tools/compute产品或正式发行镜像。保实际路径/文件sha/模式/UID/叶symlink链；父usrmerge目录按最小Guest直接目录物化，只是组件runtime，不是生产RO alias/constructor前闭包资格。ldd仅用于可信测试工具的依赖发现，绝不当生产pre-main信任证明。

两次准确执行分开保留：
1 首Guest真实QEMU0/KVM true、准确内核和产物，Root9全部PASS/零skip（真实BPF双阶段/198关闭/Root TID≠TGID/DETACH、错源和SIGCONT/EXITKILL/30s超时等）。filter组因最小Guest漏建/dev/shm，连default-off PG也POSIX共享内存ENOENT，故整体Guest1/FAIL原文保留。外部runner沿Host固定/evidence/test-result.json，两组共用导致Root9覆盖早先filter summary；filter所有失败raw run.log/console仍完整。原外层metadata错误和真实FAIL不改，只读补证按payload cohort和原始日志/实际artifact恢复Root9准确归属。
2 只补Guest init的有界256MiB /dev/shm tmpfs和devpts目录，外部runner按已有phase env隔离输出路径；断言/24源/ELF/测试方法全不改。第二Guest只精确重跑失败filter5，真实5PASS/零skip，Guest0/QEMU0/KVMtrue，24.34秒；sync/worker、两IO worker、三次恢复、ready真10s超时精确回收、protecteduring拒绝等全实际运行。没有为输出metadata重跑已PASS Root9。两stage root文件唯一差异为init和fixture/runner.py，完整hash清单和delta保留。

组合结果：准确同14个稳定逻辑根在369环境全部真实PASS（Root9来自首Guest，filter5来自第二Guest）。这是Host原14的多平台证据，新增覆盖计数0/E0；不能把首Guest整体FAIL洗成成功，也不虚称第二Guest重新执行Root9。Host55原始档案单独冻结，不覆盖。

仍未完成/未授权：Go source policy PGmajor18、真实生产Root issuer/原WAL/Node预算、PG18 System/FI/实际RO发行闭包、safe normal真正硬降限、VM伸缩和三节点K8s E2E。本档仅准确Guest内核C组件；内核链root-v1通过不等于生产18Source或完整normal资格。tmpfs/--no-sync是测试机制，不是物理磁盘断电耐久认证。
