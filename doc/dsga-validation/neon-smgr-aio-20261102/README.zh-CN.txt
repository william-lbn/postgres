PG18 正式 Neon 扩展 / smgr / AIO 阶段归档（2026-11-02）

本批完成的是PG18底层存储适配与有界组件验证；PG17发布锁、Neon gitlink及上层DSGA/SQL JSON合同未移动。PG18内核与Neon各沿当前功能分支，Git/index与推送由Root处理。不能将本批称为PG18正式Pageserver时间线、Root出生、normal硬降限或K8s端到端通过。

实际实现：PG18原smgrregister/owns/name/maxcombine/startreadv/fd与独立unlogged hooks接线，local/unlogged仍走md；远端保原Neon RPC，完成后经新内部同步AIO入口处理原stage/submit/shared/local callbacks。扩展不写opaque handle、不提交假OS FD。内部fd=-1仅表示本操作不关联OS文件，不进行OS readv替身。ERROR先完成所有失败Buffer与pin释放，再重抛原错误；errfinish会清零holdoff，CATCH重新HOLD。已收到的远端ERROR slot在PG18原ereport保存文本后精确退役，使同一backend能真正重新请求，PG17原分支保持。

另按实际头适配PG18 inttypes格式、WaitLatch显式声明、injection point双参数、slot acquire新参数；SLRU真实int64 hook标识为v3，缩窄前分别核UInt32 wire与现有int请求字段，超过范围拒绝，不能截断。PG18 CLOG/multixact仍使用核心<=24bit短名，不支持类型交回核心。Neon WAL头17/18字节一致；PG18 ReorderBuffer Get->Alloc真实API在rmgr本模块头适配，定义真实decoder，正式logical I/U/D已实际产生并解码。

最终稳定目标4个：a14568的正式Neon1.17 SQL协议grow/shrink/regrow、数据/replay/conflict/expired及Work2/Connection1/Prepared1/DSM1观察接口与logical I/U/D；同次模型peer的实际Neon远端读成功；原ERROR后同backend、AIO完成数等于pin释放数且后续读成功；SLRUv3实际下载/范围拒绝。24d151仅补最后SLRU目标的INT_MAX+1、UINT32_MAX边界拒绝，最终ID以该目标替换，不累加为5；其它函数AST未变且3根不重跑。

peer_is_model=true：ci/test_dsg_pg18_remote_aio.py::PageStream是本地PGv3/COPY协议模型，不是正式Pageserver。GetPage数据来自同一隔离真实PG CHECKPOINT后表文件，路径/长度/SHA在原证据中；SLRU是fixture固定3页M。只针对单个测试relation注册md元数据+真实导出的Neon读回调，生产全relation selector本批仅编译/静态核，不能冒该选择器运行验收。正式SQL本身不使用外部storage peer。U=0，新增服务/组件S=4，正式storage E=0，normal/Source均false。

PG17未重跑旧功能矩阵，仅原准确PG17头/pg_config/649f ELF下编译7个受影响C translation units：原6个exit0；libpagestore首缺原PGXS internal include，补该真实include后仅重跑该1个exit0。最终编译器收据逐项归并为7，不计8。旧one-driver由于PASS_SEVEN marker名误判wrapper false，原result/verdict均保留；本归档独立核每个真实compiler exit，不将该marker当7次执行或测试通过。无PG17内核修改、无PG17新ELF/SQL测试。

所有原失败与修复按nonce保留：宏兼容失败、smgr/latch接口失败、其余编译接口、rmgr decoder未定义导致首次启动拒绝、内部local符号被误要求动态export的脚本失败、AIO CATCH holdoff断言、旧ERROR slot复用。最终行为依据真实后继运行，不覆盖旧日志，不把BuildKit0等同extension make0。小证据映射列全部原路径/SHA，ELF、DSO、大源码/缓存仅留/data，不进Git。

后继真实缺口：当前Rust版本模块/FFI/Pageserver仅支持14–17；不得把PG17时间线或control文件冒18。PG18实际Pageserver初始化、PG18 control/WAL解析、Neon生产发行版本选择与联合回归仍待独立适配。PG17 Linux安全normal降限与117/118/119 Helm K8s硬门槛不由本批替代。Storage broker是否需改必须按实际版本字段/协议消费点判断，不能仅因旧镜像标签重构。
