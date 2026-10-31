/*-------------------------------------------------------------------------
 *
 * buf_init.c
 *	  buffer manager initialization routines
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/buffer/buf_init.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "storage/buf_internals.h"
#include "storage/bufmgr.h"
#include "storage/dynamic_shared_buffers.h"
#include "storage/dsg_envelope_birth.h"
#include "storage/dsg_array_root.h"
#include "storage/dsg_native_birth_wire.h"
#include "storage/ipc.h"
#include "storage/proc.h"

BufferDescPadded *BufferDescriptors;
char	   *BufferBlocks;
ConditionVariableMinimallyPadded *BufferIOCVArray;
WritebackContext BackendWritebackContext;
CkptSortItem *CkptBufferIds;

/*
 * Buffer with target WAL redo page.
 * We must not evict this page from the buffer pool, but we cannot just keep it pinned because
 * some WAL redo functions expect the page to not be pinned. So we have a special check in
 * localbuf.c to prevent this buffer from being evicted.
 */
Buffer		wal_redo_buffer;
bool		am_wal_redo_postgres = false;

/*-
 * Postmaster进程私有的VA所有权表；fork继承副本，不在MAP_SHARED目录里。
 * 四数组几何在整个Postmaster寿命内固定，内部恢复只换backing。Parked占位
 * 不可访问且不含旧共享数据，不能因DSBCtrl被销毁就把它当成未预留。非共享
 * 子进程必须独立解除Shared和Parked两种映射，不能依赖共享目录仍存在。
 */
typedef enum DsgArrayMappingState
{
	DSG_ARRAY_MAPPING_EMPTY,
	DSG_ARRAY_MAPPING_SHARED,
	DSG_ARRAY_MAPPING_PARKED,
	DSG_ARRAY_MAPPING_TRANSITION
} DsgArrayMappingState;

static struct
{
	DsgRegion	region;
	DsgArrayMappingState state;
} DynamicBufferMappings[DSG_BUFFER_ARRAY_COUNT];

static void
ClearDynamicBufferAliases(void)
{
	DSBCtrl = NULL;
	BufferDescriptors = NULL;
	BufferBlocks = NULL;
	BufferIOCVArray = NULL;
	CkptBufferIds = NULL;
}

/*-
 * Postmaster 在共享内存子进程全部退出后调用 shmem_exit 重建共享内存。
 * logger等不使用共享内存的子进程出生时独立脱离四数组，不能让其继续持有
 * 旧backing。此回调在主段解除映射前执行，避免多次恢复累积旧映射/boot。
 * fork 子进程的退出回调列表会由 PG 重置，不会销毁其他进程的映射。
 */
static void
DestroyDynamicBufferRegions(int code, Datum arg)
{
	int			i;

	DsgArrayRootRetire();
	for (i = 0; i < DSG_BUFFER_ARRAY_COUNT; i++)
	{
		if (DynamicBufferMappings[i].state == DSG_ARRAY_MAPPING_TRANSITION)
			elog(FATAL, "DSGA array mapping ownership is unconfirmed");
		if (DynamicBufferMappings[i].state != DSG_ARRAY_MAPPING_SHARED)
			continue;
		/* 保持VA占位，但解除此进程对旧共享backing的引用；不触共享目录。 */
		DynamicBufferMappings[i].state = DSG_ARRAY_MAPPING_TRANSITION;
		DsgRegionPark(&DynamicBufferMappings[i].region);
		DynamicBufferMappings[i].state = DSG_ARRAY_MAPPING_PARKED;
	}
	ClearDynamicBufferAliases();
}

/*-
 * 仅解除当前进程的继承映射；调用者必须已停止本进程对数组的全部访问。
 * postmaster_child_launch在非共享内存子进程的Main之前调用，此时没有PGPROC，
 * 不取backend锁。四数组VA/span在本boot不可变，可直接复制几何信息。
 *
 * 只销毁进程私有所有权表：共享目录属于MAP_SHARED，直接Destroy目录里的
 * region会MemSet父进程和业务backend仍使用的地址/长度。只清本进程的表与
 * 全局别名，不触碰共享目录、提交成本/未知标记或父进程退出回调。munmap失败
 * 继续走原ERROR/退出路径，不能进入声称不持共享内存的Main继续运行。
 */
