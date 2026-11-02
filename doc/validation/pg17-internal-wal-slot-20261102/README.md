# PG17 内置 WAL 进程固定复制槽修复

问题：DSGA新增物理复制权限门要求当前/session都是内部SU，但Neon原内置WAL proposer没有数据库会话，因而被拒管理自己的wal_proposer_slot。真实2f6389完成扩展创建后卡建表，主worker245次命中同一新门。

实现：PM仅从原shared_preload静态登记的精确内置描述生成私有来源位；动态API拒绝伪造位，PM复制动态shared slot时二次剔除。只允许该内置进程对固定持久物理槽Create/Acquire，不升SU、不扩外部SQL/walsender权限。

验证：cefa02增量编译0；5183db真实扩展/建表/插入三步均完成，但后续反馈错误路径在Neon扩展崩溃，整体native退出1，不重标通过。外部负控在两个原nonce分账完成：3项SQL与1项物理启动拒绝，共5次实际尝试均42501且槽/容器硬限不变。首SQL驱动后续非零保留；物理连接在命令发送前就拒绝，不冒称测过两个命令dispatch。

待验：动态描述仿冒与伪造来源位仍NOT_RUN。此为阶段提交，真实normal降限、Linux全E和K8s尚未验收，不移动父仓gitlink或发行锁。原测试、失败和大日志保留Linux /data，准确索引见receipt。
