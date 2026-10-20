/*-------------------------------------------------------------------------
 * message.h
 *	   Exports from replication/logical/message.c
 *
 * Copyright (c) 2013-2024, PostgreSQL Global Development Group
 *
 * src/include/replication/message.h
 *-------------------------------------------------------------------------
 */
#ifndef PG_LOGICAL_MESSAGE_H
#define PG_LOGICAL_MESSAGE_H

#include "access/xlog.h"
#include "access/xlogdefs.h"
#include "access/xlogreader.h"

/*
 * Generic logical decoding message wal record.
 */
typedef struct xl_logical_message
{
	Oid			dbId;			/* database Oid emitted from */
	bool		transactional;	/* is message transactional? */
	Size		prefix_size;	/* length of prefix */
	Size		message_size;	/* size of the message */
	/* payload, including null-terminated prefix of length prefix_size */
	char		message[FLEXIBLE_ARRAY_MEMBER];
} xl_logical_message;

#define SizeOfLogicalMessage	(offsetof(xl_logical_message, message))

/*-
 * Neon AUX文件快照复用逻辑消息WAL，但从来不属于逻辑订阅的应用数据。
 * 保留原有neon-file前缀空间（包括没有冒号的历史名称），让普通/流式
 * 输出、fast-forward检查和SQL准入共享同一判定，避免升级检查永远追不上
 * 自己写出的slot状态。只比较固定前缀；neon-fil等相似应用名称仍须交付。
 * 此函数不赋予写内部WAL的权限；SQL入口必须明确拒绝该保留空间。
 */
static inline bool
IsNeonFileMessage(const char *prefix)
{
	return strncmp(prefix, "neon-file", sizeof("neon-file") - 1) == 0;
}

extern XLogRecPtr LogLogicalMessage(const char *prefix, const char *message,
									size_t size, bool transactional,
									bool flush);

extern void wallog_file(char const* path, uint64_t limit);
extern void wallog_file_descriptor(char const* path, int fd, uint64_t limit);
extern void wallog_file_removal(char const* path);

/* RMGR API */
#define XLOG_LOGICAL_MESSAGE	0x00
extern void logicalmsg_redo(XLogReaderState *record);
extern void logicalmsg_desc(StringInfo buf, XLogReaderState *record);
extern const char *logicalmsg_identify(uint8 info);

#endif							/* PG_LOGICAL_MESSAGE_H */
