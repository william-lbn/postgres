/*-------------------------------------------------------------------------
 * dynamic_shared_buffers.c -- DSGA Buffer Adapter 的共享容量目录。
 *
 * 本目录始终位于不回收的主共享内存，数组本体由普通页 Provider 预留。
 * 不能将目录、锁或身份放入可回收的尾部 Granule。
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>

#include "miscadmin.h"
#include "storage/buf_internals.h"
#include "storage/dynamic_shared_buffers.h"
#include "storage/pg_shmem.h"
#include "storage/shmem.h"
#include "utils/guc.h"

bool		enable_dynamic_shared_buffers = false;
int			max_shared_buffers = 0;
bool		dynamic_shared_buffers_require_admission = false;
int			dynamic_shared_buffers_startup_budget = 0;
DynamicSharedBuffersControl *DSBCtrl = NULL;
static Size startup_shared_upper_bytes;

/*-
 * 在主共享段及 preload 扩展请求全部计入后，额外计入四个独立数组的
 * bootstrap页对齐成本。按M固定的hash/目录已在主段，不能重复计费。
 * 这是PG共享内存预算上界，不是Guest总量/RSS或私有查询内存的硬上限。
 */
void
DsgBufferCheckStartupBudget(Size main_segment_bytes)
{
	Size		bytes = main_segment_bytes;
	Size		main_page_bytes;
	long		page_bytes;
	Size		elements[] = {sizeof(BufferDescPadded), BLCKSZ,
	sizeof(ConditionVariableMinimallyPadded), sizeof(CkptSortItem)};
	int			i;

	if (!enable_dynamic_shared_buffers)
	{
		if (dynamic_shared_buffers_require_admission || dynamic_shared_buffers_startup_budget != 0)
			ereport(FATAL, (errmsg("managed dynamic buffer settings require dynamic shared buffers")));
		return;
	}
	if (dynamic_shared_buffers_require_admission && dynamic_shared_buffers_startup_budget == 0)
		ereport(FATAL, (errmsg("managed dynamic buffers require an explicit startup shared-memory budget")));
	page_bytes = sysconf(_SC_PAGESIZE);
	if (page_bytes <= 0)
		ereport(FATAL, (errmsg("could not determine dynamic buffer page size")));
	main_page_bytes = page_bytes;
	/*-
	 * Linux上huge_pages=try仍可能把主共享段向巨页取整。四个可伸缩数组
	 * 使用普通页不代表主共享段也一定是普通页；启动前按两种分配的较大
	 * 页界计费，失败回退只会使预算更保守。mmap模式另有一个SysV身份页。
	 */
	if (shared_memory_type == SHMEM_TYPE_MMAP)
	{
		Size		huge_bytes = 0;

		if (huge_pages != HUGE_PAGES_OFF)
			GetHugePageSize(&huge_bytes, NULL);
		main_page_bytes = Max(main_page_bytes, huge_bytes);
		bytes = add_size(bytes, (Size) page_bytes);
	}
	if (main_segment_bytes % main_page_bytes != 0)
		bytes = add_size(bytes, main_page_bytes - main_segment_bytes % main_page_bytes);
	for (i = 0; i < lengthof(elements); i++)
	{
		Size		array_bytes = mul_size(NBuffers, elements[i]);

		/*-
		 * 加法先检查溢出，再向下对齐已加padding的长度，得到安全ceil。
		 */
		bytes = add_size(bytes, TYPEALIGN_DOWN((Size) page_bytes,
											   add_size(array_bytes, (Size) page_bytes - 1)));
	}
	startup_shared_upper_bytes = bytes;
	if (dynamic_shared_buffers_startup_budget > 0 &&
		bytes > mul_size(dynamic_shared_buffers_startup_budget, 1024))
		ereport(FATAL,
				(errcode(ERRCODE_CONFIGURATION_LIMIT_EXCEEDED),
				 errmsg("dynamic buffer startup layout exceeds the shared-memory budget"),
				 errdetail("Layout requires %zu bytes; budget is %d kB.", bytes,
						   dynamic_shared_buffers_startup_budget)));
}

/*-
 * 新boot在普通命令执行/解析前拒绝准入，而非等待Monitor发现重启后再关门。
 * 返回正常SQL ERROR，不主动终止连接；内部PG超级用户保留恢复/管理通道。
 * 此处是启动门禁，不替代运行时私有预算、所有后台工作排空和VM执行栅栏。
 */
