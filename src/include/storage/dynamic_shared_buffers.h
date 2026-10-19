/*-------------------------------------------------------------------------
 * dynamic_shared_buffers.h -- Buffer Pool 的固定地址上限及共享容量状态。
 *
 * L（low）只约束新 victim；H（high）是已初始化且允许访问的上限；M（maximum）
 * 在本轮共享内存生命周期固定。已有 pin 的有效性不能使用 L 判断。
 *-------------------------------------------------------------------------
 */
#ifndef DYNAMIC_SHARED_BUFFERS_H
#define DYNAMIC_SHARED_BUFFERS_H

#include "port/atomics.h"
#include "storage/dsg_region.h"
#include "storage/lwlock.h"

typedef enum DsgBufferArrayId
{
	DSG_BUFFER_DESCRIPTORS,
	DSG_BUFFER_BLOCKS,
	DSG_BUFFER_IOCV,
	DSG_BUFFER_CHECKPOINT,
	DSG_BUFFER_ARRAY_COUNT
} DsgBufferArrayId;

typedef struct DsgBufferArray
{
	DsgRegion	region;
	Size		element_bytes;

	/*
	 * 预算保守上界，不是 RSS 采样值；未发布部分也必须计入。
	 */
	Size		committed_upper_bytes;
	bool		allocation_unknown;
} DsgBufferArray;

typedef struct DynamicSharedBuffersControl
{
	pg_atomic_uint32 low;
	pg_atomic_uint32 high;
	int			maximum;

	/*
	 * 每次重建共享内存都换身份，包括同一 Postmaster 的
	 * crash recovery。
	 */
	uint64		boot_id;
	LWLock		access_lock;
	DsgBufferArray arrays[DSG_BUFFER_ARRAY_COUNT];
} DynamicSharedBuffersControl;

/*-
 * Guard 持有期间 H 及其数组 backing 不会被缩容回收。嵌套调用复用本进程
 * 已持有的锁；每个 Guard 只释放自己取得的锁，避免重复共享加锁遇到排队
 * writer 时自锁。ERROR 路径由 PG 的 LWLockReleaseAll 负责清理。
 */
typedef struct DsgBufferAccess
{
	int			count;
	bool		acquired;
} DsgBufferAccess;

extern PGDLLIMPORT bool enable_dynamic_shared_buffers;
extern PGDLLIMPORT int max_shared_buffers;
extern PGDLLIMPORT DynamicSharedBuffersControl *DSBCtrl;

extern int	GetMaxNBuffers(void);
extern int	GetLowNBuffers(void);
extern int	GetHighNBuffers(void);
extern Size DynamicSharedBuffersShmemSize(void);
extern void DynamicSharedBuffersInit(void);
extern DsgBufferAccess DsgBufferAccessBegin(void);
extern void DsgBufferAccessEnd(DsgBufferAccess *access);

#endif							/* DYNAMIC_SHARED_BUFFERS_H */
