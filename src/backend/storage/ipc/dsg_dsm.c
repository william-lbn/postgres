/*-------------------------------------------------------------------------
 * dsg_dsm.c -- 外部 DSM 创建前预留；真实释放前不减少保守 payload 上界。
 *
 * 不修改原生 DSM 控制段格式，避免重启旧控制段清理解码失配。固定主段元数据
 * 由启动预算计费。默认不限制 DSM；只有显式配置的本 boot 正数额度持续生效。
 * 该限制不包含 PTE/内核/THP/Host，也不能代替跨 boot 的 root Held/Original。
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>

#include "miscadmin.h"
#include "storage/dsg_dsm.h"
#include "storage/dsm.h"
#include "storage/proc.h"
#include "storage/shmem.h"

int dynamic_shared_buffers_dsm_budget = 0;

typedef enum DsgDsmState
{
	DSG_DSM_EMPTY,
	DSG_DSM_CREATING,
	DSG_DSM_RESIDENT,
	DSG_DSM_UNKNOWN
} DsgDsmState;

typedef struct DsgDsmSlot
{
	dsm_handle handle;
	DsgDsmState state;
	uint64 upper_bytes;
	bool created;
	bool mapping_unknown;
} DsgDsmSlot;

typedef struct DsgDsmControl
{
	uint64 boot_id;
	DsgDsmCounters counters;
	DsgDsmSlot slots[FLEXIBLE_ARRAY_MEMBER];
} DsgDsmControl;

static DsgDsmControl *dsm_registry;
/* 本地容量由不可变启动参数计算，不信任共享段长度决定数组访问边界。 */
static uint32 registry_capacity;
static Size accounting_page_bytes;

static uint32
DsgDsmCapacity(void)
{
	/*-
	 * 原生控制槽之外，每个活进程可有正在 OS 分配、尚未发布的段。未知记录
	 * 也占槽；耗尽时拒绝新创建，不能覆盖未知以换取可用性。额外固定余量给
	 * 控制/扩展嵌套创建，但不声称任意深递归无限可用。
	 */
	return dsm_control_slot_capacity() + MaxBackends + NUM_AUXILIARY_PROCS;
}

Size
DsgDsmShmemSize(void)
{
	if (!enable_dynamic_shared_buffers)
		return 0;
	return add_size(offsetof(DsgDsmControl, slots),
		mul_size(DsgDsmCapacity(), sizeof(DsgDsmSlot)));
}

void
DsgDsmShmemInit(void)
{
	bool found;
	long page_size;
	Size bytes;
	bool supported;

	if (!enable_dynamic_shared_buffers)
		return;
	if (DSBCtrl == NULL)
		elog(FATAL, "DSGA DSM registry requires initialized buffer state");
	page_size = sysconf(_SC_PAGESIZE);
	if (page_size <= 0 || page_size > PG_UINT32_MAX ||
		(page_size & (page_size - 1)) != 0)
		elog(FATAL, "DSGA DSM accounting requires a valid OS page size");
	accounting_page_bytes = (Size) page_size;
	registry_capacity = DsgDsmCapacity();
	supported = dynamic_shared_memory_type == DSM_IMPL_POSIX ||
		dynamic_shared_memory_type == DSM_IMPL_SYSV ||
		dynamic_shared_memory_type == DSM_IMPL_MMAP;
	if (dynamic_shared_buffers_dsm_budget > 0 && !supported)
		elog(FATAL, "DSGA DSM budget does not support this DSM implementation");
	bytes = DsgDsmShmemSize();
	dsm_registry = ShmemInitStruct("DSGA DSM Registry", bytes, &found);
	if (!found)
	{
		MemSet(dsm_registry, 0, bytes);
		dsm_registry->boot_id = DSBCtrl->boot_id;
		dsm_registry->counters.page_bytes = (uint32) accounting_page_bytes;
		dsm_registry->counters.external_budget_bytes =
			(uint64) dynamic_shared_buffers_dsm_budget * 1024;
		dsm_registry->counters.limit_enforced = dynamic_shared_buffers_dsm_budget > 0;
		dsm_registry->counters.accounting_valid = supported;
	}
	if (dsm_registry->boot_id != DSBCtrl->boot_id)
		elog(FATAL, "DSGA DSM registry does not match this PG boot");
}

static uint64
DsgDsmRoundBytes(Size bytes)
{
	if (bytes == 0)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("DSGA DSM size must be positive")));
	if (bytes > SIZE_MAX - (accounting_page_bytes - 1))
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						errmsg("DSGA DSM size exceeds accounting range")));
	return TYPEALIGN(accounting_page_bytes, bytes);
}

