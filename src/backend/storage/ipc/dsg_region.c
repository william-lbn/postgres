/*-------------------------------------------------------------------------
 * dsg_region.c -- DSGA 普通页稳定地址区。
 *
 * 共享 mmap 在 Postmaster fork 前建立后，其地址和 backing 身份由子进程
 * 继承。后续操作不移动地址，也不在各 Backend 中单独 remap。
 * 本文件暂不提供 EXEC_BACKEND、显式 HugeTLB 或跨重启 attach；这些能力
 * 必须由各自 Provider 明确实现，不能借普通页的回执推断。
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <errno.h>
#ifndef WIN32
#include <unistd.h>
#include <sys/mman.h>
#endif

#include "miscadmin.h"
#include "storage/dsg_region.h"
#include "utils/injection_point.h"

/*-
 * 用减法检查范围，避免 offset + bytes 溢出后通过边界判断。
 * 零长度允许恰好位于末尾，但不能使用尚未建立或已经销毁的 Region。
 */
static void
DsgRegionCheckRange(const DsgRegion *region, Size offset, Size bytes)
{
	if (region == NULL || region->address == NULL ||
		region->page_bytes == 0 || offset > region->maximum_bytes ||
		bytes > region->maximum_bytes - offset)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("DSGA memory range is outside the reserved region")));
}

static DsgMemoryResult
DsgRegionResult(Size offset, Size bytes)
{
	DsgMemoryResult result;

	MemSet(&result, 0, sizeof(result));
	result.status = bytes == 0 ? DSG_MEMORY_NOOP : DSG_MEMORY_OK;
	result.requested_offset = offset;
	result.requested_bytes = bytes;
	return result;
}

void
DsgRegionReserve(DsgRegion *region, Size maximum_bytes)
{
#ifndef WIN32
	long		page_bytes = sysconf(_SC_PAGESIZE);
	Size		mapped_bytes;
	void	   *address;

	if (region == NULL || region->address != NULL || maximum_bytes == 0 ||
		page_bytes <= 0 || maximum_bytes > SIZE_MAX - ((Size) page_bytes - 1))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid DSGA region reservation")));

	mapped_bytes = TYPEALIGN((Size) page_bytes, maximum_bytes);
	address = mmap(NULL, mapped_bytes, PROT_READ | PROT_WRITE,
				   MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (address == MAP_FAILED)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("could not reserve DSGA shared region: %m")));

	region->address = address;
	region->maximum_bytes = maximum_bytes;
	region->mapped_bytes = mapped_bytes;
	region->page_bytes = (Size) page_bytes;
#else
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("DSGA shared regions require mmap support")));
#endif
}

/*-
 * 此操作只解除本进程的整个映射。调用方负责先停止全部使用者，并对每个
 * 参与进程执行生命周期清理；不能把本函数当成跨进程物理释放证明。
 */
