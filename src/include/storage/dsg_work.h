/*-------------------------------------------------------------------------
 * dsg_work.h -- 同一PG boot的真实进程/前台工作观察，独立于Buffer ABI6。
 *
 * 前台空闲只说明已登记的消息/命令/保留事务，不是完整Quiesce证书；启动前
 * 连接、认证、后台/DSM、外部预算和VM栅栏必须分别有实际覆盖，不能由本值推断。
 *-------------------------------------------------------------------------
 */
#ifndef DSG_WORK_H
#define DSG_WORK_H

#include "storage/dynamic_shared_buffers.h"

#define NEON_DSG_WORK_API_VERSION 1

typedef struct DsgWorkSnapshot
{
	DsgBufferSnapshot observed;
	uint32 client_processes;
	uint32 startup_clients;
	uint32 idle_clients;
	uint32 frontend_reads;
	uint32 frontend_executes;
	uint32 bounded_inputs;
	uint32 retained_transactions;
	uint32 background_processes;
	uint32 auxiliary_processes;
	bool observer_excluded;
	bool foreground_idle;
} DsgWorkSnapshot;

extern Size DsgWorkShmemSize(void);
extern void DsgWorkShmemInit(void);
extern void DsgWorkAttach(bool auxiliary);
extern void DsgWorkDetach(void);
extern bool DsgWorkStartMessage(bool ordinary_work, bool bounded_discard);
extern void DsgWorkStartExecution(void);
extern void DsgWorkIdle(bool retained_transaction);
extern void DsgWorkGetSnapshot(DsgWorkSnapshot *snapshot);

#endif
