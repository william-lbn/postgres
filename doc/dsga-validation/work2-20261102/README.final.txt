Work2 本批最终直接验证补充（沿原十五源冻结，仅增一份Python）

独立审查确认原五目标以Q路径为主，因此未把静态与17相等当作扩展协议已测。保持原
1df69850 PG/b40bfcd1 DSO及14编译/安装输入逐SHA相同，不重编、不重跑原五目标，新增
35811d实际PG两根六子例，5.885秒PASS/零skip。

005的P/B/D/E各独立场景：open实际Parse/Bind/Describe/Execute并取nextval=1，closed
只发送原类型/长度及首byte，实际Work snapshot先看到bounded_inputs=1、frontend_reads=0，
再发送剩余正文+Sync，真实收到55000并恢复同连接。closed Execute不得递增非事务序列，
reopen实际取2；SQL pg_backend_pid核仍为原wire pid。事务内Execute错误后保留aborted事务，
原closed ROLLBACK真实结束，不把断开/重连冒作协议恢复。

006同时验证Query及closed extended unnamed Parse→DescribeStmt→Bind→DescribePortal→
Execute结束事务链：完整body恰512B允许ROLLBACK，513B必须55000拒绝并保留事务，
随后原ROLLBACK恢复。两边界后真实Work观察无retained/bounded，reopen查询仍正常。
实际阶段、长度、原PID和snapshot在extended-supplement/evidence，不只记录静态期待。

本批最终7个稳定直接目标=1U-native+6S，共8子例、零skip；原初五根中的失败及补齐
checkpoint后单根恢复证据仍按原README/catalog保留，旧ELF两个ENV对照不累计为Work目标。
新增Python不改变任何内核实现或已冻结十五源。独审小报告附后，Root等最终审阅后同原
PG18分支提交；不动PG17或发行锁。启用standalone/EXEC_BACKEND变体仍只有静态或拒绝
设计证据；不称所有协议/OS/故障组合都已认证。MemoryContext硬预算、DSM/Prepared、
required connection gate、全Neon18/RM134新写WAL恢复、Root Source/normal及K8s仍未完成。