void
DsgBufferCheckAdmission(void)
{
	/*
	 * 消息入口尚未开始事务；使用PG随SET ROLE维护的只读权限标志，不查系统表。
	 */
	if (DSBCtrl != NULL && pg_atomic_read_u32(&DSBCtrl->admission_open) == 0 &&
		!current_role_is_superuser)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("dynamic shared buffer admission is closed for this boot")));
}

/*-
 * 业务REPLICATION属性只用于逻辑复制，不能授予全实例物理WAL/备份/物理槽权限。
 * 物理数据包含全局认证目录，仅有SQL列ACL无法隔离。当前及session身份均须为
 * 内部SU；使用初始化/SET ROLE维护的缓存标志，不启动事务或读取系统目录，
 * 因而适用于没有数据库的physical walsender以及持槽ControlLock的调用点。
 * 调用方不得持spinlock；ERROR按PG标准清理LWLock，须发生在槽认领/写入前。
 * 非DSGA保持官方行为。此检查不代替私有HBA/镜像认证、逻辑槽预算或流式排空。
 */
void
DsgBufferCheckPhysicalReplication(void)
{
	if (enable_dynamic_shared_buffers &&
		(!current_role_is_superuser || !GetSessionUserIsSuperuser()))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("dynamic shared buffer physical replication requires internal management privileges")));
}

void
DsgBufferSetAdmission(uint64 boot_id, uint64 revision, uint64 expected_generation, bool open)
{
	bool		valid;

	if (!superuser())
		ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
						errmsg("dynamic buffer admission requires internal management privileges")));
	if (DSBCtrl == NULL)
		ereport(ERROR, (errmsg("dynamic shared buffers are disabled")));
	SpinLockAcquire(&DSBCtrl->status_lock);
	valid = boot_id == DSBCtrl->boot_id && revision > 0 &&
		expected_generation == pg_atomic_read_u64(&DSBCtrl->generation) &&
		(revision > DSBCtrl->admission_revision ||
		 (revision == DSBCtrl->admission_revision &&
		  open == (pg_atomic_read_u32(&DSBCtrl->admission_open) != 0)));
	if (valid)
	{
		DSBCtrl->admission_revision = revision;
		pg_atomic_write_u32(&DSBCtrl->admission_open, open ? 1 : 0);
	}
	SpinLockRelease(&DSBCtrl->status_lock);
	if (!valid)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("invalid or stale dynamic buffer admission request")));
}

/*-
 * 新 GUC 只在 Postmaster 启动时生效。开关关闭时完全保留原布局；未指定 M
 * 时以启动 shared_buffers 为 M。这里也会由共享内存估算路径调用，因此
 * 必须在任何乘法/分配前检查上限，不能等初始化数组时才发现 M 小于初值。
 */
int
GetMaxNBuffers(void)
{
	if (!enable_dynamic_shared_buffers)
		return NBuffers;
	if (DSBCtrl != NULL)
		return DSBCtrl->maximum;

#if defined(EXEC_BACKEND) || defined(WIN32)
	ereport(FATAL,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("dynamic shared buffers require fork-inherited shared mappings")));
#endif

	/*
	 * 普通页 Provider 不接受以 huge_pages=on 声明整池巨页的配置。
	 */
	if (huge_pages == HUGE_PAGES_ON)
		ereport(FATAL,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("dynamic shared buffers do not yet support explicit huge pages")));
	if (max_shared_buffers != 0 && max_shared_buffers < NBuffers)
		ereport(FATAL,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("max_shared_buffers must not be less than shared_buffers")));

	return max_shared_buffers == 0 ? NBuffers : max_shared_buffers;
}

int
GetLowNBuffers(void)
{
	return DSBCtrl == NULL ? NBuffers : (int) pg_atomic_read_u32(&DSBCtrl->low);
}

int
GetHighNBuffers(void)
{
	return DSBCtrl == NULL ? NBuffers : (int) pg_atomic_read_u32(&DSBCtrl->high);
}

Size
DynamicSharedBuffersShmemSize(void)
{
	Size		size;
	int			count;

	if (!enable_dynamic_shared_buffers)
		return 0;
	count = 1 + (GetMaxNBuffers() - 1) / DSG_BUFFER_GRANULE_BUFFERS;
	size = add_size(offsetof(DynamicSharedBuffersControl, granules),
					mul_size(count, sizeof(DsgBufferGranule)));
	return add_size(MAXALIGN(size), PG_CACHE_LINE_SIZE);
}

