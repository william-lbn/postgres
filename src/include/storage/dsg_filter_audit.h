/* Root-only纯编译审计入口；不是安装证明或normal资格。 */
#ifndef DSG_FILTER_AUDIT_H
#define DSG_FILTER_AUDIT_H

#include "storage/dsg_envelope_birth.h"
#define DSG_FILTER_AUDIT_ARG "--neon-dsga-audit-filter-v1"
#define DSG_FILTER_AUDIT_MAX_INSTRUCTIONS 1024

typedef struct DsgFilterAuditInstruction
{
	uint16 code;
	uint8 jt;
	uint8 jf;
	uint32 k;
} DsgFilterAuditInstruction;
typedef struct DsgFilterAuditProgram
{
	uint32 count;
	DsgFilterAuditInstruction instructions[DSG_FILTER_AUDIT_MAX_INSTRUCTIONS];
} DsgFilterAuditProgram;

/* 无PG启动、MemoryContext、文件输入或filter安装；仅精确argv、uid0。 */
extern pg_attribute_noreturn() void DsgFilterAuditMain(int argc, char *argv[]);
/* 与Install复用同Build，下面接口仅编译，不读PG自报安装状态。 */
extern void DsgBirthFilterCompileAudit(const DsgBirthArraysSnapshot *arrays,
		bool child, bool birth_open, DsgFilterAuditProgram *program);
#endif
