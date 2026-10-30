/*-
 * dsg_envelope_birth.c -- 在任何PG MemoryContext/配置/扩展前预留固定四数组VA。
 *
 * 仅接受 NEON_DSGA_EARLY_ARRAYS_PROFILE=test-v1:<规范maxBuffers>。
 * caller环境不具授权性，首批不支持root-v1，不安装seccomp、不迁cgroup，
 * 不宣称完整包络；Root生产launcher应删除该测试环境，不能把它当资格。
 * 后续真实出生协议必须另行核原Source、PID/start、映射和全部实际过滤规则。
 */
#include "postgres.h"

#include <limits.h>
#include <unistd.h>
#ifndef WIN32
#include <sys/mman.h>
#include <sys/resource.h>
#endif
#ifdef __linux__
#include <sys/prctl.h>
#endif

#include "storage/buf_internals.h"
#include "storage/dsg_envelope_birth.h"

static bool initialized;
static DsgBirthArraysSnapshot birth;

/* 早期没有MemoryContext；失败只写固定说明并立即退出，不elog/分配/回退。 */
static pg_attribute_noreturn() void
DsgBirthArraysFail(const char *reason)
{
	const char *prefix = "DSGA early arrays: ";
	(void) write(STDERR_FILENO, prefix, strlen(prefix));
	(void) write(STDERR_FILENO, reason, strlen(reason));
	(void) write(STDERR_FILENO, "\n", 1);
	_exit(1);
}

static uint64
DsgBirthArraysDecimal(const char *raw, uint64 maximum)
{
	uint64 result = 0;
	const char *cursor;
	if (raw == NULL || raw[0] < '1' || raw[0] > '9')
		DsgBirthArraysFail("expected canonical positive decimal");
	for (cursor = raw; *cursor; ++cursor)
	{
		unsigned digit;
		if (*cursor < '0' || *cursor > '9')
			DsgBirthArraysFail("expected canonical positive decimal");
		digit = *cursor - '0';
		if (result > (maximum - digit) / 10)
			DsgBirthArraysFail("decimal exceeds supported range");
		result = result * 10 + digit;
	}
	return result;
}

void
DsgBirthArraysEarlyInit(void)
{
	const char *raw = getenv(DSG_BIRTH_ARRAYS_PROFILE_ENV);
	if (initialized)
		DsgBirthArraysFail("initialization cannot be repeated");
	initialized = true;
	birth.protocol_version = NEON_DSG_BIRTH_ARRAYS_API_VERSION;
	birth.phase = "disabled";
	if (raw == NULL)
		return;
#if defined(__linux__) && defined(RLIMIT_AS) && defined(MAP_ANONYMOUS)
	{
		const char *names[] = {"descriptors", "blocks", "iocv", "checkpoint"};
		Size elements[] = {sizeof(BufferDescPadded), BLCKSZ,
			sizeof(ConditionVariableMinimallyPadded), sizeof(CkptSortItem)};
		struct rlimit actual;
		long page = sysconf(_SC_PAGESIZE);
		uint64 address_space;
		int i;

		if (strncmp(raw, "test-v1:", 8) != 0)
			DsgBirthArraysFail("only unbound test-v1 is supported; root-v1 is unavailable");
		birth.maximum_buffers = (int) DsgBirthArraysDecimal(raw + 8, INT_MAX);
		/* 原AS环境由main先解析落实；这里再核真实hard/soft，不能只相信文本。 */
		address_space = DsgBirthArraysDecimal(getenv("NEON_DSGA_ADDRESS_SPACE_MAX_BYTES"), PG_INT64_MAX);
		if (getuid() == 0 || getuid() != geteuid() ||
			prctl(PR_GET_NO_NEW_PRIVS, 0L, 0L, 0L, 0L) != 1 ||
			getrlimit(RLIMIT_AS, &actual) != 0 ||
			actual.rlim_cur != actual.rlim_max || (uint64) actual.rlim_max != address_space)
			DsgBirthArraysFail("original unprivileged NNP hard address space is required");
		if (page <= 0 || (page & (page - 1)) != 0)
			DsgBirthArraysFail("invalid actual page size");
		birth.address_space_max_bytes = address_space;
		birth.page_size_bytes = (Size) page;
		/* 先核完整几何/总量再做第一个mmap，所有乘加/向上取整均checked。 */
		for (i = 0; i < DSG_BUFFER_ARRAY_COUNT; ++i)
		{
			DsgBirthArrayRange *range = &birth.ranges[i];
			Size bytes;
			if ((Size) birth.maximum_buffers > (SIZE_MAX - ((Size) page - 1)) / elements[i])
				DsgBirthArraysFail("array geometry overflows");
			bytes = (Size) birth.maximum_buffers * elements[i];
			range->name = names[i];
			range->element_bytes = elements[i];
			range->region.maximum_bytes = bytes;
			range->region.mapped_bytes = TYPEALIGN((Size) page, bytes);
			range->region.page_bytes = (Size) page;
			if (range->region.mapped_bytes > SIZE_MAX - birth.reserved_total_bytes)
				DsgBirthArraysFail("total geometry overflows");
			birth.reserved_total_bytes += range->region.mapped_bytes;
		}
		if (birth.reserved_total_bytes >= address_space)
			DsgBirthArraysFail("reserved arrays must be strictly smaller than original AS");
		for (i = 0; i < DSG_BUFFER_ARRAY_COUNT; ++i)
		{
			DsgRegion *region = &birth.ranges[i].region;
			void *address = mmap(NULL, region->mapped_bytes, PROT_NONE,
							 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
			if (address == MAP_FAILED)
				DsgBirthArraysFail("could not reserve complete arrays");
			region->address = address;
		}
		/* 只发布进程内测试事实；两个授权位始终为false，不用0/空SHA冒充绑定。 */
		birth.enabled = true;
		birth.phase = "reserved_unbound_test";
	}
#else
	DsgBirthArraysFail("early array profile requires supported Linux AS/mmap");
#endif
}

bool
DsgBirthArraysEnabled(void)
{
	return birth.enabled;
}

void
DsgBirthArraysGetSnapshot(DsgBirthArraysSnapshot *snapshot)
{
	*snapshot = birth;
}

void
DsgBirthArraysCheckMaximum(int maximum)
{
	if (birth.enabled && maximum != birth.maximum_buffers)
		elog(FATAL, "DSGA early array maximum differs from actual buffer configuration");
}

void
DsgBirthArraysImport(DsgRegion regions[DSG_BUFFER_ARRAY_COUNT], int maximum)
{
	int i;
	DsgBirthArraysCheckMaximum(maximum);
	if (!birth.enabled || birth.imported)
		elog(FATAL, "DSGA early array reservation is absent or already imported");
	for (i = 0; i < DSG_BUFFER_ARRAY_COUNT; ++i)
		regions[i] = birth.ranges[i].region;
	birth.imported = true;
}
