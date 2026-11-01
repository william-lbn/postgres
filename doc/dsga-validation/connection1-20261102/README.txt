PG18 Connection1：真实Postmaster连接门禁独立模块

基线aafbb451，原codex/dsga-pg18-v1-20261031。Linux实际开发，六生产文件、四测试文件；
不改PG17/gitlink/发行锁或公共上层合同。保持PG17 Connection1 snapshot字段/结构及Work2、
Buffer ABI6；Prepared/DSM/MemoryContext预算没有实现或冒报。六个核心门禁函数与PG17
81794a0e逐函数体字节一致，PM18实际生命周期接线单独适配。

Work私有控制增加原生32位wanted/applied原子，原ShmemSize会计入固定共享段预算。word为
serial<<1|open；新boot serial1关闭、applied0。只有PM主循环/正常处理pmsignal后应答，不在
原始信号handler、launch中途或backend锁中应答。原prestart发布屏障先于ack，观察者先读
ack再按read barrier和同status_lock采prestart/PGPROC，不能拼出不存在的“已关门且空闲”。
Publish只有status_lock内定长原子写，Notify必须锁外；serial耗尽也保安全关门并拒新开门。

PG18 BackendStartup在分配PM child槽和任何fork之前检查原accepted socket；关闭/租约过期
时只准内核getsockname核定的固定PGDATA/dsg_socket端点，并实时lstat核owned 0700目录
和socket，不读startup用户名/自报管理标记。管理路径启动时必须原监听配置中明确包含，
权限错误/缺预算/缺实际native profile/符号链接均失败关闭。公共端点不因SU身份绕过。

只登记四个PG17同名PGC_POSTMASTER参数，原默认/单位/范围：require_connection_gate和
require_admission默认off，startup_budget默认0/kB，lease_timeout 30000ms范围1000–60000。
不暴露尚无完整provider资格的enable/max参数，沿真实native/birth入口决定arrays启用/上限。
因此不是完整PG18生产配置或Root资格上线；关闭门禁只是完整安全降限合同中的独立事实。

b6eede受限heavy6CPU8GiB/make6/缓存构建39.396秒通过，自动串接native2CPU3GiB。
同一PG b65ef3f6a0edf17163d2e297ad2ffc4a1b4bcdc9b3496127899ab880057070f8、DSO
 a38274bbf5beeb1147f960d36b36e7f9761d6e564a7e4ff8d7c59b81f617ce29复用三次原生图。
最终7稳定目标=2U-native+5S，7配置负例子场景，零skip；仅新改门禁相关功能，未重跑旧
Buffer/AIO/Work2无关矩阵。实际覆盖：默认off/原session ACL；七配置负例；新boot初始关闭
ack/private可管理，八个不发startup字节公共连接立即EOF且PM子集合无变化；开关/再开，
真实halfstartup child44跨关闭ack仍startup1且foreground_idle=false，实际退出回收后才0；
1000ms原单调lease过期拒新公共连接、迟到open精确拒controlled recovery；同PM134 SIGKILL
reinit boot17937432665328950775→10332687590236564737，旧serial2/open→新serial1/closed
ack，不继承旧租约/准入。初始化CREATE EXT/ROLE后CHECKPOINT沿已核旧RM134夹具边界，
不是任意新heap WAL恢复或HA验收。

首轮e54804保留4PASS+2FAIL：新测试把已退出snapshot psql尚未被PM waitpid回收算入
halfchild；过期拒已正确发生但预期泛admissionrejected而原错误实际lease expired。没有改
C/删条件遮错误。仅Py等真实observer回收再断言单halfchild、断言精确expired错误，同九个
编译/安装输入及ELF复用只重跑两失败根61449f，实际2PASS2.809秒，不重跑原四根。

独立补1根ae8b12实际early birth test-v1入口，原数组import、真实NNP1、/proc核soft/hardAS
均2GiB，四数组原预留最大2048；同Connection1闭门→开放→再闭门通过0.830秒。没有
legacy数组环境；明确birth source_bound/root_enrolled=false，不能叫root-v1或Source通过。
原六测试方法AST不变，默认helper仍调用原super.start；同ELF不重编。全部原始失败、命令、
nonce/spec/源码锁/unit终态及cpu.stat/memory.peak/io.stat/memory.events保留，原缓存不prune。

剩余硬门槛：MemoryContext工作域硬预算、Prepared/DSM及完整排空、持续normal安全降限、
完整Root-bound Source/force/I、PG18生产启用配置、完整Neon新WAL扩展、PG17 Linux及三节点
Helm K8s真实端到端。Connection1的public-blocked/Work idle均不签上述资格或VM许可。

独立只读审查已完成，未见本模块明确代码阻断；原始审查2小档随阶段归档，不重计测试。