/*-
 * Postmaster 启动、尚无 child 时登记固定控制段；运行期 Postmaster 仍不取
 * backend LWLock/spinlock。该段独立于 main/外部 payload cap，不能漏算或双算。
 */
void
DsgDsmSetControlSize(Size bytes)
{
	if (enable_dynamic_shared_buffers)
		dsm_registry->counters.control_upper_bytes = DsgDsmRoundBytes(bytes);
}

/* 所有槽操作先持原生 DynamicSharedMemoryControlLock，再持 status_lock。 */
static DsgDsmSlot *
DsgDsmFindLocked(dsm_handle handle)
{
	uint32 i;

	Assert(LWLockHeldByMe(DynamicSharedMemoryControlLock));
	for (i = 0; i < registry_capacity; ++i)
		if (dsm_registry->slots[i].state != DSG_DSM_EMPTY &&
			dsm_registry->slots[i].handle == handle)
			return &dsm_registry->slots[i];
	return NULL;
}

static void
DsgDsmUnknownLocked(DsgDsmSlot *slot)
{
	/* 损坏不能在关闭 assert 的生产构建中下溢成“空闲”；撤销整个预算有效性。 */
	if (slot->state == DSG_DSM_EMPTY ||
		(slot->state == DSG_DSM_CREATING && dsm_registry->counters.pending_creations == 0))
	{
		dsm_registry->counters.accounting_valid = false;
		return;
	}
	if (slot->state == DSG_DSM_UNKNOWN)
		return;
	if (slot->state == DSG_DSM_CREATING)
		dsm_registry->counters.pending_creations--;
	dsm_registry->counters.unknown_segments++;
	slot->state = DSG_DSM_UNKNOWN;
}

static void
DsgDsmForgetLocked(DsgDsmSlot *slot)
{
	DsgDsmCounters *c = &dsm_registry->counters;

	if (slot->state == DSG_DSM_EMPTY || c->reserved_upper_bytes < slot->upper_bytes ||
		c->external_segments == 0 ||
		(slot->state == DSG_DSM_CREATING && c->pending_creations == 0) ||
		(slot->state == DSG_DSM_UNKNOWN && c->unknown_segments == 0))
	{
		c->accounting_valid = false;
		return;
	}
	c->reserved_upper_bytes -= slot->upper_bytes;
	c->external_segments--;
	if (slot->state == DSG_DSM_CREATING)
		c->pending_creations--;
	else if (slot->state == DSG_DSM_UNKNOWN)
		c->unknown_segments--;
	MemSet(slot, 0, sizeof(*slot));
}

int
DsgDsmBeginCreate(dsm_handle handle, Size bytes)
{
	uint64 upper = DsgDsmRoundBytes(bytes);
	DsgDsmCounters *c = &dsm_registry->counters;
	int token = -1;
	uint32 i;
	bool denied = false;

	LWLockAcquire(DynamicSharedMemoryControlLock, LW_EXCLUSIVE);
	if (DsgDsmFindLocked(handle) != NULL)
	{
		LWLockRelease(DynamicSharedMemoryControlLock);
		return -1;
	}
	for (i = 0; i < registry_capacity; ++i)
		if (dsm_registry->slots[i].state == DSG_DSM_EMPTY)
		{
			token = (int) i;
			break;
		}
	SpinLockAcquire(&DSBCtrl->status_lock);
	if (token < 0 || !c->accounting_valid ||
		upper > PG_UINT64_MAX - c->reserved_upper_bytes ||
		(c->limit_enforced &&
		 (c->reserved_upper_bytes > c->external_budget_bytes ||
		  upper > c->external_budget_bytes - c->reserved_upper_bytes)))
		denied = true;
	else
	{
		DsgDsmSlot *slot = &dsm_registry->slots[token];

		slot->handle = handle;
		slot->state = DSG_DSM_CREATING;
		slot->upper_bytes = upper;
		c->reserved_upper_bytes += upper;
		c->external_segments++;
		c->pending_creations++;
	}
	SpinLockRelease(&DSBCtrl->status_lock);
	LWLockRelease(DynamicSharedMemoryControlLock);
	/* 未分配任何 OS 段；包括 SU/后台在内均受同一额度，错误在全部锁外。 */
	if (denied)
		ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_RESOURCES),
						errmsg("DSGA external DSM budget or registry capacity is exhausted")));
	return token;
}

