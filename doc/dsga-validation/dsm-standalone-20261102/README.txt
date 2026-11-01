PG17 DSM standalone边界修复：定向组件归档，非首版封版验收

本批沿原codex/dsga-pg17-v3-20261019分支、81794a0e基线，只改dsm.c与dsg_dsm.c。
实际注册、创建中标记、after-create注入及两处发布共同使用enable_dynamic_shared_buffers
且IsUnderPostmaster，避免standalone走未登记却发布的PANIC路径。正数额度在native关闭或
standalone不能强制的模式必须启动FATAL，不能静默忽略。standalone零额度保留原生DSM行为，
其未跟踪计数明确accounting_valid=false/limit_enforced=false；PM初始化仍允许有效跟踪后由
真实fork继承。原控制段路径不新增依赖PGPROC的锁，不移动gitlink或发行锁。

真实执行只有一个单服务/组件目标PG17-S-DSM-STANDALONE-AND-CAP-001，含四组检查，
不是四个unittest根；Unit=0、单服务组件=1、E=0。直接受影响的测试使用probe/shim17.c
及native/run.sh、native/worker.py，源码/脚本/测试DSO/PG ELF/完整编译输入SHA均有锁。
真实零额度standalone先SELECT证明启动，再dsm_create4096/write42/read/detach，并读取
invalid/unenforced快照；正额度standalone和nativeoff各须精确FATAL。真实fork先实测原生
pgStat外部段1MiB（实际dsa_dump总area1.25MiB，主内存256KiB及外部1MiB，未假设零），
据此固定cap为1,056,768B，再真实pin8192达上限，下一4096请求必须OS创建前拒绝，前后
实际OS对象名/大小清单逐字节相同，pending/unknown为0，真正unpin后恢复原baseline。

最终f8fc68新PG ELF649f4535d07e682fe5015fd8df8be23f626e73bd772da21b16e1c032721c53c6，
构建61.828s；同nonce-native实际3.526s通过、唯一Docker Wait0、OOMfalse。镜像已明确拉入
Docker可见：127.0.0.1:5001/neon/pg17-dsm-standalone-fix:20261101t190032z-f8fc68-TEST_ONLY。
此为测试镜像，完整不可变digest与资源记录见catalog.json。构建上限6CPU/8GiB；native预约
2CPU/3GiB而实际容器1CPU/512MiB，无额外swap，所有命令/nonce/unit/metrics原件保留。
编译父层与最终层完整source-index只变化dsg_dsm.c；不能将父3b569旧ELF冒称修好invalid。

原817305启动第一SELECT前已经PANIC134，准确解释另存，原driver误分类不重写。初1ae562
修复四项通过但未检查standalone snapshot；补充90a9be真实返回valid=true导致断言FAIL，
该失败推动fix2新C/新ELF。本目录history保留原始结果、错误和来源，未把失败重标为通过。
第一次自动native因unit未绑定-native nonce在调度前被拒的原记录也保留于原/data目录；
不构成已执行业务测试。后续正式f8fc68所有预约/启动均成功，未重用失败调度的身份。

完整原始工作目录均见catalog，未复制PG ELF、测试DSO二进制、数据目录或缓存入Git。
本批不签Root Source、跨boot Root-held清理、normal安全降限或Linux/K8s端到端；只验证
本次修改直接影响的边界，不扩测原Neon未修改逻辑。最终仍需主任务完成真实链与三节点Helm。
