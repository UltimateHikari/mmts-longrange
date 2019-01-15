/*-------------------------------------------------------------------------
 *
 * pglogical_proto.h
 *		pglogical protocol
 *
 * Copyright (c) 2015, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  pglogical_proto.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_LOGICAL_PROTO_H
#define PG_LOGICAL_PROTO_H

struct PGLogicalOutputData;
struct PGLRelMetaCacheEntry;

typedef void (*pglogical_write_rel_fn)(StringInfo out, struct PGLogicalOutputData *data,
									   Relation rel/*, struct PGLRelMetaCacheEntry *cache_entry*/);

typedef void (*pglogical_write_begin_fn)(StringInfo out, struct PGLogicalOutputData *data,
							 ReorderBufferTXN *txn);
typedef void (*pglogical_write_message_fn)(StringInfo out, LogicalDecodingContext *ctx,
					XLogRecPtr end_lsn,
					const char *prefix, Size sz, const char *message);
typedef void (*pglogical_write_commit_fn)(StringInfo out, struct PGLogicalOutputData *data,
							 ReorderBufferTXN *txn, XLogRecPtr commit_lsn);

typedef void (*pglogical_write_origin_fn)(StringInfo out, const char *origin,
							 XLogRecPtr origin_lsn);

typedef void (*pglogical_write_insert_fn)(StringInfo out, struct PGLogicalOutputData *data,
							 Relation rel, HeapTuple newtuple);
typedef void (*pglogical_write_update_fn)(StringInfo out, struct PGLogicalOutputData *data,
							 Relation rel, HeapTuple oldtuple,
							 HeapTuple newtuple);
typedef void (*pglogical_write_delete_fn)(StringInfo out, struct PGLogicalOutputData *data,
							 Relation rel, HeapTuple oldtuple);

typedef void (*pglogical_write_caughtup_fn)(StringInfo out, struct PGLogicalOutputData *data,
											XLogRecPtr wal_end_ptr);

typedef void (*write_startup_message_fn)(StringInfo out, List *msg);

typedef void (*pglogical_setup_hooks_fn)(struct PGLogicalHooks* hooks);

typedef struct PGLogicalProtoAPI
{
	pglogical_write_rel_fn		write_rel;
	pglogical_write_begin_fn	write_begin;
	pglogical_write_message_fn	write_message;
	pglogical_write_commit_fn	write_commit;
	pglogical_write_origin_fn	write_origin;
	pglogical_write_insert_fn	write_insert;
	pglogical_write_update_fn	write_update;
	pglogical_write_delete_fn	write_delete;
	pglogical_write_caughtup_fn	write_caughtup;
	pglogical_setup_hooks_fn    setup_hooks;
	write_startup_message_fn	write_startup_message;
} PGLogicalProtoAPI;


typedef enum PGLogicalProtoType
{
	PGLogicalProtoNative,
	PGLogicalProtoJson
} PGLogicalProtoType;

extern PGLogicalProtoAPI *pglogical_init_api(PGLogicalProtoType typ);


extern void pglogical_write_prepare(StringInfo out,
					   struct PGLogicalOutputData *data,
					   ReorderBufferTXN *txn, XLogRecPtr lsn);
extern void pglogical_write_commit_prepared(StringInfo out,
					   struct PGLogicalOutputData *data,
					   ReorderBufferTXN *txn, XLogRecPtr lsn);
extern void pglogical_write_abort_prepared(StringInfo out,
					   struct PGLogicalOutputData *data,
					   ReorderBufferTXN *txn, XLogRecPtr lsn);

#endif /* PG_LOGICAL_PROTO_H */
