PG17 四数组Root注册/预触页组件（2026-11-01，默认关闭）

原因与行为：原四数组由PG直接populate，不能建立独立arrays收费域。此批先把四数组专用路径接到固定Root Unix seqpacket：先完成四映射，再注册四range，再预触，最后PG初始化/发布；普通DSM/泛DsgRegionPopulate不改。Grow在原resizeX内保守记成本后发送，accessX/status spin不贯穿RPC；Root不得持资源锁SQL回调。PM全旧共享子进程退出后仅发送RETIRE_BEGIN，logger Park不全局退役；Root需保旧FD直到完整新inode注册或原PM真实退出。

兼容与边界：环境变量NEON_DSGA_ARRAY_ROOT_PROFILE缺省关闭。test-v1仅显式原生fixture；root-v1必须已有原Native Birth Final Gate，服务缺失/超时/错回复均失败关闭。512B/160B私有版本1合同未改变API6/Work2。packet中的boot/M/Key/区间只是待比对值，生产权限必须来自Root原Birth/WAL/预算/可信executor签名链。本批未实现生产Root服务，不能代替完整normal降限/Source资格。

验证：Linux GitHub原分支053d1c3c+精确6生产overlay构建；7221实际输入逐hash核验，GNU编译/安装成功。新PG ELF/扩展hash见source-manifest和build原收据。旧Root9+兼容11+迁移机制13case(同一机制root)为21逻辑根直接回归，不重复计新增。新4个真实PG单服务组件根均PASS/零skip：真实map_files backing/Root预触，增长-缩小-再增长，坏或丢回复，logger及三次内部恢复。Root为明确测试fixture，不是真实原Birth/Key/预算来源；Docker Host并非KVM369。

真实失败全部保留：启动期pg_cryptohash_create依赖尚不存在CurrentResourceOwner导致SIGSEGV，已改OpenSSL一次性EVP_Digest；另源夹具/ccache/receipt路径/Docker ENTRYPOINT配置/固定socket名及低于原NBuffers下限的夹具错误均保留原终态。不能转签旧627原生证据。

持续入口：test_array_root.py尚为本批精确独立入口；需要隔离Root fixture所需SYS_ADMIN/SYS_PTRACE，network none、4CPU/5GiB、无Host敏感挂载。不能把该测试Root容器作为生产Source或normal授权。新Root服务完成后须以其真实原Birth、专用body签名、WAL、helper真EXIT+持久前缀和全cap写者屏障另行验收；K8s E0。
