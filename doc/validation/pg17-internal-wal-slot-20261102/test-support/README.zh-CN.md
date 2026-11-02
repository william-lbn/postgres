# 静态WAL来源位：隔离组件测试源码

这两个源码文件逐字复制实际094/8af输入：C与094已编源完全相同，stdlib Host driver与8af成功源完全相同。它们是测试替身，不是真实Neon业务实现，不产生Root/预算/VM权限。禁止安装进生产Guest、PS或任何发行镜像；禁止在生产PG/lib覆盖neon.so。没有生产Makefile target，也没有自动install步骤。

实际8af：native唯一Wait=0、driver=0、1.954秒。静态PM getter=true且原PID/父postmaster匹配；动态同descriptor getter=false且原Core handle PID匹配并到STOPPED；携私有位的动态注册API返回false/no新child。DSB明确关闭。本结果只证明真实a766 Core的来源位/API机制；Source/normal/E均0。真实Neon静态WAL及DDL/SK正控与普通SQL/外部physical负控另有原收据，不与本替身混算。

## 实际输入与隔离

- 实际PG ELF a766574d095a40d00c70bd753fa9d0fcfde9f48767e883ab3e2e59014c4956af，PG17 cf1659f三core修复。测试只读其库/utility，不改任何旧测试或生产数据库。
- 编译器容器已缓存：127.0.0.1:5001/neon/pg17-dev-tools@sha256:c940750ed64e18dbe1d5acda5f12b61bb3f1930be149140c54b4e09e62379ba1；实际/usr/bin/gcc，UID1000，cap-dropALL/NNP/RO/networknone，compile与运行同native2CPU3GiB reservation串行。
- 准确private header取本次PG源src/include/postmaster/bgworker_internals.h（a517bd9b前缀，完整SHA见receipt/原source-lock），放临时RUN/headers/postmaster；不从任意旧PG头推断private bit。
- 实际原隔离运行基底：127.0.0.1:5001/neon/pg17-dsm-standalone-fix@sha256:b164d61a939502fa3ba983c0359f7832b9345ccefeb15c00326eaceab61a3a40，来自f7b独立SQL夹具。postgres可执行文件只读覆盖为上面新a766，utility ABI/库与原夹具保持，旧DB未触。
- 每次必须新临时cluster、新RUN、新container名。把这两个源码复制至RUN，Host driver只在RUN旁写evidence；禁止直接在Git目录生成ELF、PGDATA或凭据。

## 复现已跑逻辑（显式参数，不增加新框架）

1. 明确PG_SOURCE、PG_INSTALL、RUN三个准确目录。校验PG_INSTALL/bin/postgres的上面SHA；从PG_INSTALL/bin/pg_config取得include/server与include/internal，不猜postgresql/server路径。复制本次private header到RUN/headers/postmaster。复制本目录C/driver至RUN。
2. 用同GCC镜像、/input=RUN只读、/out=RUN/evidence可写、/pgdev=PG_INSTALL只读编译；实际CFLAGS为：

   gcc -shared -fPIC -O0 -Werror -Wall -Wextra -Wno-unused-parameter -I/input/headers -I/pgdev/include/server -I/pgdev/include/internal -I/pgdev/include /input/static_origin_probe.c -o /out/neon.so

   产物名neon.so只是测试tuple要求，不是生产库。实际ELF99447c3cfacd4b3430e744b3d0bd8d142ae0426ed9714628c6d4d8936e962e18，仅限临时组件；若编译路径/编译器不同，新产物SHA必须独立记录，不伪造相同digest。
3. 在上述独立runtime容器中，显式只读挂测试库至临时PG的lib/neon.so，使用新PG可执行文件。该路径仅在一次性容器内，不能对原PG_INSTALL、真实Guest或PS做make install/覆盖。原临时initdb/pg_ctl驱动可复用f7b，但配置必须为shared_preload_libraries='neon'、enable_dynamic_shared_buffers=off、shared_buffers='2MB'、listen_addresses=''、port=55432及本次私有socket目录。原fixture只本机Unix trust，不能搬到生产认证环境。
4. 只设置本测试固定环境DSG_STATIC_SLOT_TEST_OUT=/evidence。把RUN/evidence挂为容器/evidence RW，等待pg_ctl真正ready；C静态worker在真实PM preload路径登记，写自有PID事实后按release文件等退出。文件仅用于测试诊断/退出同步，绝不是授权来源。
5. 从Host已存在的Python执行，不向runtime安装Python或新依赖：

   python3 RUN/host_probe_driver.py CONTAINER_NAME /usr/local/pgsql/bin ACTUAL_PRIVATE_SOCKET_DIRECTORY ACTUAL_INITDB_SUPERUSER

   参数用原fixture真实值；没有连接串/密码/JWT。Host driver通过docker exec调用真实psql。C SQL helper只允许临时SU，测试用descriptor仍保持SHMEM/RecoveryFinished/notify0/neon/WalProposerMain/name/type。静态probe用/proc PID的PPid匹配真实postmaster.pid，不强求无DB shim发布pg_stat_activity。
6. driver观察静态true后仅释放该worker。动态同tuple注册由原Core Handle观察真实PID，子probe false后释放，原GetBackgroundWorkerPid到STOPPED。伪造位调用必须同步false且probe集合不变；不能把slots满/别的错误当拒绝来源证明。
7. driver结束后由原驱动正常pg_ctl stop并唯一Wait、保存小JSON及资源读回，清理仅本次容器。任何超时/异常先FAIL，不能从marker存在补签。旧094静态观察器失败原件仍保留；8af仅修观察器、复用同字节C/ELF，没有重编或变更Core。

## 归档边界

actual-receipt.json列真实source/result路径SHA。原/data的NOT_RUN设计收据及失败run保留历史，本新收据描述当前实际已执行状态。此仓库只归档小C/Host stdlib源码、中文说明和小收据；不复制ELF、raw日志、完整Docker命令/env或任何private数据。运行器/环境接线缺失应NOT_RUN，不得因为源码存在即通过。
