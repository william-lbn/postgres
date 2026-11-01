PG18 Work2：真实进程与前台工作观察独立模块

基线60378838，原codex/dsga-pg18-v1-20261031；Linux实际开发，不动PG17/gitlink/发行锁。
Work2沿PG17相同snapshot字段和版本，不是MemoryContext硬预算或VM许可。PG17原
utils/mmgr也未含此预算；不能把Work登记叫作已实现内存分配记账。Connection1、Prepared1、
DSM和required connection gate未在此实现/声明，原PG18 required stub仍拒绝。

15源=11核心/接线及4测试。主共享段精确预算固定Work控制+PGPROC槽+PM预启动原子槽。
PM在真实fork前登记、实际waitpid/fork失败后清理；与PGPROC STARTUP在同status_lock交接，
不留两者同时0的窗口。PM不取backend锁、只用原生32位原子；detach在原资源退出之后、
ProcStructLock之外。新共享boot重新初始化，不继承旧进程/事务许可。PG18由PMChild*分池，
故release钩在pmchild.c；IO worker按实际后台登记。standalone无PM预启动槽；本批fork-only，
EXEC_BACKEND受管feature显式拒绝且不声明Work2，原默认关闭不受影响。

前台类型字节在正文分配前登记READING/BOUNDED_INPUT，实际执行后才记EXEC，真实消息/
事务/Portal清理及Ready输出后才记idle；retained事务独立保留。关门丢弃复用原固定缓冲，
保类型/长度/Sync/EOF语义；已有事务只允许最大512B已核结束帧，并在实际AST再次确认。
修改前程序化证明原18 SocketBackend与去掉Work增量的17逻辑一致，才移植同义增量；
原18 pq_getmessage本体不变。控制响应仍只是原测试夹具，不授预算/normal资格。

原6f5eff heavy6CPU8GiB/make6/ccache实际编译53.648秒、无编译错误，自动注册2CPU3GiB
native9b272d，无人工编译→验证空档。5直接目标：原默认off/原session ACL；真实半startup
关闭及SIGTERM后PM回收；半消息读取/实际执行/取消；保留事务+关门4MiB有界流式丢弃与
协议恢复；同PM SIGKILL实际reinit新boot清旧计数。最终1U-native+4S共5根、2子例、零skip。
前4在9b实际PASS，末根首次ERROR：新PM startup redo遇原Neon RM134未注册。

没有修改生产遮该故障。先以旧未修改ece22255 PG/原be266f DSO实际双对照6da39d：无
初始化checkpoint精确复现相同134；加checkpoint后同PM60 boot14648270242637647818→
7964699935485965399真实恢复。源码RM_NEON_ID134只由本批未改heapam写入，Work没有
新增WAL记录；旧arrays recovery夹具本来先CHECKPOINT。于是只在新crash测试给已完成
CREATE EXTENSION/ROLE的初始化做CHECKPOINT，之后测试仅BEGIN/共享Work状态，不制造
新heap WAL。不称134插件已实现、不扩大到任意写负载恢复，也不跳过真实reinit。
同14编译/安装输入、同PG1df69850/DSOb40bfcd1，仅Python crash方法改变，排除该方法后
全模块AST与首轮完全一致。复用同ELF只重跑失败根893317，实际PASS1.416秒；同PM21
boot12631253334209973732→16690231203782605449，原client1/retained1变为0/0、其它
工作计数0，真实BG2/Aux5仍可观察。require_admission=false测试profile新boot open=true
如实记录，不伪称关闭。首次错误、旧ELF前置对照、所有原命令/源码锁/资源指标完整保留。

尚未完成：MemoryContext预算、DSM/Prepared保留资源、requiredgate/no-future-fork ack、
完整Neon18扩展与134新WAL恢复、Root原force/I/normal降限、准确Guest与K8s。Work idle
始终只是同观察点的事实，不能推出无未来分配/进程或VM回收许可；不替代PG17单机/K8s门槛。
