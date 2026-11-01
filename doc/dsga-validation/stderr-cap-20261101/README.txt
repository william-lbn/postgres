PG17 stderr容量硬上界：阶段归档，非封版验收

实际基线b95b142190cdc51de0d5603bfb9c46a0e0e7051f，保持原功能分支。本批公共fcntl_rule仅增加
F_SETPIPE_SZ=>EPERM；PMbase、PMfinal、child共同覆盖。保留GETPIPE_SZ和普通fcntl，
无ABI、公共控制协议或发行锁变更；默认关闭模式保持旧行为。

实际独立Linux容器验证2根/5子例，全部PASS、零skip。新增两根：
三个实际BPF程序逐一执行同容量、增大、缩小和高32位alias SETPIPE拒绝；同一UID1000、
NNP1在安装过滤前同容量SET成功，排除仅因无特权而失败的伪阳性。GET、SETFD、NONBLOCK、
DUP和原管道往返正常。真实PG PM及其backend/stderr collector路径定向检验，核默认off、
保护模式、原session权限、SQL数据与真实PM Wait。PG18还重跑直接受影响的原Root出生链
一根（含leader与pthread）；PG17不借用PG18的二进制或DSO。原始命令/结果、C/Python探针、
源码锁均在此目录；完整可执行产物与隔离上下文保留在/data/neon-build/validation实际路径。

PG17实际编译增量101.25秒；PG18实际新编译65.91秒。后续仅加强Python无特权正控时复用
相同已核C输入和PG/DSO，避免无必要重编译。不能将历史失败/较弱旧通过重标：PG17首轮
保护模式缺enable_dynamic_shared_buffers启动失败；PG18首编译尝试因report0700令UID1000
无法读build.sh而未真正编译，均留原始证据。精确耗时/ELF/source SHA以catalog与原锁为准。

本批是Host Linux内核与隔离PG服务组件测试，不是指定18c Guest，不签完整Root Source、
持续force/I、真实normal安全降限或Linux/K8s E。仅deny所有PG进程SETPIPE不独自闭合
Compute/Root仍持写端的持续来源；后继真实Rust注册/唯一Wait/EOF/OFD验证须另外通过。
本机registry测试镜像不代表发行镜像；不移动PG17 gitlink、PG17/AS发行锁或共享接口。