void
DynamicSharedBuffersInit(void)
{
	bool		found;
	int			maximum;
	int			i;
	Size		size;

	if (!enable_dynamic_shared_buffers)
		return;
	maximum = GetMaxNBuffers();
	size = DynamicSharedBuffersShmemSize();
	DSBCtrl = ShmemInitStruct("DSGA Buffer Directory", size, &found);

	/*-
	 * EXEC_BACKEND 已拒绝；fork 子进程继承地址，无需二次 attach。
	 */
	Assert(!found);
	MemSet(DSBCtrl, 0, size);
	pg_atomic_init_u32(&DSBCtrl->low, NBuffers);
	pg_atomic_init_u32(&DSBCtrl->high, NBuffers);
	pg_atomic_init_u32(&DSBCtrl->admission_open, dynamic_shared_buffers_require_admission ? 0 : 1);
	DSBCtrl->startup_shared_upper_bytes = startup_shared_upper_bytes;
	DSBCtrl->maximum = maximum;

	/*
	 *
	 * 0在跨语言控制契约中表示未绑定，不能把极低概率的0随机值当作合法boot。
	 */
	do
	{
		if (!pg_strong_random(&DSBCtrl->boot_id, sizeof(DSBCtrl->boot_id)))
			ereport(FATAL,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("could not generate DSGA shared memory boot identity")));
	} while (DSBCtrl->boot_id == 0);
	LWLockInitialize(&DSBCtrl->access_lock, LWTRANCHE_DSG_BUFFER_ACCESS);
	LWLockInitialize(&DSBCtrl->checkpoint_lock, LWTRANCHE_DSG_BUFFER_CHECKPOINT);
	pg_atomic_init_u32(&DSBCtrl->checkpoint_prepared_buffers, NBuffers);
	LWLockInitialize(&DSBCtrl->resize_lock, LWTRANCHE_DSG_BUFFER_RESIZE);
	SpinLockInit(&DSBCtrl->status_lock);
	pg_atomic_init_u64(&DSBCtrl->generation, 1);
	DSBCtrl->granule_count = 1 + (maximum - 1) / DSG_BUFFER_GRANULE_BUFFERS;
	for (i = 0; i < DSBCtrl->granule_count; i++)
	{
		DsgBufferGranule *granule = &DSBCtrl->granules[i];

		granule->first_buffer = i * DSG_BUFFER_GRANULE_BUFFERS;
		granule->maximum_buffers = Min(DSG_BUFFER_GRANULE_BUFFERS,
									   maximum - granule->first_buffer);
		granule->active_buffers = Min(granule->maximum_buffers,
									  Max(0, NBuffers - granule->first_buffer));
		granule->prepared_buffers = granule->active_buffers;
		granule->generation = 1;
		granule->state = granule->active_buffers > 0 ? DSG_GRANULE_ACTIVE : DSG_GRANULE_RESERVED;
	}
}

DsgBufferAccess
DsgBufferAccessBegin(void)
{
	DsgBufferAccess access;

	access.acquired = DSBCtrl != NULL && !LWLockHeldByMe(&DSBCtrl->access_lock);
	if (access.acquired)
	{
		LWLockAcquire(&DSBCtrl->access_lock, LW_SHARED);
		/*-
		 * 普通 LWLock 会一直 HOLD_INTERRUPTS，但数组遍历可能很长，不能
		 * 因只读生命周期保护而屏蔽查询取消。仅抵消本锁增加的 holdoff，
		 * 原有 header/mapping/其他锁的中断保护不受影响。ERROR/FATAL
		 * 仍由 PG 的 LWLockReleaseAll 释放；它自身会平衡每次 release，
		 * 不依赖当前 holdoff 必须与已持锁数量相等。
		 */
		RESUME_INTERRUPTS();
	}
	access.count = GetHighNBuffers();
	return access;
}

void
DsgBufferAccessEnd(DsgBufferAccess *access)
{
	if (access->acquired)
	{
		/*-
		 * 与 Begin 抵消成对，恢复原生 LWLockRelease 所需的 holdoff。
		 */
		HOLD_INTERRUPTS();
		LWLockRelease(&DSBCtrl->access_lock);
		access->acquired = false;
	}
}
