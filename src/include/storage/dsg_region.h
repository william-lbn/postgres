/*-------------------------------------------------------------------------
 * dsg_region.h -- DSGA 稳定共享地址区的普通页 Provider。
 *
 * 此层只管理地址和物理提交操作，不判断 Buffer pin、dirty、IO 或进程屏障。
 * 调用方必须在发布前提交，在全部访问者排水后回收；Provider 的成功回执
 * 不能代替组件自己的并发安全证明，也不能直接授权 VM 缩容。
 *-------------------------------------------------------------------------
 */
#ifndef DSG_REGION_H
#define DSG_REGION_H

typedef struct DsgRegion
{
	char	   *address;
	Size		maximum_bytes;
	Size		mapped_bytes;
	Size		page_bytes;
} DsgRegion;

typedef enum DsgMemoryStatus
{
	DSG_MEMORY_OK,
	DSG_MEMORY_NOOP,
	DSG_MEMORY_UNSUPPORTED,
	DSG_MEMORY_FAILED
} DsgMemoryStatus;

typedef struct DsgMemoryResult
{
	DsgMemoryStatus status;
	int			system_errno;
	Size		requested_offset;
	Size		requested_bytes;
	Size		affected_offset;
	Size		affected_bytes;
	bool		physical_release_verified;
	bool		allocation_may_be_partial;
	bool		release_may_be_partial;
} DsgMemoryResult;

/* Region 必须先清零；maximum_bytes 是可访问上限，映射按系统页向上对齐。 */
extern void DsgRegionReserve(DsgRegion *region, Size maximum_bytes);
extern void DsgRegionDestroy(DsgRegion *region);
/*-
 * 仅用于调用者独占的完整Region寿命转换。Park以PROT_NONE私有占位替换
 * 旧共享backing但不释放VA；Rearm只把该占位换成全新零填充共享backing。
 * 不可传入任意地址/子区间，不能有本进程访问者或旧共享子进程。几何保持
 * 不变；固定映射出现不确定失败则FATAL，禁止继续使用/复用未知地址所有权。
 * 这是进程映射生命周期，不是跨进程物理回收或VM预算许可。
 */
extern void DsgRegionPark(DsgRegion *region);
extern void DsgRegionRearm(DsgRegion *region);
extern DsgMemoryResult DsgRegionPopulate(DsgRegion *region, Size offset, Size bytes);
extern DsgMemoryResult DsgRegionRelease(DsgRegion *region, Size offset, Size bytes);

/*-
 * 调用者必须已排水 [offset, maximum_bytes) 的全部对象。与普通 Release
 * 不同，此接口还释放 Region 独占的末页 padding；保留起始共享边界页。
 * requested 是逻辑尾部，affected 可包含不可放置对象的末页填充空间。
 */
extern DsgMemoryResult DsgRegionReleaseTail(DsgRegion *region, Size offset);
extern bool DsgRegionCanRelease(void);

#endif							/* DSG_REGION_H */
