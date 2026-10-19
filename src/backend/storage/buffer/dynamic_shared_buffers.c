/*-------------------------------------------------------------------------
 * dynamic_shared_buffers.c -- DSGA Buffer Adapter 的共享容量目录。
 *
 * 本目录始终位于不回收的主共享内存，数组本体由普通页 Provider 预留。
 * 不能将目录、锁或身份放入可回收的尾部 Granule。
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "miscadmin.h"
#include "storage/dynamic_shared_buffers.h"
#include "storage/pg_shmem.h"
#include "storage/shmem.h"

bool		enable_dynamic_shared_buffers = false;
int			max_shared_buffers = 0;
DynamicSharedBuffersControl *DSBCtrl = NULL;

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
	return enable_dynamic_shared_buffers ?
		add_size(MAXALIGN(sizeof(*DSBCtrl)), PG_CACHE_LINE_SIZE) : 0;
}

void
DynamicSharedBuffersInit(void)
{
	bool		found;
	int			maximum;

	if (!enable_dynamic_shared_buffers)
		return;
	maximum = GetMaxNBuffers();
	DSBCtrl = ShmemInitStruct("DSGA Buffer Directory", sizeof(*DSBCtrl), &found);

	/*
	 * EXEC_BACKEND 已拒绝；fork 子进程继承地址，无需二次
	 * attach。
	 */
	Assert(!found);
	MemSet(DSBCtrl, 0, sizeof(*DSBCtrl));
	pg_atomic_init_u32(&DSBCtrl->low, NBuffers);
	pg_atomic_init_u32(&DSBCtrl->high, NBuffers);
	DSBCtrl->maximum = maximum;
	if (!pg_strong_random(&DSBCtrl->boot_id, sizeof(DSBCtrl->boot_id)))
		ereport(FATAL,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("could not generate DSGA shared memory boot identity")));
	LWLockInitialize(&DSBCtrl->access_lock, LWTRANCHE_DSG_BUFFER_ACCESS);
}

DsgBufferAccess
DsgBufferAccessBegin(void)
{
	DsgBufferAccess access;

	access.acquired = DSBCtrl != NULL && !LWLockHeldByMe(&DSBCtrl->access_lock);
	if (access.acquired)
		LWLockAcquire(&DSBCtrl->access_lock, LW_SHARED);
	access.count = GetHighNBuffers();
	return access;
}

void
DsgBufferAccessEnd(DsgBufferAccess *access)
{
	if (access->acquired)
	{
		LWLockRelease(&DSBCtrl->access_lock);
		access->acquired = false;
	}
}
