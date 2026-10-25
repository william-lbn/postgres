/*-------------------------------------------------------------------------
 * dsg_dsm.h -- 同一 PG boot 外部 DSM payload 的分配前预算登记。
 *
 * 主段内 DSM 已在固定主段成本中计费；本接口不按每个 mapping 重复计费。
 * 独立能力不改变 Buffer6/Work2/Connection1/Prepared1 已发布结构的 ABI。
 *-------------------------------------------------------------------------
 */
#ifndef DSG_DSM_H
#define DSG_DSM_H

#include "storage/dsg_work.h"
#include "storage/dsm_impl.h"

#define NEON_DSG_DSM_API_VERSION 1

extern PGDLLIMPORT int dynamic_shared_buffers_dsm_budget;

/* 所有计数在 status_lock 同点复制；unknown 是仍包含在 reserved 中的子集。 */
typedef struct DsgDsmCounters
{
	uint64 reserved_upper_bytes;
	uint64 control_upper_bytes;
	uint64 external_budget_bytes;
	uint32 external_segments;
	uint32 pending_creations;
	uint32 unknown_segments;
	uint32 page_bytes;
	bool limit_enforced;
	bool accounting_valid;
} DsgDsmCounters;

typedef struct DsgDsmSnapshot
{
	DsgPreparedSnapshot prepared;
	DsgDsmCounters dsm;
} DsgDsmSnapshot;

extern Size DsgDsmShmemSize(void);
extern void DsgDsmShmemInit(void);
extern void DsgDsmSetControlSize(Size bytes);
/* 返回 -1 仅表示候选 handle 已被本登记表占用；没有 OS 动作，调用方可换号。 */
extern int DsgDsmBeginCreate(dsm_handle handle, Size bytes);
extern void DsgDsmCreated(int token, Size mapped_size);
extern void DsgDsmCreateCollision(int token);
extern void DsgDsmCreateError(int token, bool os_started);
/* 仅由原生控制表发布路径持 DynamicSharedMemoryControlLock 调用。 */
extern void DsgDsmPublishLocked(dsm_handle handle);
extern void DsgDsmAfterOp(dsm_handle handle, dsm_op op, bool success);
/* 调用方已持 status_lock，不取得任何 LWLock/IO/内存，不扫描槽。 */
extern void DsgDsmCopyLocked(DsgDsmCounters *counters);
extern void DsgDsmGetSnapshot(DsgDsmSnapshot *snapshot);

#endif
