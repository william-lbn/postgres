/* PG18 Work2：与PG17同一观察合同；不是MemoryContext硬预算或VM许可。 */
#ifndef DSG_WORK_H
#define DSG_WORK_H
#include "storage/dynamic_shared_buffers.h"

/* 仅实际fork生命周期适配支持；EXEC_BACKEND受管模式显式拒绝，不伪报能力。 */
#ifndef EXEC_BACKEND
#define NEON_DSG_WORK_API_VERSION 2
#define NEON_DSG_CONNECTION_API_VERSION 1
#define NEON_DSG_PREPARED_API_VERSION 1
#endif

extern PGDLLIMPORT bool dynamic_shared_buffers_require_connection_gate;

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

/* 同一status观察点的工作及Postmaster门禁应答，不是完整Quiesce或VM许可。 */
typedef struct DsgConnectionSnapshot
{
	DsgWorkSnapshot work;
	uint32 requested_serial;
	uint32 applied_serial;
	bool enabled;
	bool requested_open;
	bool applied_open;
	bool acknowledged;
	bool private_management_ready;
	bool public_connections_blocked;
} DsgConnectionSnapshot;

/* 独立API，保留已发布Work2/Connection1的结构和调用ABI。含尚未valid的保留槽。 */
typedef struct DsgPreparedSnapshot
{
	DsgConnectionSnapshot connections;
	uint32 prepared_slots;
	uint32 maximum_prepared_slots;
} DsgPreparedSnapshot;

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
/* Publish只在status_lock内调用，不分配/等待/报错；Notify必须在锁外调用。 */
extern bool DsgConnectionPublishLocked(bool open);
extern void DsgConnectionNotify(void);
extern void DsgConnectionPostmasterRefresh(void);
extern bool DsgConnectionAccept(pgsocket socket);
extern void DsgConnectionGetSnapshot(DsgConnectionSnapshot *snapshot);
/* 调用者持TwoPhaseStateLock；只在同status锁内登记，不分配/IO/等待LWLock。 */
extern bool DsgPreparedReserve(bool recovery);
extern void DsgPreparedForget(void);
extern void DsgPreparedGetSnapshot(DsgPreparedSnapshot *snapshot);

#endif
