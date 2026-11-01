/* PG18 Work2：与PG17同一观察合同；不是MemoryContext硬预算或VM许可。 */
#ifndef DSG_WORK_H
#define DSG_WORK_H
#include "storage/dynamic_shared_buffers.h"

/* 仅实际fork生命周期适配支持；EXEC_BACKEND受管模式显式拒绝，不伪报能力。 */
#ifndef EXEC_BACKEND
#define NEON_DSG_WORK_API_VERSION 2
#endif

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
extern void DsgWorkPreStart(int child_slot);
extern void DsgWorkPreForget(int child_slot);
extern void DsgWorkAttach(bool auxiliary);
extern void DsgWorkDetach(void);
extern bool DsgWorkStartMessage(bool ordinary_work, bool bounded_discard);
extern void DsgWorkStartExecution(void);
extern void DsgWorkIdle(bool retained_transaction);
extern void DsgWorkGetSnapshot(DsgWorkSnapshot *snapshot);
#endif
