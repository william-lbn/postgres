/*-
 * dsg_envelope_birth.h -- 最早四数组VA占位，独立增量ABI1。
 *
 * 显式test-v1/root-v1，默认关闭；本占位接口本身永远不授予Root资格。占位不是RSS、
 * 持续收费包络、seccomp或normal降限许可；不得据本接口解除既有拒绝。
 */
#ifndef DSG_ENVELOPE_BIRTH_H
#define DSG_ENVELOPE_BIRTH_H

#include "storage/dynamic_shared_buffers.h"

#define NEON_DSG_BIRTH_ARRAYS_API_VERSION 1
#define DSG_BIRTH_ARRAYS_PROFILE_ENV "NEON_DSGA_EARLY_ARRAYS_PROFILE"

typedef struct DsgBirthArrayRange
{
	const char *name;
	Size element_bytes;
	DsgRegion region;
} DsgBirthArrayRange;

typedef struct DsgBirthArraysSnapshot
{
	uint32 protocol_version;
	bool enabled;
	bool imported;
	bool source_bound;
	bool root_enrolled;
	const char *phase;
	int maximum_buffers;
	uint64 address_space_max_bytes;
	Size page_size_bytes;
	Size reserved_total_bytes;
	DsgBirthArrayRange ranges[DSG_BUFFER_ARRAY_COUNT];
} DsgBirthArraysSnapshot;

/* 必须且只可在main最早AS落实后、MemoryContext初始化前调用一次。 */
extern void DsgBirthArraysEarlyInit(void);
extern bool DsgBirthArraysEnabled(void);
/* 只读复制本进程私有出生事实，不探测内核、不分配、不伪造Root/PG boot。 */
extern void DsgBirthArraysGetSnapshot(DsgBirthArraysSnapshot *snapshot);
/* Postmaster首次InitBufferPool时移交原占位；后继boot不得重复导入或换几何。 */
extern void DsgBirthArraysImport(DsgRegion regions[DSG_BUFFER_ARRAY_COUNT], int maximum);
extern void DsgBirthArraysCheckMaximum(int maximum);

#endif