void
DsgDsmCreated(int token, Size mapped_size)
{
	DsgDsmSlot *slot;
	uint64 actual = mapped_size == 0 ? 0 : DsgDsmRoundBytes(mapped_size);
	bool invalid;

	Assert(token >= 0 && (uint32) token < registry_capacity);
	LWLockAcquire(DynamicSharedMemoryControlLock, LW_EXCLUSIVE);
	slot = &dsm_registry->slots[token];
	if (slot->state != DSG_DSM_CREATING)
		elog(PANIC, "DSGA DSM allocation lost its original reservation");
	SpinLockAcquire(&DSBCtrl->status_lock);
	invalid = mapped_size == 0 || actual > slot->upper_bytes;
	if (invalid)
	{
		/*-
		 * 当前支持实现应返回不超过预留的映射。若契约失效，保留已知实际上界、
		 * 撤销有效预算；不能截小数值伪装符合 cap。未再派发新的 OS 创建。
		 */
		if (actual > slot->upper_bytes)
		{
			uint64 extra = actual - slot->upper_bytes;

			if (extra > PG_UINT64_MAX - dsm_registry->counters.reserved_upper_bytes)
				dsm_registry->counters.reserved_upper_bytes = PG_UINT64_MAX;
			else
				dsm_registry->counters.reserved_upper_bytes += extra;
			slot->upper_bytes = actual;
		}
		dsm_registry->counters.accounting_valid = false;
		DsgDsmUnknownLocked(slot);
	}
	else
		slot->created = true;
	SpinLockRelease(&DSBCtrl->status_lock);
	LWLockRelease(DynamicSharedMemoryControlLock);
	if (invalid)
		elog(ERROR, "DSGA DSM implementation exceeded its allocation reservation");
}

void
DsgDsmCreateCollision(int token)
{
	Assert(token >= 0 && (uint32) token < registry_capacity);
	LWLockAcquire(DynamicSharedMemoryControlLock, LW_EXCLUSIVE);
	SpinLockAcquire(&DSBCtrl->status_lock);
	/* 原生 O_EXCL 明确无本次效果；不销毁撞名对象，只撤销本次预留。 */
	DsgDsmForgetLocked(&dsm_registry->slots[token]);
	SpinLockRelease(&DSBCtrl->status_lock);
	LWLockRelease(DynamicSharedMemoryControlLock);
}

void
DsgDsmCreateError(int token, bool os_started)
{
	Assert(token >= 0 && (uint32) token < registry_capacity);
	LWLockAcquire(DynamicSharedMemoryControlLock, LW_EXCLUSIVE);
	SpinLockAcquire(&DSBCtrl->status_lock);
	if (os_started)
		DsgDsmUnknownLocked(&dsm_registry->slots[token]);
	else
		DsgDsmForgetLocked(&dsm_registry->slots[token]);
	SpinLockRelease(&DSBCtrl->status_lock);
	LWLockRelease(DynamicSharedMemoryControlLock);
}

void
DsgDsmPublishLocked(dsm_handle handle)
{
	DsgDsmSlot *slot;

	if (!enable_dynamic_shared_buffers)
		return;
	slot = DsgDsmFindLocked(handle);
	if (slot == NULL || slot->state != DSG_DSM_CREATING || !slot->created)
		elog(PANIC, "DSGA DSM publication lost its original reservation");
	SpinLockAcquire(&DSBCtrl->status_lock);
	dsm_registry->counters.pending_creations--;
	slot->state = DSG_DSM_RESIDENT;
	SpinLockRelease(&DSBCtrl->status_lock);
}

void
DsgDsmAfterOp(dsm_handle handle, dsm_op op, bool success)
{
	DsgDsmSlot *slot;

	Assert(op == DSM_OP_DETACH || op == DSM_OP_DESTROY);
	LWLockAcquire(DynamicSharedMemoryControlLock, LW_EXCLUSIVE);
	slot = DsgDsmFindLocked(handle);
	if (slot != NULL)
	{
		SpinLockAcquire(&DSBCtrl->status_lock);
		if (!success)
		{
			/* 解除映射失败后，unlink 成功也不能证明仍存 mapping 已释放。 */
			slot->mapping_unknown = true;
			DsgDsmUnknownLocked(slot);
		}
		else if (op == DSM_OP_DESTROY)
		{
			if (slot->mapping_unknown)
				DsgDsmUnknownLocked(slot);
			else
				DsgDsmForgetLocked(slot);
		}
		SpinLockRelease(&DSBCtrl->status_lock);
	}
	LWLockRelease(DynamicSharedMemoryControlLock);
}

void
DsgDsmCopyLocked(DsgDsmCounters *counters)
{
	*counters = dsm_registry->counters;
}
