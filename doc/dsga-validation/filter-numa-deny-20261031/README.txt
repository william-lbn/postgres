PG17 Native NUMA策略不可漂移：home_node最小共同拒绝

准确基线为99f7ff1dac70a28fab9a3980e9ad5eb07b042603，保持原PG17功能分支。
只改dsg_envelope_filter.c的共同forbidden数组，增加__NR_set_mempolicy_home_node。
原move_pages/migrate_pages/mbind/set_mempolicy、namespace、io_uring/userfaultfd及
其他规则保持；默认关闭/test-v1/root-v1模式与原出生gate无放宽。没有改变公共ABI。

原因：准确Linux6.12.26 mempolicy.c1540..1604的home_node只改current mm既有
BIND/PREFERRED_MANY VMA策略，不直接迁移，但仍改变未来NUMA分配策略。原共同
过滤器default ALLOW使它能执行。旧原生镜像真实backend中len0返回0的证据已保留；
本批新增规则在PMbase/final/Child共同Build里最先匹配，任何参数均EPERM。
具体线程继承/child安装窗口/Root-helper角色边界见prior-readonly-role-review.json。

仅COPY该一份新C输入到准确旧af97镜像增量编译，RUN实际sha与新C完全相同；
构建32.4秒，含导出推本机registry共45.099秒，无warning/error。新镜像仅TEST_ONLY，
准确digest7e602e1c...与PG ELF62757f73...见image和actual-image-source-and-elf.json。
原fork/launch输入未变，原旧cohort/镜像/ELF/放行结果不覆盖或改名为新通过。

三个独立容器并行，直接受影响21根(6U15S)、零skip全部PASS：Root9真实gate/peer/
ACK/错tracer/同uid信号/原30秒绝对超时/M-N0核准/纯audit/ancillary，兼容11含
默认off、原R/logger/恢复3次、原ACL/definer、真实PM leader/pthread/ready四错误与
child syscall边界；机制新增1S根实际13syscall均EPERM，与无filter的全部不同内核
结果对照。Root9/兼容11的结果内新PG ELF均62757f73...，未借旧binary通过。
机制只编独立probe.so，不改PG扩展测试模块生产C；C源/runner/命令/结果全部在此。
它不执行真实页面迁移，不以空参数错误当物理迁移安全证明。首次旧探针因模块lib路径
错误启动失败保留在/data/.../pg17-migration-filter-mechanism-20261031T014317Z-f3d499。

S均为Host6.8 Linux隔离原生组件，不是准确369Guest/KVM。Root9的Root oracle是
组件fixture，未替代正式Root Source/ELF/cgroup/WAL。BPF字节已变化，RootGo tracer
真实Host与369Guest须按新image/ELF另跑，未在本批声称通过。Root/System/未来
RootArrayHelper不受PGfilter覆盖，其migration/GUP/DirectIO/uring/fork caller集合仍
UNKNOWN；这条deny不补持续force/I来源，不签normal许可，五拒绝未动。
