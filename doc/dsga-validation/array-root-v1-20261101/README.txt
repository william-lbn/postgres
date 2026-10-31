PG18 四数组 Root ABI1 组件（2026-11-01，TEST_ONLY；首版整体未完成）

行为：只在显式原NEON_DSGA_PG18_NATIVE_ARRAYS=test-v1:M加NEON_DSGA_ARRAY_ROOT_PROFILE=test-v1启用。四真实backing全部建立与登记后才注册Root，再由Root预触，PG开始初始化业务数组；grow在原resizeX下先保守记unknown/upper再RPC。Root短连接/512B/160B ABI1和header与PG17固定Git b95b1421完全同版。新文件不扩泛Provider/DSM，不改变原AIO completion屏障或IO worker类别。

原18尚无早期AS/R、原出生198/最终ptrace门：root-v1明确FATAL，不能借test-v1的11/22/33字节造生产身份。普通无profile仍原本地Provider，静态配置也前置检查显式Root请求。PM清旧共享子进程后只发RETIRE_BEGIN，不把ACK当backing释放；logger退出不发全局Retire。Root必须保旧FD，直到完整新四inode注册或原PM真实退出，不按客户端代次/账本清表自行结算。Root不得在resizeX RPC内SQL回调。

来源与实际验证：Linux18固定15cacfb3原功能分支+独占8源，17代码只从已push b95b1421 Git对象读取；独立archive+只读/src共7281输入实际sha256sum全部OK。GNU12/4CPU5GiB/禁网/原ccache，无新正式镜像或发行锁。最终编译32.67秒、零编译warning；git diff --check通过。
最终ELF c431dc0dcffe68ff731d2ae08c1d173d67e3282619e077455794bfb101c8e7f7；扩展9a9a99e7。最终4S真实外层/测试全0、4PASS/零skip（6.63秒）：四真实shmem inode/map_files+Root MADV_POPULATE_WRITE、512→2048→512→1536及SQL数据；坏摘要/丢初始回复、无18出生root-v1及缺原数组profile失败；丢grow回复保H/generation/unknown；logger和三次真实共享恢复、新四inode才释放旧RootFD，原18 IO workers仍为2。

旧31直接根在缩进前ELF c101上真实运行：原region/arrays/resize/failure30根PASS，以及精确uring一根PASS。最终只对本人三行前导缩进转tabs，源逐行非缩进字节/行数相同，7281源唯一差异是该数组C。实际48节ELF仅debug_info/debug_line/BuildID不同，34个ALLOC节除BuildID全部内容/地址相同；证明见build/elf-section-equivalence.json。最终4S已用新ELF重跑。旧31保持原产物归属与严格运行节等价证据，没有声称它们在最终ELF重跑，也不虚增覆盖。

失败历史完整保留：首跑driver缺--init使Root成为PID1，被原严格peer.pid>1门禁拒绝；实际socketpair诊断无init peer=[1,0,0]，有init=[7,0,0]。仅修fixture命令，不放宽C，再同源同ELF4PASS。另uring按继承class加载6个旧failure副本+1uring都实际PASS，但expected1门禁拒；后继精确方法1PASS，收据driver读取Docker省略Init键KeyError原unit1保留，另只读原容器证实Exited/ExitCode0/实际测试1PASS后清理本人已退出容器，不改旧外层结果。最终新增4个稳定S，U新增0/E0；重复运行不算新增。

这里的Root明确测试fixture，不能产生原生产Source/Birth/Host预算、normal或VM许可。tmpfs隔离协议IO且initdb --no-sync，不是物理磁盘/断电持久证明。未取代PG17首版normal、真实Guest、117/118/119 Helm最终端到端门槛。

持续下一批：18自身早期AS→四R预留/导入与Park保R→Native准备198/PM base/full BPF→Root独立最终停点→后续MemoryContext/GUC；迁移时保原B_IO_WORKER、共享与非共享子类、原AIO末访问lease和access/resize/completion锁序。原14/18 Reader、预算、Source/发行闭包及多版本联合回归另需真实验证，不等待控制面但不假称当前组件已经全部支持18。Git由Root统一提交。