void
DsgBufferDetachInheritedArrays(void)
{
	int			i;

	for (i = 0; i < DSG_BUFFER_ARRAY_COUNT; i++)
	{
		/* 此表是进程私有，Destroy清零不会MemSet父/业务进程的共享目录。
		 * DSBCtrl已为空时也要清理恢复期间继承的PROT_NONE占位。 */
		if (DynamicBufferMappings[i].state == DSG_ARRAY_MAPPING_TRANSITION)
			elog(FATAL, "cannot detach unconfirmed DSGA array mapping");
		if (DsgBirthArraysEnabled())
		{
			/* 统一R测试profile不能把logger的VA腾给私有分配，也不能只
			 * mprotect后继续持有旧backing；Park同步替换为全新无访问占位。 */
			if (DynamicBufferMappings[i].state == DSG_ARRAY_MAPPING_SHARED)
			{
				DynamicBufferMappings[i].state = DSG_ARRAY_MAPPING_TRANSITION;
				DsgRegionPark(&DynamicBufferMappings[i].region);
				DynamicBufferMappings[i].state = DSG_ARRAY_MAPPING_PARKED;
			}
			else if (DynamicBufferMappings[i].state != DSG_ARRAY_MAPPING_PARKED)
				elog(FATAL, "DSGA early array child lost its original reservation");
		}
		else
		{
			DsgRegionDestroy(&DynamicBufferMappings[i].region);
			DynamicBufferMappings[i].state = DSG_ARRAY_MAPPING_EMPTY;
		}
	}
	ClearDynamicBufferAliases();
}

static void *
ReserveDynamicBufferArray(DsgBufferArrayId id, Size element_bytes, bool map_only)
{
	DsgBufferArray *array = &DSBCtrl->arrays[id];
	Size		initial_bytes = mul_size(NBuffers, element_bytes);
	DsgMemoryResult result;
	Size		maximum_bytes = mul_size(DSBCtrl->maximum, element_bytes);

	array->element_bytes = element_bytes;
	if (DynamicBufferMappings[id].state == DSG_ARRAY_MAPPING_EMPTY)
	{
		DsgRegionReserve(&DynamicBufferMappings[id].region, maximum_bytes);
		DynamicBufferMappings[id].state = DSG_ARRAY_MAPPING_SHARED;
	}
	else
	{
		if (DynamicBufferMappings[id].state != DSG_ARRAY_MAPPING_PARKED ||
			DynamicBufferMappings[id].region.maximum_bytes != maximum_bytes)
			elog(FATAL, "DSGA array mapping lifetime or geometry changed");
		/* 新boot只能复用自己始终持有的占位，不attach旧共享backing。 */
		DynamicBufferMappings[id].state = DSG_ARRAY_MAPPING_TRANSITION;
		DsgRegionRearm(&DynamicBufferMappings[id].region);
		DynamicBufferMappings[id].state = DSG_ARRAY_MAPPING_SHARED;
	}
	array->region = DynamicBufferMappings[id].region;
	if (map_only)
		return array->region.address;
	result = DsgBufferPopulateArray(id, &array->region, 0, initial_bytes, NULL, 30000);
	if (result.status != DSG_MEMORY_OK)
	{
		errno = result.system_errno;
		ereport(FATAL,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("could not populate initial DSGA buffer array: %m")));
	}

	/*
	 * 仅记录触页区间的页对齐保守预算；不把预留的全部 M 当作已提交。
	 */
	array->committed_upper_bytes = TYPEALIGN(array->region.page_bytes, initial_bytes);
	return array->region.address;
}

/*
 * Data Structures:
 *		buffers live in a freelist and a lookup data structure.
 *
 *
 * Buffer Lookup:
 *		Two important notes.  First, the buffer has to be
 *		available for lookup BEFORE an IO begins.  Otherwise
 *		a second process trying to read the buffer will
 *		allocate its own copy and the buffer pool will
 *		become inconsistent.
 *
 * Buffer Replacement:
 *		see freelist.c.  A buffer cannot be replaced while in
 *		use either by data manager or during IO.
 *
 *
 * Synchronization/Locking:
 *
 * IO_IN_PROGRESS -- this is a flag in the buffer descriptor.
 *		It must be set when an IO is initiated and cleared at
 *		the end of the IO.  It is there to make sure that one
 *		process doesn't start to use a buffer while another is
 *		faulting it in.  see WaitIO and related routines.
 *
 * refcount --	Counts the number of processes holding pins on a buffer.
 *		A buffer is pinned during IO and immediately after a BufferAlloc().
 *		Pins must be released before end of transaction.  For efficiency the
 *		shared refcount isn't increased if an individual backend pins a buffer
 *		multiple times. Check the PrivateRefCount infrastructure in bufmgr.c.
 */