void
DsgRegionDestroy(DsgRegion *region)
{
	if (region == NULL || region->address == NULL)
		return;
#ifndef WIN32
	if (munmap(region->address, region->mapped_bytes) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not unmap DSGA shared region: %m")));
#endif
	MemSet(region, 0, sizeof(*region));
}

/*-
 * MAP_FIXED只允许覆盖本进程仍独占的完整Region；调用方的私有寿命表证明
 * 该范围没有被交还通用地址分配器。不能先munmap再mmap：窗口内可能有新的
 * 分配，也会暂时释放未来私有额度证明所依赖的固定VA。一次替换丢弃旧引用，
 * 不改Region几何，更不写其他进程的映射。旧共享进程必须先退出/独立脱离。
 *
 * 失败后的映射状态不能一概假设未变，故FATAL而不是返回可重试ERROR；调用者
 * 也必须先标记转换中，只有系统调用完整成功才发布Parked/Shared状态。此API
 * 不声称物理释放，不修改控制目录的已提交成本或旧操作结果。
 */
static void
DsgRegionReplaceOwnedMapping(DsgRegion *region, bool shared)
{
#ifndef WIN32
	void *address;

	if (region == NULL || region->address == NULL || region->page_bytes == 0 ||
		region->maximum_bytes == 0 || region->mapped_bytes == 0 ||
		region->mapped_bytes % region->page_bytes != 0 ||
		region->maximum_bytes > region->mapped_bytes ||
		(uintptr_t) region->address % region->page_bytes != 0)
		ereport(FATAL,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid owned DSGA mapping geometry")));
	address = mmap(region->address, region->mapped_bytes,
				   shared ? PROT_READ | PROT_WRITE : PROT_NONE,
				   MAP_FIXED | MAP_ANONYMOUS | (shared ? MAP_SHARED : MAP_PRIVATE), -1, 0);
	if (address == MAP_FAILED)
		ereport(FATAL,
				(errcode_for_file_access(),
				 errmsg("could not replace owned DSGA mapping: %m")));
	if (address != region->address)
		elog(FATAL, "owned DSGA mapping replacement returned a different address");
#else
		ereport(FATAL,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("DSGA owned mapping replacement requires mmap support")));
#endif
}

void
DsgRegionPark(DsgRegion *region)
{
	DsgRegionReplaceOwnedMapping(region, false);
}

void
DsgRegionRearm(DsgRegion *region)
{
	DsgRegionReplaceOwnedMapping(region, true);
}

DsgMemoryResult
DsgRegionPopulate(DsgRegion *region, Size offset, Size bytes)
{
	DsgMemoryResult result = DsgRegionResult(offset, bytes);

	DsgRegionCheckRange(region, offset, bytes);
	if (bytes == 0)
		return result;

	/*
	 * 启动 Buffer Pool 时注入点共享目录尚未初始化，只在
	 * Backend 运行钩子。
	 */
	if (IsUnderPostmaster)
		INJECTION_POINT("dsg-region-before-populate");

#if defined(__linux__) && defined(MADV_POPULATE_WRITE)
	{
		Size		start = TYPEALIGN_DOWN(region->page_bytes, offset);
		Size		end = TYPEALIGN(region->page_bytes, offset + bytes);

		/*-
		 * POPULATE 不清零已经存在的边界页，因此不会覆盖仍活跃的前缀对象。
		 * 系统调用失败可能已提交部分页；失败回执明确标记未知占用，上层
		 * 必须清理或保留预算，不能仅因逻辑容量未发布就把实际成本记为零。
		 */
		if (madvise(region->address + start, end - start, MADV_POPULATE_WRITE) != 0)
		{
			result.status = DSG_MEMORY_FAILED;
			result.system_errno = errno;
			result.allocation_may_be_partial = true;
			return result;
		}
		result.affected_offset = start;
		result.affected_bytes = end - start;
	}
#else
	/*-
	 * Mac 的本地业务验证使用真实共享映射并触页，但只触及调用方声明的
	 * 未发布区间，不能把向外对齐后的既有对象清零。此路径不声明 Linux
	 * 可返回分配错误的预提交能力，更不产生任何物理回收证明。
	 */
	memset(region->address + offset, 0, bytes);
	result.affected_offset = offset;
	result.affected_bytes = bytes;
#endif
	return result;
}

bool
DsgRegionCanRelease(void)
{
	/*
	 * 编译期能力提示；只有一次实际成功的 Release 回执才是回收证明。
	 */
#if defined(__linux__) && defined(MADV_REMOVE)
	return true;
#else
	return false;
#endif
}

/* 调用者已检查 Region 和范围；此处只执行对齐后的真实回收并记录回执。 */
static DsgMemoryResult
DsgRegionReleasePages(DsgRegion *region, DsgMemoryResult result, Size start, Size end)
{
	Assert(start % region->page_bytes == 0 && end % region->page_bytes == 0);
	Assert(start <= region->mapped_bytes && end <= region->mapped_bytes);
	if (start >= end)
	{
		result.status = DSG_MEMORY_NOOP;
		return result;
	}
	if (IsUnderPostmaster)
		INJECTION_POINT("dsg-region-before-release");

#if defined(__linux__) && defined(MADV_REMOVE)
	if (madvise(region->address + start, end - start, MADV_REMOVE) != 0)
	{
		result.status = DSG_MEMORY_FAILED;
		result.system_errno = errno;

		/*
		 * 系统调用失败不能证明整段未变；上层必须保留原预算并对账。
		 */
		result.release_may_be_partial = true;
		return result;
	}
	result.affected_offset = start;
	result.affected_bytes = end - start;
	result.physical_release_verified = true;
#else

	/*
	 * 不支持时保留实际页面；VM 层必须据此拒绝物理缩容。
	 */
	result.status = DSG_MEMORY_UNSUPPORTED;
	result.system_errno = ENOSYS;
#endif
	return result;
}

DsgMemoryResult
DsgRegionRelease(DsgRegion *region, Size offset, Size bytes)
{
	DsgMemoryResult result = DsgRegionResult(offset, bytes);
	Size		start;
	Size		end;

	DsgRegionCheckRange(region, offset, bytes);
	if (bytes == 0)
		return result;
	/*-
	 * 普通区间继续严格向内对齐：两端半页可能包含其他活跃对象，不能
	 * 因复用尾部回收代码而扩大范围。回执记录实际完整页，不推算 RSS。
	 */
	start = TYPEALIGN(region->page_bytes, offset);
	end = TYPEALIGN_DOWN(region->page_bytes, offset + bytes);
	return DsgRegionReleasePages(region, result, start, end);
}

DsgMemoryResult
DsgRegionReleaseTail(DsgRegion *region, Size offset)
{
	DsgMemoryResult result;
	Size		start;

	/*
	 * 先检查再做减法，offset 越界时不能绕回成巨大合法请求。
	 */
	DsgRegionCheckRange(region, offset, 0);
	result = DsgRegionResult(offset, region->maximum_bytes - offset);
	start = TYPEALIGN(region->page_bytes, offset);
	/*-
	 * Region 单独拥有整个 mmap；maximum_bytes 后的 padding 不能存放
	 * 任何对象。因此，完整尾部已排水后可回收到 mapped_bytes，包括
	 * M 的最后一个不满页；起始仍向上对齐，保护与 H 共页的活跃前缀。
	 * 该契约仅用于整段尾部，不能套用到普通 Release 或中间空洞。
	 */
	return DsgRegionReleasePages(region, result, start, region->mapped_bytes);
}
