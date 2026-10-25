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
 * Postmaster 在共享内存子进程全部退出后调用 shmem_exit 重建共享内存。
 * logger等不使用共享内存的子进程出生时独立脱离四数组，不能让其继续持有
 * 旧backing。此回调在主段解除映射前执行，避免多次恢复累积旧映射/boot。
 * fork 子进程的退出回调列表会由 PG 重置，不会销毁其他进程的映射。
 */
static void
DestroyDynamicBufferRegions(int code, Datum arg)
{
	DsgBufferDetachInheritedArrays();
}

/*-
 * 仅解除当前进程的继承映射；调用者必须已停止本进程对数组的全部访问。
 * postmaster_child_launch在非共享内存子进程的Main之前调用，此时没有PGPROC，
 * 不取backend锁。四数组VA/span在本boot不可变，可直接复制几何信息。
 *
 * 必须销毁局部DsgRegion副本：目录本身是MAP_SHARED，直接Destroy目录里的
 * region会MemSet父进程和所有业务backend仍使用的地址/长度。只清本进程的
 * 全局别名，不触碰共享目录、提交成本/未知标记或父进程退出回调。munmap失败
 * 继续走原ERROR/退出路径，不能进入声称不持共享内存的Main继续运行。
 */
void
DsgBufferDetachInheritedArrays(void)
{
	int i;

	if (DSBCtrl == NULL)
		return;
	for (i = 0; i < DSG_BUFFER_ARRAY_COUNT; i++)
	{
		DsgRegion local = DSBCtrl->arrays[i].region;

		DsgRegionDestroy(&local);
	}
	DSBCtrl = NULL;
	BufferDescriptors = NULL;
	BufferBlocks = NULL;
	BufferIOCVArray = NULL;
	CkptBufferIds = NULL;
}

static void *
ReserveDynamicBufferArray(DsgBufferArrayId id, Size element_bytes)
{
	DsgBufferArray *array = &DSBCtrl->arrays[id];
	Size		initial_bytes = mul_size(NBuffers, element_bytes);
	DsgMemoryResult result;

	array->element_bytes = element_bytes;
	DsgRegionReserve(&array->region, mul_size(DSBCtrl->maximum, element_bytes));
	result = DsgRegionPopulate(&array->region, 0, initial_bytes);
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

	if (enable_dynamic_shared_buffers)
	{
		DynamicSharedBuffersInit();

		/*
		 * 先注册清理，后面的任何预留/提交失败均不会漏掉已建立的映射。
		 */
		on_shmem_exit(DestroyDynamicBufferRegions, (Datum) 0);
		BufferDescriptors = ReserveDynamicBufferArray(DSG_BUFFER_DESCRIPTORS,
													  sizeof(BufferDescPadded));
		BufferBlocks = ReserveDynamicBufferArray(DSG_BUFFER_BLOCKS, BLCKSZ);
		BufferIOCVArray = ReserveDynamicBufferArray(DSG_BUFFER_IOCV,
													sizeof(ConditionVariableMinimallyPadded));
		CkptBufferIds = ReserveDynamicBufferArray(DSG_BUFFER_CHECKPOINT,
												  sizeof(CkptSortItem));
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