/*
 * Initialize shared buffer pool
 *
 * This is called once during shared-memory initialization (either in the
 * postmaster, or in a standalone backend).
 */
void
InitBufferPool(void)
{
	bool		foundBufs = false,
				foundDescs = false,
				foundIOCV = false,
				foundBufCkpt = false;

	/* 即使static buffer配置，也拒绝显式但不受支持/不完整的Root模式；
	 * 不能因未进入dynamic分支而静默忽略启用。缺省仍零网络副作用。 */
	(void) DsgArrayRootEnabled();
	if (DsgBirthArraysEnabled())
	{
		DsgRegion regions[DSG_BUFFER_ARRAY_COUNT];
		int i;

		if (!enable_dynamic_shared_buffers)
			elog(FATAL, "DSGA early arrays require dynamic shared buffers");
		DsgBirthArraysCheckMaximum(GetMaxNBuffers());
		/* 初次与内部恢复都先核原M/N0，再导入或触页；不从GUC扩大原出生。 */
		if (!DsgNativeBirthWireCheckConfiguration(GetMaxNBuffers(), NBuffers))
			elog(FATAL, "DSGA root birth M/N0 or final audit gate differs");
		if (DynamicBufferMappings[0].state == DSG_ARRAY_MAPPING_EMPTY)
		{
			DsgBirthArraysImport(regions, GetMaxNBuffers());
			for (i = 0; i < DSG_BUFFER_ARRAY_COUNT; ++i)
			{
				if (DynamicBufferMappings[i].state != DSG_ARRAY_MAPPING_EMPTY)
					elog(FATAL, "DSGA early arrays have mixed mapping ownership");
				DynamicBufferMappings[i].region = regions[i];
				DynamicBufferMappings[i].state = DSG_ARRAY_MAPPING_PARKED;
			}
		}
	}
	if (enable_dynamic_shared_buffers)
	{
		DynamicSharedBuffersInit();

		/*
		 * 先注册清理，后面的任何预留/提交失败均不会漏掉已建立的映射。
		 */
		on_shmem_exit(DestroyDynamicBufferRegions, (Datum) 0);
		BufferDescriptors = ReserveDynamicBufferArray(DSG_BUFFER_DESCRIPTORS,
													  sizeof(BufferDescPadded), DsgArrayRootEnabled());
		BufferBlocks = ReserveDynamicBufferArray(DSG_BUFFER_BLOCKS, BLCKSZ, DsgArrayRootEnabled());
		BufferIOCVArray = ReserveDynamicBufferArray(DSG_BUFFER_IOCV,
													sizeof(ConditionVariableMinimallyPadded), DsgArrayRootEnabled());
		CkptBufferIds = ReserveDynamicBufferArray(DSG_BUFFER_CHECKPOINT,
												  sizeof(CkptSortItem), DsgArrayRootEnabled());
		if (DsgArrayRootEnabled())
		{
			int i;
			/* 四backing全部存在后才允许Root注册/预触；此前没有数组对象触页。 */
			DsgArrayRootRegisterAll();
			for (i = 0; i < DSG_BUFFER_ARRAY_COUNT; ++i)
			{
				DsgBufferArray *a = &DSBCtrl->arrays[i];
				Size initial = mul_size(NBuffers, a->element_bytes);
				DsgMemoryResult result;
				a->committed_upper_bytes = TYPEALIGN(a->region.page_bytes, initial);
				a->allocation_unknown = true;
				result = DsgBufferPopulateArray((DsgBufferArrayId) i, &a->region, 0, initial, NULL, 30000);
				if (result.status != DSG_MEMORY_OK)
				{
					errno = result.system_errno;
					ereport(FATAL, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("DSGA initial Root array populate unknown: %m")));
				}
				a->allocation_unknown = false;
			}
		}
		if ((uintptr_t) BufferBlocks % PG_IO_ALIGN_SIZE != 0)
			ereport(FATAL,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("DSGA provider cannot satisfy buffer I/O alignment")));
	}
	else
	{

		/* Align descriptors to a cacheline boundary. */
		BufferDescriptors = (BufferDescPadded *)
			ShmemInitStruct("Buffer Descriptors",
							NBuffers * sizeof(BufferDescPadded),
							&foundDescs);

		/* Align buffer pool on IO page size boundary. */
		BufferBlocks = (char *)
			TYPEALIGN(PG_IO_ALIGN_SIZE,
					  ShmemInitStruct("Buffer Blocks",
									  NBuffers * (Size) BLCKSZ + PG_IO_ALIGN_SIZE,
									  &foundBufs));

		/* Align condition variables to cacheline boundary. */
		BufferIOCVArray = (ConditionVariableMinimallyPadded *)
			ShmemInitStruct("Buffer IO Condition Variables",
							NBuffers * sizeof(ConditionVariableMinimallyPadded),
							&foundIOCV);

		/*
		 * The array used to sort to-be-checkpointed buffer ids is located in
		 * shared memory, to avoid having to allocate significant amounts of
		 * memory at runtime. As that'd be in the middle of a checkpoint, or
		 * when the checkpointer is restarted, memory allocation failures
		 * would be painful.
		 */
		CkptBufferIds = (CkptSortItem *)
			ShmemInitStruct("Checkpoint BufferIds",
							NBuffers * sizeof(CkptSortItem), &foundBufCkpt);
	}

	if (foundDescs || foundBufs || foundIOCV || foundBufCkpt)
	{
		/* should find all of these, or none of them */
		Assert(foundDescs && foundBufs && foundIOCV && foundBufCkpt);
		/* note: this path is only taken in EXEC_BACKEND case */
	}
	else
	{
		int			i;

		/*
		 * Initialize all the buffer headers.
		 */
		for (i = 0; i < NBuffers; i++)
		{
			BufferDesc *buf = GetBufferDescriptor(i);

			ClearBufferTag(&buf->tag);

			pg_atomic_init_u32(&buf->state, 0);
			buf->wait_backend_pgprocno = INVALID_PROC_NUMBER;

			buf->buf_id = i;

			/*
			 * Initially link all the buffers together as unused. Subsequent
			 * management of this list is done by freelist.c.
			 */
			buf->freeNext = i + 1;

			LWLockInitialize(BufferDescriptorGetContentLock(buf),
							 LWTRANCHE_BUFFER_CONTENT);

			ConditionVariableInit(BufferDescriptorGetIOCV(buf));
		}

		/* Correct last entry of linked list */
		GetBufferDescriptor(NBuffers - 1)->freeNext = FREENEXT_END_OF_LIST;
	}

	/* Init other shared buffer-management stuff */
	StrategyInitialize(!foundDescs);

	/* Initialize per-backend file flush context */
	WritebackContextInit(&BackendWritebackContext,
						 &backend_flush_after);
}

