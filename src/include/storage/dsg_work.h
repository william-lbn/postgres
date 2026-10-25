/*-------------------------------------------------------------------------
 * dsg_work.h -- 同一PG boot的真实进程/前台工作观察，独立于Buffer ABI6。
 *
 * Work2含Postmaster fork前登记、启动包/SSL/GSS及PGPROC交接；前台空闲仍是
 * 观察点，不是禁止后续新fork的ack或完整Quiesce。后台/DSM、外部持续预算和
 * VM栅栏必须分别有实际覆盖，不能由本值推断。
 *-------------------------------------------------------------------------
 */
#ifndef DSG_WORK_H
#define DSG_WORK_H

#include "storage/dynamic_shared_buffers.h"

#define NEON_DSG_WORK_API_VERSION 2
#define NEON_DSG_CONNECTION_API_VERSION 1
#define NEON_DSG_PREPARED_API_VERSION 1

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
/* Postmaster专用：只用原生32位原子，不取得backend共享锁，不在早期child清理。 */
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
