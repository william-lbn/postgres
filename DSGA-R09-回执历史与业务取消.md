# DSGA R09：内核回执、有界历史与业务取消

日期：2026-10-20。此模块是PG17内核C接口，不是Neon生产SQL/HTTP管理端点。
不改变WAL或磁盘格式；共享结构/请求扩展使Buffer能力ABI由4升为5，相关扩展必须
重编，不能按PG大版本推断兼容。默认关闭、启动小池及新boot准入规则继续生效。

## 请求和结果的职责

`DsgBufferControlResize/Status/Cancel`返回`DsgBufferControlResponse`：

- `receipt.key`始终是本请求的boot、issuer epoch、revision与操作ID两个半部。
- `receipt.has_result`为真才有本操作的`result`；BUSY/STALE/CONFLICT等不能把当前
  另一操作的状态填成自己的成功结果。ACCEPTED只表示已建立操作，需再查看phase/reason。
- `observed`独立表示当前共享事实。查询历史G1时，result可以是H1/generation1，
  observed已经是G3/H3；这不是矛盾，也不能用G1的回执授权当前VM动作。

旧Grow/Shrink函数仍保留原有签名和拒绝时ERROR行为；受管入口新增明确回执类型，
不捕获/吞掉原生ERROR或FATAL。调用事务ROLLBACK不撤销共享容量事实。实际管理
认证、VM/Compute实例身份、当前issuer所有权、资源承诺须由Neon/外层执行链核验。

请求补`issuer_epoch`以及32字节外层规范意图SHA-256绑定。epoch/revision按uint64
二元顺序比较，较大epoch允许revision重新从1开始；0仅兼容旧内核测试调用。
同一保留操作ID改变epoch/revision、方向、目标、generation或context hash返回冲突。
hash不能充当认证或预算证明，生产调用方必须从验证后的完整意图计算，不能信任
普通请求自行填入的hash。本模块测试token只用于绑定差异，不宣称验证了生产哈希。

`DsgBufferParseUint64`只接受规范ASCII十进制字节串，不经signed bigint/float；
拒绝空、前导零、符号、空白、小数、指数、非ASCII和溢出。失败不改输出。boot
随机生成显式排除0。所有跨语言输出仍须用十进制字符串，不能在JSON中降为浮点数。

## 有界历史与恢复

当前操作外保留16条终态快照。新请求通过校验后，持resizeX先恢复确无执行者的
遗留操作、再归档并覆盖；status_lock只做固定上限查找/复制，无分配、IO或长等待。
环形日志内存属于永久控制目录，启动共享预算自动包括实际结构大小。

查询和取消可以条件取得resizeX，对账FATAL先放原生LWLock、退出回调尚未执行的
PREPARING。正常执行者仍持串行权时查询不等待。中断的Shrink恢复L=H，Grow保留
已经计入的backing/unknown；旧回调按完整身份复核，不能改写后来操作。
原始Grow路径的新请求未通过校验时，不提前清理另一Grow；Shrink遗留L则必须先
恢复才能核验容量方向。受管状态查询的恢复是查询本身的明确语义。

相同旧key被淘汰后，查询RESULT_EXPIRED，新resize返回STALE，绝不重新执行。
换boot返回STALE。RESULT_EXPIRED仅表示内核无法证明旧结果，不能声称该ID曾经
被接受。**16条日志不能证明全生命周期UUID唯一性**：把已淘汰ID故意配上更大
epoch/revision的篡改必须由外层持久历史/唯一执行者约束拒绝，不把内核有界内存
宣传为无限历史。外层须先持久登记意图，按完整实例/boot对账，不能重造ID规避未知结果。

## 取消的线性化与预算

取消只作用于完整key匹配的当前PREPARING。与最终发布共用status_lock：取消先
到则置标志，执行者在分批populate/初始化/排水/过滤以及发布锁等待期间退出；
最终BeginCommit先到则关闭窗口，之后TOO_LATE。H不变的Grow仍关闭同一窗口，
只提交操作终态，不需要数组accessX。

CANCEL_REQUESTED是已登记意图，终态必须再查询；不是“资源已回收”。取消前
Grow已经触页的成本不抹除，Shrink H未发布则恢复L并保留已写回/驱逐的真实结果。
H发布后禁止回退或取消逐数组账本，Provider不支持/失败/中断仍保留保守预算。
每个数组回收身份包含epoch、revision、ID两个半部和generation，避免换epoch后
同revision的no-op Shrink误归属。实际Provider调用仍是同步系统调用，不承诺强行
中断已发出的调用。查询/取消旧终态返回TOO_LATE，未知请求不等于成功取消。

业务取消不发送进程信号；SQL cancel/FATAL另走原生异常清理并记录INTERRUPTED。
SIGKILL按Postmaster规则重建boot，旧回执/许可不能沿用；ROLLBACK不撤销已发布H。
所有这些结果都不是VM完成或VM缩容许可，外层仍须执行预算与最终动作fencing。

## Mac测试与剩余边界

新增独立C测试桥，仅转换参数/快照，不改共享状态、pin或Provider。桥的C入口
同时核验当前角色和会话superuser，显式GRANT/SECURITY DEFINER不能绕过；这项
测试防护不能替代未来生产端点的完整认证。

永久用例位于Neon `ci/test_dsg_control.py`，并被严格宿主测试入口显式纳入。
覆盖真实G→S→G历史、冲突/淘汰边界、uint64高位/换代、BUSY与当前身份分离、
运行中重复查询、发布前/后取消、L恢复/真实数据、Guard等待取消、SQL回滚、
ERROR/FATAL交错、换boot及权限。Guard真实持accessSH，取消执行者必须在Guard
仍未释放且10秒原期限未到时退出，不能通过先释放读锁使测试假通过。

等待锁取消的旧实现已真实失败两项（2秒等待上限），随后补请求绑定轮询；原始
失败日志`build/neon-r09-guard-negative-fixed-fixture.log`保留。第一次测试夹具
错误复用了要求resizeX的断言，导致断言失败；已按注入点分别核验accessSH/resizeX，
该首次失败不能充当生产缺陷证据。历史保留初次用例将16条边界算错一位，也已
更正为当前21、历史5..20，不通过减少日志保留数或放宽实现满足错误预期。

最终候选原有48项加新增19项合计67项通过122.538s，零skip；最后补接口反向锁序
拒绝与历史取消不影响当前PREPARING两项后，完整新增21项通过44.279s，零skip。
PG及测试桥实际重编/安装，Ruff/pgindent与diff检查通过。日志分别为
`build/neon-r09-control-tests-final.log`、`build/neon-r09-reviewed-tests.log`；
准确SHA重编、69项组合、完整开关回归及Neon扩展结果继续记录在主仓实施状态。
Neon生产端点/完整预算、持久历史、跨层fencing、其他PG版本仍是代码工作，不列为
Linux后验。真实物理回收/HugeTLB/VM/K8s/目标性能继续按V3在目标环境认证。
