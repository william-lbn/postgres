/*-
 * Root-only纯filter编译入口；不安装seccomp，不读取PG安装快照。
 * Root必须先锁本二进制及其动态依赖/RootFS来源，再用实际ptrace内核指令
 * 对比本输出；单独生成字节、hash、length或UID0运行均不证明实际已安装。
 */
#include "postgres.h"
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#include "storage/buf_internals.h"
#include "storage/dsg_filter_audit.h"

static pg_attribute_noreturn() void
AuditFail(void)
{
	const char *message = "DSGA filter audit requires exact root-only ABI1 geometry\n";
	(void) write(STDERR_FILENO, message, strlen(message));
	_exit(1);
}

static uint64
Decimal(const char *raw, uint64 maximum)
{
	uint64 value = 0;
	const char *cursor;
	if (raw == NULL || raw[0] < '1' || raw[0] > '9')
		AuditFail();
	for (cursor = raw; *cursor; cursor++)
	{
		unsigned digit;
		if (*cursor < '0' || *cursor > '9')
			AuditFail();
		digit = *cursor - '0';
		if (value > (maximum - digit) / 10)
			AuditFail();
		value = value * 10 + digit;
	}
	return value;
}

static void
Put16(uint8 *raw, uint16 value)
{
	raw[0] = value >> 8;
	raw[1] = value & 255;
}

static void
Put32(uint8 *raw, uint32 value)
{
	int i;
	for (i = 3; i >= 0; i--)
	{
		raw[i] = value & 255;
		value >>= 8;
	}
}

static void
Put64(uint8 *raw, uint64 value)
{
	int i;
	for (i = 7; i >= 0; i--)
	{
		raw[i] = value & 255;
		value >>= 8;
	}
}

static void
Output(const void *raw, size_t bytes)
{
	const char *cursor = raw;
	while (bytes > 0)
	{
		ssize_t sent = write(STDOUT_FILENO, cursor, bytes);
		if (sent < 0 && errno == EINTR)
			continue;
		if (sent <= 0)
			AuditFail();
		cursor += sent;
		bytes -= sent;
	}
}

pg_attribute_noreturn() void
DsgFilterAuditMain(int argc, char *argv[])
{
	/* 真/有效uid都为0；不接受caller env声明root，也不借setuid路径放行。
	 * main入口须在AS setter/PG MemoryContext/GUC/插件前直接分派到此并_exit。 */
	if (getuid() != 0 || geteuid() != 0 || argc != 12 || strcmp(argv[1], DSG_FILTER_AUDIT_ARG) != 0)
		AuditFail();
#if defined(__linux__) && defined(__x86_64__)
	{
		DsgBirthArraysSnapshot arrays = {0};
		DsgFilterAuditProgram programs[3];
		Size elements[4] = {sizeof(BufferDescPadded), BLCKSZ,
			sizeof(ConditionVariableMinimallyPadded), sizeof(CkptSortItem)};
		uint8 header[144] = {0};
		int i;
		StaticAssertStmt(DSG_BUFFER_ARRAY_COUNT == 4, "audit ABI1 requires four arrays");
		arrays.maximum_buffers = (int) Decimal(argv[3], INT_MAX);
		arrays.address_space_max_bytes = Decimal(argv[2], PG_INT64_MAX);
		arrays.page_size_bytes = 4096;
		arrays.enabled = true;
		if (sysconf(_SC_PAGESIZE) != 4096 || arrays.address_space_max_bytes % (64 << 10) != 0)
			AuditFail();
		for (i = 0; i < 4; i++)
		{
			DsgBirthArrayRange *range = &arrays.ranges[i];
			uint64 base = Decimal(argv[4 + i * 2], (UINT64CONST(1) << 47) - 1);
			uint64 length = Decimal(argv[5 + i * 2], (UINT64CONST(1) << 47) - 1);
			uint64 recomputed;
			int j;
			if ((uint64) arrays.maximum_buffers > (PG_UINT64_MAX - 4095) / elements[i])
				AuditFail();
			recomputed = ((uint64) arrays.maximum_buffers * elements[i] + 4095) & ~UINT64CONST(4095);
			if (base < 4096 || base % 4096 != 0 || length != recomputed ||
				length > (UINT64CONST(1) << 47) - base || length > SIZE_MAX - arrays.reserved_total_bytes)
				AuditFail();
			for (j = 0; j < i; j++)
			{
				uint64 previous = (uint64) (uintptr_t) arrays.ranges[j].region.address;
				if (base < previous + arrays.ranges[j].region.mapped_bytes && previous < base + length)
					AuditFail();
			}
			range->element_bytes = elements[i];
			range->region.address = (char *) (uintptr_t) base;
			range->region.mapped_bytes = length;
			range->region.maximum_bytes = (Size) arrays.maximum_buffers * elements[i];
			range->region.page_bytes = 4096;
			arrays.reserved_total_bytes += length;
		}
		if (arrays.reserved_total_bytes >= arrays.address_space_max_bytes)
			AuditFail();
		/* 同原Install的完整Build：base、final、child，只把原snapshot显式传入。
		 * Root看到的是expected程序，不是本进程或nativePG已安装的自报。 */
		DsgBirthFilterCompileAudit(&arrays, false, true, &programs[0]);
		DsgBirthFilterCompileAudit(&arrays, false, false, &programs[1]);
		DsgBirthFilterCompileAudit(&arrays, true, false, &programs[2]);
		memcpy(header, "NDSGFP01", 8);
		Put32(header + 8, 1);
		Put32(header + 12, 0xc000003eU); /* AUDIT_ARCH_X86_64，显式profile ABI。 */
		Put32(header + 16, 3);
		/* 20:24保留全零；A/M/page/sumR及四range+原生sizeof均完整回显。 */
		Put64(header + 24, arrays.address_space_max_bytes);
		Put32(header + 32, arrays.maximum_buffers);
		Put32(header + 36, arrays.page_size_bytes);
		Put64(header + 40, arrays.reserved_total_bytes);
		for (i = 0; i < 4; i++)
		{
			Put64(header + 48 + 24 * i, (uint64) (uintptr_t) arrays.ranges[i].region.address);
			Put64(header + 56 + 24 * i, arrays.ranges[i].region.mapped_bytes);
			Put64(header + 64 + 24 * i, arrays.ranges[i].element_bytes);
		}
		Output(header, sizeof(header));
		for (i = 0; i < 3; i++)
		{
			uint8 prefix[8];
			uint8 encoded[8 * DSG_FILTER_AUDIT_MAX_INSTRUCTIONS];
			uint32 j;
			if (programs[i].count == 0 || programs[i].count > DSG_FILTER_AUDIT_MAX_INSTRUCTIONS)
				AuditFail();
			Put32(prefix, i + 1); /* 1=PMbase,2=PMfinal,3=child。 */
			Put32(prefix + 4, programs[i].count);
			for (j = 0; j < programs[i].count; j++)
			{
				const DsgFilterAuditInstruction *instruction = &programs[i].instructions[j];
				Put16(encoded + 8 * j, instruction->code);
				encoded[8 * j + 2] = instruction->jt;
				encoded[8 * j + 3] = instruction->jf;
				Put32(encoded + 8 * j + 4, instruction->k);
			}
			Output(prefix, sizeof(prefix));
			Output(encoded, 8 * programs[i].count);
		}
		/* 不调用PG退出/动态库atexit回调，不启动PG或安装任何filter。 */
		_exit(0);
	}
#else
	AuditFail();
#endif
}
