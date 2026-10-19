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
#include "storage/spin.h"

/* Neon 扩展按能力宏适配，不能由 PG 大版本号推断本 fork 的内核接口。 */
#define NEON_DSG_BUFFER_API_VERSION 1
#define DSG_BUFFER_GRANULE_BUFFERS 2048

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

/*-
 * Granule 是固定 Buffer ID 范围，元数据永不随数组尾部回收。
 * 初值或 M 可以形成部分 Granule；active_buffers 才是已发布数量。
 * RESERVED 仅表示逻辑未发布，不声称 backing 已经释放。
 * PREPARING/READY 可以保留原有 active_buffers，新增部分不可见。
 */
typedef enum DsgBufferGranuleState
{
	DSG_GRANULE_RESERVED,
	DSG_GRANULE_PREPARING,
	DSG_GRANULE_READY,
	DSG_GRANULE_ACTIVE
} DsgBufferGranuleState;

typedef struct DsgBufferGranule
{
	int			first_buffer;
	int			maximum_buffers;
	int			active_buffers;
	int			prepared_buffers;
	uint64		generation;
	DsgBufferGranuleState state;
} DsgBufferGranule;

typedef enum DsgBufferOperationPhase
{
	DSG_OP_IDLE,
	DSG_OP_PREPARING,
	DSG_OP_COMMITTED,
	DSG_OP_DENIED
} DsgBufferOperationPhase;

typedef enum DsgBufferOperationReason
{
	DSG_REASON_NONE,
	DSG_REASON_TIMEOUT,
	DSG_REASON_POPULATE_FAILED,
	DSG_REASON_INTERRUPTED
} DsgBufferOperationReason;

/*-
 * boot 防止跨共享内存重建重放，revision 拒绝过期请求，operation_id
 * 用于同一请求重试，expected_generation 防止据过期容量执行新请求。
 * 外层 VM/Compute 身份仍须由 Neon/Agent 栅栏核验，本结构不能替代它。
 */
typedef struct DsgBufferGrowRequest
{
	uint64		boot_id;
	uint64		revision;
	uint64		operation_id_high;
	uint64		operation_id_low;
	uint64		expected_generation;
	int			target_buffers;
} DsgBufferGrowRequest;

typedef struct DsgBufferOperation
{
	DsgBufferGrowRequest request;
	int			original_buffers;
	DsgBufferOperationPhase phase;
	DsgBufferOperationReason reason;
} DsgBufferOperation;

typedef struct DsgBufferSnapshot
{
	DsgBufferOperation operation;
	uint64		boot_id;
	uint64		generation;
	int			low;
	int			high;
	int			maximum;
	Size		committed_upper_bytes[DSG_BUFFER_ARRAY_COUNT];
	bool		allocation_unknown[DSG_BUFFER_ARRAY_COUNT];
	bool		replayed;
} DsgBufferSnapshot;

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
	/*-
	 * resize_lock 只串行协调者。status_lock 仅保护短的状态复制，
	 * 禁止在 spinlock 内分配、调用 Provider、等待或报告 ERROR。
	 * 锁序：resize -> access -> status；普通观察者只取 status。
	 */
	LWLock		resize_lock;
	slock_t		status_lock;
	pg_atomic_uint64 generation;
	DsgBufferOperation operation;
	DsgBufferArray arrays[DSG_BUFFER_ARRAY_COUNT];

	/*
	 * 目录遍历需持 resize_lock；禁止在 status spinlock 内做长遍历。
	 */
	int			granule_count;
	DsgBufferGranule granules[FLEXIBLE_ARRAY_MEMBER];
} DynamicSharedBuffersControl;

/*-
 * Guard 持有期间 H 及其数组 backing 不会被缩容回收。嵌套调用复用本进程
 * 已持有的锁；每个 Guard 只释放自己取得的锁，避免重复共享加锁遇到排队
 * writer 时自锁。Guard 不阻止查询取消；ERROR/FATAL 路径由 PG 的
 * LWLockReleaseAll 清理。Begin/End 对原生 holdoff 的调整必须严格成对。
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
extern void DsgBufferGetSnapshot(DsgBufferSnapshot *snapshot);

/* 内核协调接口；调用者须先完成权限/外层资源预算检查，尚无用户 SQL 入口。 */
extern bool DsgBufferGrow(const DsgBufferGrowRequest *request, int timeout_ms,
						  DsgBufferSnapshot *snapshot);

#endif							/* DYNAMIC_SHARED_BUFFERS_H */