/*
 * BufferShmemSize
 *
 * compute the size of shared memory for the buffer pool including
 * data pages, buffer descriptors, hash tables, etc.
 */
Size
BufferShmemSize(void)
{
	Size		size = 0;

	/*-
	 * 动态数组在主共享内存之外各自 reserve，不能在主段再分配一份 M。
	 * Hash 按 M 固定预算，目录及锁不随尾部回收。shared_memory_size 仍
	 * 表示主共享段；DSGA 的独立占用须通过数组目录单独核算。
	 */
	(void) GetMaxNBuffers();
	if (enable_dynamic_shared_buffers)
		return add_size(DynamicSharedBuffersShmemSize(), StrategyShmemSize());

	/* size of buffer descriptors */
	size = add_size(size, mul_size(NBuffers, sizeof(BufferDescPadded)));
	/* to allow aligning buffer descriptors */
	size = add_size(size, PG_CACHE_LINE_SIZE);

	/* size of data pages, plus alignment padding */
	size = add_size(size, PG_IO_ALIGN_SIZE);
	size = add_size(size, mul_size(NBuffers, BLCKSZ));

	/* size of stuff controlled by freelist.c */
	size = add_size(size, StrategyShmemSize());

	/* size of I/O condition variables */
	size = add_size(size, mul_size(NBuffers,
								   sizeof(ConditionVariableMinimallyPadded)));
	/* to allow aligning the above */
	size = add_size(size, PG_CACHE_LINE_SIZE);

	/* size of checkpoint sort array in bufmgr.c */
	size = add_size(size, mul_size(NBuffers, sizeof(CkptSortItem)));

	return size;
}
