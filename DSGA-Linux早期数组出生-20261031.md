# PG17 早期四数组地址预留：默认关闭组件

本批在main落实原hard AS后、PG MemoryContext/配置/扩展前，显式test-v1预留四个PROT_NONE区间；长度按真实编译ABI/page checked推导，R必须小于原AS。默认缺省不启用，root-v1尚不支持，测试模式始终source_bound=false/root_enrolled=false，不能取得normal或VM降限资格。

首次InitBufferPool导入原占位，拒绝max/off漂移；内部重建复用Park/Rearm。非共享logger将旧shared backing同步替换为private PROT_NONE，保持R，不把VA腾给私有分配；原未启用路径保持原行为。真实PM/backend/logger映射、logger重启、三轮后台崩溃内部恢复及数据checksum均已验证。

原生Linux v2新增6根（2U/4S）全部PASS、零skip；另采用v1直接关联5根（其中AS格式根已在既有PG17-S-PG-019归档，不重复算新增覆盖）。二者明确是各自准确快照，未把旧5根说成v2重跑。PG生产二进制v1/v2一致，v2测试库仅新函数权限门禁改变。SQL REVOKE PUBLIC加C当前/原会话管理员核验，真实CREATE EXTENSION、普通role、显式GRANT及PUBLIC别名拒绝均通过，独立审查finding已关闭。

v2实际镜像内7165份源码逐hash与冻结树一致，包含全部9改动；v1编译镜像7164份源码一致，Python在编译后新增、通过只读运行挂载实际hash核验。初次把运行时Python也要求出现在v1镜像的来源核验失败保留，分类后无编译源码不一致。摘要/稳定编号/命令/原始证据hash见[收据目录](doc/dsga-validation/early-birth-linux-20261031/v2-catalog.json)。大日志、真实maps和原镜像保留/data/neon-build/validation/pg17-native-birth{-acl-v2}-20261030...目录。

尚缺：不可放宽seccomp、Root Pending/实际映射和完整filter核验、入组/Enrolled、N0原出生绑定、完整文件/在途/Outside/MOVABLE来源、安全normal硬限下降及KVM/K8s E2E。只验收Linux fork/default-off组件，不外推EXEC_BACKEND、32位或Meson实编。Python持续CI入口在下一批接线；本批使用独立严格6根runner，没有声称installcheck自动覆盖。
