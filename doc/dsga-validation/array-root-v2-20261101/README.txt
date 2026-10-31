PG17四数组Root组件v2：非Linux缺省关闭编译保护

当前以37ab5c精确Linux7221输入构建为准。新C只在__linux__编译SO_PEERCRED/ucred/seqpacket实现；其它平台缺省使用原Provider，显式启用FATAL。InitBufferPool无论静态/动态配置都先检查显式模式，不能因未进入dynamic分支静默忽略请求。没有宣称Mac交叉构建功能认证。

当前7源和ELF见source-manifest。新4个真实PG单服务机制用例d96127全部PASS。旧直接21根：Root9和迁移机制root(13case)6911e3通过，兼容11中10通过；唯一旧BIRTH-S3因实际PG crash fsync超过原15s失败，原日志保留。只在隔离测试容器给/tmp设置512MiB tmpfs后，以相同源码/ELF单独重跑未改的旧S3，047460真实PASS。新4S恢复用例也保持原15s，仅初始化诊断变量，未放宽断言。fsync仍真实调用；tmpfs只隔离协议/机制夹具IO，不是物理磁盘耐久/断电证明。

先前74档案完整保留，是原版本历史：旧source-manifest/artifact-manifest引用旧C7输入，不转签当前源。新joint manifest覆盖当前全部提交文件；manifest自己不自hash。所有历史ENV、SIGSEGV、socket夹具、下限错误、fsync deadline及中间锚点版本均保留原路径/小日志。

实际生产仍需Root原Birth/真实peer pidfd→map_files registry→原Start CAS专用body签名→Root私有WAL→helper真正EXIT+fsync→ACK及所有cap最终写者屏障。test-v1 Root来源是明确fixture；root-v1缺正式服务失败关闭；本组件不产生normal或VM许可。API6/Work2、PG18/发行锁均未改。E0。
