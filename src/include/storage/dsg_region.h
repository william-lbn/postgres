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
} DsgMemoryResult;

/* Region 必须先清零；maximum_bytes 是可访问上限，映射按系统页向上对齐。 */
extern void DsgRegionReserve(DsgRegion *region, Size maximum_bytes);
extern void DsgRegionDestroy(DsgRegion *region);
extern DsgMemoryResult DsgRegionPopulate(DsgRegion *region, Size offset, Size bytes);
extern DsgMemoryResult DsgRegionRelease(DsgRegion *region, Size offset, Size bytes);
extern bool DsgRegionCanRelease(void);

#endif							/* DSG_REGION_H */
