/*****************************************************************************

Copyright (c) 2013, Oracle and/or its affiliates. All Rights Reserved.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Suite 500, Boston, MA 02110-1335 USA

*****************************************************************************/

/**************************************************//**
@file row/row0trunc.cc
TRUNCATE implementation

Created 2013-04-12 Sunny Bains
*******************************************************/

#include "row0mysql.h"

#ifdef UNIV_NONINL
#include "row0mysql.ic"
#endif /* UNIV_NOINL */

#include "pars0pars.h"
#include "dict0crea.h"
#include "dict0boot.h"
#include "dict0stats.h"
#include "dict0stats_bg.h"
#include "lock0lock.h"
#include "fts0fts.h"
#include "srv0space.h"

#ifdef UNIV_DEBUG
/** The index number of the second secondary */
static const ulint INDEX_NUM_SECOND_SECONDARY = 3;
#endif /* UNIV_DEBUG */

/**
Iterator over the the raw records in an index, doesn't support MVCC. */
struct IndexIterator {

	IndexIterator(dict_index_t* index, bool rw = false)
		:
		m_rw(rw),
		m_heap(),
		m_index(index)
	{
		/* Do nothing */
	}

	~IndexIterator()
	{
		if (m_heap) {
			mem_heap_free(m_heap);
		}
	}

	/**
	Search for key. Position the cursor on a record GE key.
	@return DB_SUCCESS or error code. */
	dberr_t search(dtuple_t& key);

	/**
	Iterate over all the records
	@return DB_SUCCESS or error code */
	template <typename Callback>
	dberr_t for_each(Callback& callback);

	bool		m_rw;
	mtr_t		m_mtr;
	btr_pcur_t	m_pcur;
	mem_heap_t*	m_heap;
	dict_index_t*	m_index;
};

/**
Search for key, Position the cursor on the first GE reord */
dberr_t
IndexIterator::search(dtuple_t& key)
{
	ut_ad(m_heap == NULL);
	m_heap = mem_heap_create(800);

	mtr_start(&m_mtr);

	/* Scan SYS_INDEXES for all indexes of the table. */

	btr_pcur_open_on_user_rec(
		m_index,
		&key,
		PAGE_CUR_GE,
		(m_rw) ? BTR_MODIFY_LEAF : BTR_SEARCH_LEAF,
		&m_pcur, &m_mtr);

	return(DB_SUCCESS);
}

/**
Iterate over all the records
@return DB_SUCCESS or error code */
template <typename Callback>
dberr_t
IndexIterator::for_each(Callback& callback)
{
	dberr_t	err = DB_SUCCESS;

	callback.begin(&m_mtr);

	for (;;) {

		if (!btr_pcur_is_on_user_rec(&m_pcur)) {
			/* The end of of the index has been reached. */
			err = DB_END_OF_INDEX;
			break;
		}

		rec_t*	rec = btr_pcur_get_rec(&m_pcur);

		if (!rec_get_deleted_flag(rec, FALSE)) {

			err = callback(btr_pcur_get_rec(&m_pcur));

			if (err != DB_SUCCESS) {
				break;
			}
		}

		btr_pcur_move_to_next_user_rec(&m_pcur, &m_mtr);
	}

	callback.end(&m_mtr, err);

	btr_pcur_close(&m_pcur);
	mtr_commit(&m_mtr);

	return(err);
}

/**
Creates a TRUNCATE log record with space id, table name, data directory path,
tablespace flags, table format, index ids, index types, number of index fields
and index field information of the table. */
struct Logger {

	Logger(dict_table_t* table, ulint flags)
		:
		m_table(table),
		m_flags(flags)
	{
		/* Convert to storage byte order. */
		mach_write_to_8(&m_id, m_table->id);
	}

	dberr_t operator()(rec_t* rec)
	{
		ulint		len;
		const byte*	field;

		field = rec_get_nth_field_old(
			rec, DICT_FLD__SYS_INDEXES__TABLE_ID, &len);

		ut_ad(len == 8);

		if (memcmp(&m_id, field, len) != 0) {
			/* End of indexes for the table (TABLE_ID mismatch). */
			return(DB_END_OF_INDEX);
		}

		field = rec_get_nth_field_old(
			rec, DICT_FLD__SYS_INDEXES__TYPE, &len);

		ut_ad(len == 4);

		truncate_t::index_t	index;

		index.m_type = mach_read_from_4(field);

		field = rec_get_nth_field_old(
			rec, DICT_FLD__SYS_INDEXES__ID, &len);

		ut_ad(len == 8);

		index.m_id = mach_read_from_8(field);

		if (fsp_flags_is_compressed(m_flags)) {

			const dict_index_t* dict_index = find(index.m_id);

			if (dict_index != NULL) {
				index.set(dict_index);
			} else {
				ib_logf(IB_LOG_LEVEL_WARN,
					"Index id "IB_ID_FMT " not found",
					index.m_id);
			}
		}

		m_truncate.m_indexes.push_back(index);

		return(DB_SUCCESS);
	}

	/** Called before iterating over the records. 
	@mtr		mini-transaction used by the caller */
	void begin(mtr_t* mtr) const
	{
		/* Do nothing */
	}

	/** Called after iteratoring over the records.
	@param mtr	mini-transaction used by the iterator
	@param err	error code that will be returned by the iterator */
	void end(mtr_t* mtr, dberr_t err)
	{
		if (err == DB_SUCCESS || err == DB_END_OF_INDEX) {

			/* We must find all the index entries on disk. */
			ut_ad(UT_LIST_GET_LEN(m_table->indexes)
			      == m_truncate.m_indexes.size());

			m_truncate.m_dir_path = m_table->data_dir_path;
		}
	}

	/**
	@return pointer to table id storage format buffer */
	table_id_t* table_id()
	{
		return(&m_id);
	}

	/**
	Write the TRUNCATE redo log */
	void log()
	{
		m_truncate.write(
			m_table->space, m_table->name, m_flags, m_table->flags);
	}

private:
	/** Lookup the index using the index id.
	@return index instance if found else NULL */
	const dict_index_t* find(index_id_t id) const
	{
		for (const dict_index_t* index = UT_LIST_GET_FIRST(
				m_table->indexes);
		     index != NULL;
		     index = UT_LIST_GET_NEXT(indexes, index)) {

			if (index->id == id) {
				return(index);
			}
		}

		return(NULL);
	}

private:
	/** Table id in storage format */
	table_id_t		m_id;

	/** Table to be truncated */
	dict_table_t*		m_table;

	/** Tablespace flags */
	ulint			m_flags;

	/** Collect the truncate REDO information */
	truncate_t		m_truncate;
};

/*********************************************************************//**
Create the indexes.
@return DB_SUCCESS or error code */
static __attribute__((warn_unused_result))
dberr_t
row_truncate_create_index(
/*======================*/
	dict_table_t*	table,		/*!< in/out: table */
	trx_t*		trx)		/*!< in/out: transaction covering
					the TRUNCATE */
{
	dict_index_t*	sys_index;
	mem_heap_t*	heap = mem_heap_create(800);
	dtuple_t*	tuple = dtuple_create(heap, 1);
	dfield_t*	dfield = dtuple_get_nth_field(tuple, 0);
	byte*		buf = static_cast<byte*>(mem_heap_alloc(heap, 8));

	mach_write_to_8(buf, table->id);

	dfield_set_data(dfield, buf, 8);

	sys_index = dict_table_get_first_index(dict_sys->sys_indexes);

	dict_index_copy_types(tuple, sys_index, 1);

	mtr_t		mtr;
	btr_pcur_t	pcur;

	mtr_start(&mtr);

	btr_pcur_open_on_user_rec(
		sys_index, tuple, PAGE_CUR_GE, BTR_MODIFY_LEAF, &pcur, &mtr);

#ifdef UNIV_DEBUG
	ulint		ind_count = 0;
#endif /* UNIV_DEBUG */

	/* Create new index trees associated with rows in SYS_INDEXES table */
	for (;;) {

		if (!btr_pcur_is_on_user_rec(&pcur)) {
			/* The end of SYS_INDEXES has been reached. */
			break;
		}

		ulint	len;
		ulint	root_page_no;
		rec_t*	rec = btr_pcur_get_rec(&pcur);

		const byte*	field = rec_get_nth_field_old(
			rec, DICT_FLD__SYS_INDEXES__TABLE_ID, &len);

		ut_ad(len == 8);

		if (memcmp(buf, field, len) != 0) {
			/* End of indexes for the table (TABLE_ID mismatch). */
			break;
		}

		if (rec_get_deleted_flag(rec, FALSE)) {
			/* The index has been dropped. */
			goto next_rec;
		}

		root_page_no = dict_recreate_index_tree(table, &pcur, &mtr);

#ifdef UNIV_DEBUG
		/* Crash during the creation of the second secondary */
		if (++ind_count == INDEX_NUM_SECOND_SECONDARY) {

			/* Waiting for MLOG_FILE_TRUNCATE record is written
			into redo log before the crash. */
			DBUG_EXECUTE_IF("crash_during_create_second_secondary",
				log_buffer_flush_to_disk(););

			DBUG_EXECUTE_IF("crash_during_create_second_secondary",
				DBUG_SUICIDE(););
		}
#endif /* UNIV_DEBUG */

		if (root_page_no != FIL_NULL) {

			page_rec_write_field(
				rec, DICT_FLD__SYS_INDEXES__PAGE_NO,
				root_page_no, &mtr);

			/* We will need to commit and restart the
			mini-transaction in order to avoid deadlocks.
			The dict_create_index_tree() call has allocated
			a page in this mini-transaction, and the rest of
			this loop could latch another index page. */
			mtr_commit(&mtr);

			mtr_start(&mtr);

			btr_pcur_restore_position(BTR_MODIFY_LEAF, &pcur, &mtr);
		} else {
			ulint	zip_size;

			zip_size = fil_space_get_zip_size(table->space);

			if (zip_size == ULINT_UNDEFINED) {
				/* Rollback the truncation and mark the table
				as corrupt if the .ibd file is missing */
				btr_pcur_close(&pcur);

				mtr_commit(&mtr);

				mem_heap_free(heap);

				dict_table_x_unlock_indexes(table);

				trx->error_state = DB_SUCCESS;

				trx_rollback_to_savepoint(trx, NULL);

				trx->error_state = DB_SUCCESS;

				table->corrupted = true;

				return(DB_ERROR);
			}
		}

next_rec:
		btr_pcur_move_to_next_user_rec(&pcur, &mtr);
	}

	btr_pcur_close(&pcur);

	mtr_commit(&mtr);

	mem_heap_free(heap);

	return(DB_SUCCESS);
}

/*********************************************************************//**
Handle FTS truncate issues.
@return DB_SUCCESS or error code. */
static __attribute__((warn_unused_result))
dberr_t
row_truncate_fts(
/*=============*/
	dict_table_t*	table,			/*!< in/out: table being
						truncated */
	table_id_t	new_id,			/*!< in: new id */
	trx_t*		trx)			/*!< in/out: covering
						transaction */
{
	dict_table_t	fts_table;

	fts_table.id = new_id;
	fts_table.name = table->name;

	dberr_t		err;

	err = fts_create_common_tables(trx, &fts_table, table->name, TRUE);

	for (ulint i = 0;
	     i < ib_vector_size(table->fts->indexes) && err == DB_SUCCESS;
	     i++) {

		dict_index_t*	fts_index;

		fts_index = static_cast<dict_index_t*>(
			ib_vector_getp(table->fts->indexes, i));

		err = fts_create_index_tables_low(
			trx, fts_index, table->name, new_id);
	}

	if (err != DB_SUCCESS) {

		trx->error_state = DB_SUCCESS;
		trx_rollback_to_savepoint(trx, NULL);
		trx->error_state = DB_SUCCESS;

		ut_print_timestamp(stderr);
		fputs("  InnoDB: Unable to truncate FTS index for"
			" table", stderr);
		ut_print_name(stderr, trx, TRUE, table->name);
		fputs("\n", stderr);
	} else {
		ut_ad(trx->state != TRX_STATE_NOT_STARTED);
	}

	return(err);
}

/*********************************************************************//**
Truncate index and update SYSTEM TABLES accordingly.
@return DB_SUCCESS or error code */
static __attribute__((warn_unused_result))
dberr_t
row_truncate_drop_indexes(
/*======================*/
	dict_table_t*	table,		/*!< in/out: table */
	trx_t*		trx)		/*!< in/out: transaction covering the
					TRUNCATE */
{
	ut_a(!dict_table_is_temporary(table));

	dict_index_t*	sys_index;
	mem_heap_t*	heap = mem_heap_create(800);
	dtuple_t*	tuple = dtuple_create(heap, 1);
	dfield_t*	dfield = dtuple_get_nth_field(tuple, 0);
	byte*		buf = static_cast<byte*>(mem_heap_alloc(heap, 8));

	mach_write_to_8(buf, table->id);

	dfield_set_data(dfield, buf, 8);

	sys_index = dict_table_get_first_index(dict_sys->sys_indexes);

	dict_index_copy_types(tuple, sys_index, 1);

	mtr_t		mtr;
	btr_pcur_t	pcur;

	mtr_start(&mtr);

	btr_pcur_open_on_user_rec(
		sys_index, tuple, PAGE_CUR_GE, BTR_MODIFY_LEAF, &pcur, &mtr);

#ifdef UNIV_DEBUG
	ulint		ind_count = 0;
#endif /* UNIV_DEBUG */

	/* Scan SYS_INDEXES for all indexes of the table */
	for (;;) {
		ulint		len;
		rec_t*		rec;
		const byte*	field;

		if (!btr_pcur_is_on_user_rec(&pcur)) {
			/* The end of SYS_INDEXES has been reached. */
			break;
		}

		rec = btr_pcur_get_rec(&pcur);

		field = rec_get_nth_field_old(
			rec, DICT_FLD__SYS_INDEXES__TABLE_ID, &len);

		ut_ad(len == 8);

		if (memcmp(buf, field, len) != 0) {
			/* End of indexes for the table (TABLE_ID mismatch). */
			break;
		}
		
		if (rec_get_deleted_flag(rec, FALSE)) {
			/* The index has been dropped. */
			goto next_rec;
		}

		ulint	root_page_no;

		root_page_no = dict_drop_index_tree(rec, &pcur, false, &mtr);

#ifdef UNIV_DEBUG
		/* Crash during the drop of the second secondary */
		if (++ind_count == INDEX_NUM_SECOND_SECONDARY) {

			/* Write and flush the MLOG_FILE_TRUNCATE record
			to the redo log before the crash. */
			DBUG_EXECUTE_IF("crash_during_drop_second_secondary",
					log_buffer_flush_to_disk(););

			DBUG_EXECUTE_IF("crash_during_drop_second_secondary",
					DBUG_SUICIDE(););
		}
#endif /* UNIV_DEBUG */

		DBUG_EXECUTE_IF("crash_if_ibd_file_is_missing",
				root_page_no = FIL_NULL;);

		if (root_page_no != FIL_NULL) {

			/* We will need to commit and restart the
			mini-transaction in order to avoid deadlocks.
			The dict_drop_index_tree() call has freed
			a page in this mini-transaction, and the rest
			of this loop could latch another index page.*/
			mtr_commit(&mtr);

			mtr_start(&mtr);

			btr_pcur_restore_position(BTR_MODIFY_LEAF, &pcur, &mtr);
		} else {
			ulint	zip_size;

			/* Check if the .ibd file is missing. */
			zip_size = fil_space_get_zip_size(table->space);

			DBUG_EXECUTE_IF("crash_if_ibd_file_is_missing",
					zip_size = ULINT_UNDEFINED;);

			if (zip_size == ULINT_UNDEFINED) {
				/* Rollback the truncation and mark the table
				as corrupt if the .ibd file is missing */
				btr_pcur_close(&pcur);

				mtr_commit(&mtr);

				mem_heap_free(heap);

				dict_table_x_unlock_indexes(table);

				trx->error_state = DB_SUCCESS;

				trx_rollback_to_savepoint(trx, NULL);

				trx->error_state = DB_SUCCESS;

				table->corrupted = true;

				return(DB_ERROR);
			}
		}

next_rec:
		btr_pcur_move_to_next_user_rec(&pcur, &mtr);
	}

	btr_pcur_close(&pcur);
	mtr_commit(&mtr);

	mem_heap_free(heap);

	return(DB_SUCCESS);
}

/*********************************************************************//**
Truncatie also results in assignment of new table id, update the system
SYSTEM TABLES with the new id.
@return	error code or DB_SUCCESS */
static __attribute__((warn_unused_result))
dberr_t
row_truncate_update_system_tables(
/*==============================*/
	dict_table_t*	table,			/*!< in/out: table */
	table_id_t	new_id,			/*!< in: new table id */
	ulint		old_space,		/*!< in: old space id */
	bool		has_internal_doc_id,	/*!< in: has doc col (fts) */
	trx_t*		trx)			/*!< in: transaction handle */
{
	pars_info_t*	info	= NULL;
	dberr_t		err	= DB_SUCCESS;

	ut_a(!dict_table_is_temporary(table));

	info = pars_info_create();
	pars_info_add_int4_literal(info, "new_space", (lint) table->space);
	pars_info_add_ull_literal(info, "old_id", table->id);
	pars_info_add_ull_literal(info, "new_id", new_id);

	err = que_eval_sql(
		info,
		"PROCEDURE RENUMBER_TABLE_ID_PROC () IS\n"
		"BEGIN\n"
		"UPDATE SYS_TABLES"
		" SET ID = :new_id, SPACE = :new_space\n"
		" WHERE ID = :old_id;\n"
		"UPDATE SYS_COLUMNS SET TABLE_ID = :new_id\n"
		" WHERE TABLE_ID = :old_id;\n"
		"UPDATE SYS_INDEXES"
		" SET TABLE_ID = :new_id,"
		" SPACE = :new_space\n"
		" WHERE TABLE_ID = :old_id;\n"
		"END;\n"
		, FALSE, trx);

	if (err == DB_SUCCESS && old_space != table->space) {
		info = pars_info_create();

		pars_info_add_int4_literal(
			info, "old_space", (lint) old_space);
		pars_info_add_int4_literal
			(info, "new_space", (lint) table->space);

		err = que_eval_sql(
			info,
			"PROCEDURE "
			"RENUMBER_TABLESPACE_PROC () IS\n"
			"BEGIN\n"
			"UPDATE SYS_TABLESPACES"
			" SET SPACE = :new_space\n"
			" WHERE SPACE = :old_space;\n"
			"UPDATE SYS_DATAFILES"
			" SET SPACE = :new_space"
			" WHERE SPACE = :old_space;\n"
			"END;\n"
			, FALSE, trx);
	}

	DBUG_EXECUTE_IF("ib_ddl_crash_before_fts_truncate", err = DB_ERROR;);

	if (err != DB_SUCCESS) {
		trx->error_state = DB_SUCCESS;
		trx_rollback_to_savepoint(trx, NULL);
		trx->error_state = DB_SUCCESS;

		/* Update system table failed.  Table in memory metadata
		could be in an inconsistent state, mark the in-memory
		table->corrupted to be true. In the long run, this
		should be fixed by atomic truncate table */
		table->corrupted = true;

		ut_print_timestamp(stderr);
		fputs("  InnoDB: Unable to assign a new identifier to table ",
		      stderr);
		ut_print_name(stderr, trx, TRUE, table->name);
		fputs("\n"
		      "InnoDB: after truncating it.  Background processes"
		      " may corrupt the table!\n", stderr);

		/* Failed to update the table id, so drop the new
		FTS auxiliary tables */
		if (has_internal_doc_id) {
			ut_ad(trx->state == TRX_STATE_NOT_STARTED);

			table_id_t	id = table->id;

			table->id = new_id;

			fts_drop_tables(trx, table);

			table->id = id;

			ut_ad(trx->state != TRX_STATE_NOT_STARTED);
		}

		err = DB_ERROR;
	} else {
		/* Drop the old FTS index */
		if (has_internal_doc_id) {
			ut_ad(trx->state != TRX_STATE_NOT_STARTED);
			fts_drop_tables(trx, table);
			ut_ad(trx->state != TRX_STATE_NOT_STARTED);
		}

		DBUG_EXECUTE_IF("ib_truncate_crash_after_fts_drop",
				DBUG_SUICIDE(););

		dict_table_change_id_in_cache(table, new_id);

		/* Reset the Doc ID in cache to 0 */
		if (has_internal_doc_id && table->fts->cache != NULL) {
			table->fts->fts_status |= TABLE_DICT_LOCKED;
			fts_update_next_doc_id(trx, table, NULL, 0);
			fts_cache_clear(table->fts->cache, TRUE);
			fts_cache_init(table->fts->cache);
			table->fts->fts_status &= ~TABLE_DICT_LOCKED;
		}
	}

	return(err);
}


/*********************************************************************//**
Prepare for the truncate process. On success all of the table's indexes will
be locked in X mode.
@param table	table to truncate
@return	error code or DB_SUCCESS */
static __attribute__((warn_unused_result))
dberr_t
row_truncate_prepare(
/*=================*/
	dict_table_t*	table,		/*!< in/out: Table to truncate */
	ulint*		flags)		/*!< out: tablespace flags */
{
	ut_ad(!dict_table_is_temporary(table));
	ut_ad(!Tablespace::is_system_tablespace(table->space));

	*flags = fil_space_get_flags(table->space);

	ut_ad(!DICT_TF2_FLAG_IS_SET(table, DICT_TF2_TEMPORARY));

	dict_get_and_save_data_dir_path(table, true);

	if (*flags != ULINT_UNDEFINED) {

		dberr_t	err = fil_prepare_for_truncate(table->space);

		if (err != DB_SUCCESS) {
			return(err);
		}
	}

	return(DB_SUCCESS);
}

/*********************************************************************//**
Start the truncate process. On success all of the table's indexes will
be locked in X mode.
@param table	table to truncate
@param flags	tablespace flags */
static
void
row_truncate_log(
/*===============*/
	dict_table_t*	table,
	ulint		flags)
{
	ut_ad(!dict_table_is_temporary(table));

	dict_index_t*	sys_index;
	Logger		logger(table, flags);
	byte		buf[DTUPLE_EST_ALLOC(1)];
	dtuple_t*	tuple = dtuple_create_from_mem(buf, sizeof(buf), 1);
	dfield_t*	dfield = dtuple_get_nth_field(tuple, 0);

	dfield_set_data(dfield, logger.table_id(), sizeof(*logger.table_id()));

	sys_index = dict_table_get_first_index(dict_sys->sys_indexes);

	dict_index_copy_types(tuple, sys_index, 1);

	IndexIterator	iterator(sys_index);

	/* Search on the table id and position the cursor on GE table_id. */
	iterator.search(*tuple);

	/* Iterate over all the table's indexes. */
	dberr_t	err = iterator.for_each(logger);

	ut_ad(err == DB_SUCCESS || err == DB_END_OF_INDEX);

	/* Write the TRUNCATE log record into redo log */
	logger.log();
}

/*********************************************************************//**
Do foreign key checks before starting TRUNCATE.
@return DB_SUCCESS or error code */
static __attribute__((warn_unused_result))
dberr_t
row_truncate_foreign_key_checks(
/*============================*/
	const dict_table_t*	table,		/*!< in: table to truncate */
	const trx_t*		trx)		/*!< in: trx covering the
						truncate */
{
	/* Check if the table is referenced by foreign key constraints from
	some other table (not the table itself) */

	dict_foreign_t*	foreign;

	for (foreign = UT_LIST_GET_FIRST(table->referenced_list);
	     foreign != 0 && foreign->foreign_table == table;
	     foreign = UT_LIST_GET_NEXT(referenced_list, foreign)) {

		/* Do nothing. */
	}

	if (!srv_read_only_mode && foreign != NULL && trx->check_foreigns) {

		FILE*	ef = dict_foreign_err_file;

		/* We only allow truncating a referenced table if
		FOREIGN_KEY_CHECKS is set to 0 */

		mutex_enter(&dict_foreign_err_mutex);

		rewind(ef);

		ut_print_timestamp(ef);

		fputs("  Cannot truncate table ", ef);
		ut_print_name(ef, trx, TRUE, table->name);
		fputs(" by DROP+CREATE\n"
		      "InnoDB: because it is referenced by ", ef);
		ut_print_name(ef, trx, TRUE, foreign->foreign_table_name);
		putc('\n', ef);

		mutex_exit(&dict_foreign_err_mutex);

		return(DB_ERROR);
	}

	/* TODO: could we replace the counter n_foreign_key_checks_running
	with lock checks on the table? Acquire here an exclusive lock on the
	table, and rewrite lock0lock.cc and the lock wait in srv0srv.cc so that
	they can cope with the table having been truncated here? Foreign key
	checks take an IS or IX lock on the table. */

	if (table->n_foreign_key_checks_running > 0) {

		ut_print_timestamp(stderr);
		fputs("  InnoDB: Cannot truncate table ", stderr);
		ut_print_name(stderr, trx, TRUE, table->name);
		fputs(" by DROP+CREATE\n"
		      "InnoDB: because there is a foreign key check"
		      " running on it.\n",
		      stderr);

		return(DB_ERROR);
	}

	return(DB_SUCCESS);
}

/*********************************************************************//**
Do some sanity checks before starting the actual TRUNCATE.
@return DB_SUCCESS or error code */
static __attribute__((warn_unused_result))
dberr_t
row_truncate_sanity_checks(
/*=======================*/
	const dict_table_t*	table)		/*!< in: table to truncate */
{
	if (srv_sys_space.created_new_raw()) {

		ib_logf(IB_LOG_LEVEL_INFO,
			"A new raw disk partition was initialized: "
			"we do not allow database modifications by the "
			"user. Shut down mysqld and edit my.cnf so that "
			"newraw is replaced with raw.");

		return(DB_ERROR);

	} else if (dict_table_is_discarded(table)) {

		return(DB_TABLESPACE_DELETED);

	} else if (table->ibd_file_missing) {

		return(DB_TABLESPACE_NOT_FOUND);
	}

	return(DB_SUCCESS);
}

/*********************************************************************//**
Truncates a table for MySQL.
@return	error code or DB_SUCCESS */
UNIV_INTERN
dberr_t
row_truncate_table_for_mysql(
/*=========================*/
	dict_table_t*	table,	/*!< in: table handle */
	trx_t*		trx)	/*!< in: transaction handle */
{
	dberr_t		err;
	table_id_t	new_id;
	ulint		old_space = table->space;

	/* How do we prevent crashes caused by ongoing operations on
	the table? Old operations could try to access non-existent
	pages.

	1) SQL queries, INSERT, SELECT, ...: we must get an exclusive
	InnoDB table lock on the table before we can do TRUNCATE
	TABLE. Then there are no running queries on the table.

	2) Purge and rollback: we assign a new table id for the
	table. Since purge and rollback look for the table based on
	the table id, they see the table as 'dropped' and discard
	their operations.

	3) Insert buffer: TRUNCATE TABLE is analogous to DROP TABLE,
	so we do not have to remove insert buffer records, as the
	insert buffer works at a low level. If a freed page is later
	reallocated, the allocator will remove the ibuf entries for
	it.

	When we prepare to truncate *.ibd files, we remove all entries
	for the table in the insert buffer tree. This is not strictly
	necessary, but we can free up some space in the system tablespace.

	4) Linear readahead and random readahead: we use the same
	method as in 3) to discard ongoing operations. (This is only
	relevant for TRUNCATE TABLE by TRUNCATE TABLESPACE.)

	5) FOREIGN KEY operations: if
	table->n_foreign_key_checks_running > 0, we do not allow the
	TRUNCATE. We also reserve the data dictionary latch. */

	err = row_truncate_sanity_checks(table);

	if (err != DB_SUCCESS) {

		return(err);

	} else if (!dict_table_is_temporary(table)) {

		/* Avoid transaction overhead for temporary table DDL. */
		trx_start_for_ddl(trx, TRX_DICT_OP_TABLE);
	}

	trx->op_info = "truncating table";

	/* Serialize data dictionary operations with dictionary mutex:
	no deadlocks can occur then in these operations */

	ut_a(trx->dict_operation_lock_mode == 0);

	/* Prevent foreign key checks etc. while we are truncating the
	table */

	row_mysql_lock_data_dictionary(trx);

	ut_ad(mutex_own(&(dict_sys->mutex)));
#ifdef UNIV_SYNC_DEBUG
	ut_ad(rw_lock_own(&dict_operation_lock, RW_LOCK_EX));
#endif /* UNIV_SYNC_DEBUG */

	/* Disable background stats collection on the table. */
	dict_stats_wait_bg_to_stop_using_table(table, trx);

	ulint	flags = ULINT_UNDEFINED;

	err = row_truncate_foreign_key_checks(table, trx);

	if (err != DB_SUCCESS) {
		goto funct_exit;
	}

	/* Remove all locks except the table-level X lock. */

	lock_remove_all_on_table(table, FALSE);

	/* Ensure that the table will be dropped by
	trx_rollback_active() in case of a crash. */

	trx->table_id = table->id;

	trx_set_dict_operation(trx, TRX_DICT_OP_TABLE);

	/* Temporary tables don't need undo logging for autocommit stmt.
	On crash (i.e. mysql restart) temporary tables are anyway not
	accessible. */

	if (!dict_table_is_temporary(table)) {

		/* Assign an undo segment for the transaction, so that the
		transaction will be recovereed in case of a crash. */

		mutex_enter(&trx->undo_mutex);

		err = trx_undo_assign_undo(trx, TRX_UNDO_UPDATE);

		mutex_exit(&trx->undo_mutex);

		if (err != DB_SUCCESS) {
			goto funct_exit;
		}
	}

	if (!Tablespace::is_system_tablespace(table->space)
	    && !dict_table_is_temporary(table)) {

		err = row_truncate_prepare(table, &flags);

		if (err != DB_SUCCESS) {
			goto funct_exit;
		}

		/* Lock all index trees for this table, as we will truncate
		the table/index and possibly change their metadata. All
		DML/DDL are blocked by table level lock, with a few exceptions
		such as queries into information schema about the table,
		MySQL could try to access index stats for this kind of query,
		we need to use index locks to sync up */

		dict_table_x_lock_indexes(table);

		row_truncate_log(table, flags);

		/* All of the table's indexes should be locked in X mode. */
	} else {

		/* Lock all index trees for this table, as we will
		truncate the table/index and possibly change their metadata.
		All DML/DDL are blocked by table level lock, with
		a few exceptions such as queries into information schema
		about the table, MySQL could try to access index stats
		for this kind of query, we need to use index locks to sync up */

		dict_table_x_lock_indexes(table);
	}

	DBUG_EXECUTE_IF("crash_after_drop_tablespace",
			log_buffer_flush_to_disk(););

	DBUG_EXECUTE_IF("crash_after_drop_tablespace",
			ut_ad(fil_discard_tablespace(table->space)
			      == DB_SUCCESS););

	DBUG_EXECUTE_IF("crash_after_drop_tablespace", DBUG_SUICIDE(););

	if (!dict_table_is_temporary(table)) {

		err = row_truncate_drop_indexes(table, trx);

		if (err != DB_SUCCESS) {
			goto funct_exit;
		}

	} else {
		/* For temporary tables we don't have entries in SYSTEM
		TABLES. */

#ifdef UNIV_DEBUG
		ulint	ind_count = 0;
#endif /* UNIV_DEBUG */

		for (dict_index_t* index = UT_LIST_GET_FIRST(table->indexes);
		     index != NULL;
		     index = UT_LIST_GET_NEXT(indexes, index)) {

#ifdef UNIV_DEBUG
			/* Crash during the drop of the second secondary
			index on a temporary table. This test doesn't
			really do much because temporary table meta-data
			is not stored in the data dictionary. */
			if (++ind_count == INDEX_NUM_SECOND_SECONDARY) {

				DBUG_EXECUTE_IF(
					"crash_during_drop_second_secondary",
					log_buffer_flush_to_disk(););

				DBUG_EXECUTE_IF(
					"crash_during_drop_second_secondary",
					DBUG_SUICIDE(););
			}
#endif /* UNIV_DEBUG */

			dict_truncate_index_tree_in_mem(index);
		}
	}

	/* Release index tree locks, subsequent work relates to table level
	meta-data changes. */

	if (!Tablespace::is_system_tablespace(table->id)
	    && !dict_table_is_temporary(table)
	    && flags != ULINT_UNDEFINED) {

		fil_reinit_space_header(
			table->space,
			table->indexes.count + FIL_IBD_FILE_INITIAL_SIZE + 1);
	}

	if (!dict_table_is_temporary(table)) {
		/* Recreate all the indexes. */
		err = row_truncate_create_index(table, trx);

		if (err != DB_SUCCESS) {
			goto funct_exit;
		}
	} else {
#ifdef UNIV_DEBUG
		ulint	ind_count = 0;

		for (dict_index_t* index = UT_LIST_GET_FIRST(table->indexes);
		     index != NULL;
		     index = UT_LIST_GET_NEXT(indexes, index)) {

			/* Crash during the drop of the second secondary
			index on a temporary table. This test doesn't
			really do much because temporary table meta-data
			is not stored in the data dictionary. */

			if (++ind_count == INDEX_NUM_SECOND_SECONDARY) {

				DBUG_EXECUTE_IF(
					"crash_during_create_second_secondary",
					log_buffer_flush_to_disk(););

				DBUG_EXECUTE_IF(
					"crash_during_create_second_secondary",
					DBUG_SUICIDE(););
			}
		}
#endif /* UNIV_DEBUG */
	}

	/* Done with index truncation, release index tree locks,
	subsequent work relates to table level metadata change */
	dict_table_x_unlock_indexes(table);

	dict_hdr_get_new_id(&new_id, NULL, NULL, table, false);

	{
		/* Create new FTS auxiliary tables with the new_id, and
		drop the old index later, only if everything runs successful. */
		bool	has_internal_doc_id =
			dict_table_has_fts_index(table)
			|| DICT_TF2_FLAG_IS_SET(table, DICT_TF2_FTS_HAS_DOC_ID);

		if (has_internal_doc_id) {

			err = row_truncate_fts(table, new_id, trx);

			if (err != DB_SUCCESS) {
				goto funct_exit;
			}
		}

		if (dict_table_is_temporary(table)) {

			dict_table_change_id_in_cache(table, new_id);
			err = DB_SUCCESS;

		} else {
			err = row_truncate_update_system_tables(
				table, new_id, old_space,
				has_internal_doc_id, trx);
		}
	}

	/* Reset auto-increment. */
	dict_table_autoinc_lock(table);
	dict_table_autoinc_initialize(table, 1);
	dict_table_autoinc_unlock(table);

	if (trx->state != TRX_STATE_NOT_STARTED) {
		trx_commit_for_mysql(trx);
	}

funct_exit:

	row_mysql_unlock_data_dictionary(trx);

	if (!Tablespace::is_system_tablespace(table->id)
	    && flags != ULINT_UNDEFINED
	    && err == DB_SUCCESS) {

		/* Waiting for MLOG_FILE_TRUNCATE record is written into
		redo log before the crash. */
		DBUG_EXECUTE_IF("crash_before_log_checkpoint",
				log_buffer_flush_to_disk(););

		DBUG_EXECUTE_IF("crash_before_log_checkpoint", DBUG_SUICIDE(););

		/* TODO: do not need to make the checkpoint after
		global data dictionary is introduced. */
		log_make_checkpoint_at(LSN_MAX, TRUE);

		DBUG_EXECUTE_IF("crash_after_log_checkpoint", DBUG_SUICIDE(););

		err = truncate_t::truncate(
			table->space, table->name, table->data_dir_path, flags);

		DBUG_EXECUTE_IF("crash_after_truncate_tablespace",
				DBUG_SUICIDE(););
	}

	dict_stats_update(table, DICT_STATS_EMPTY_TABLE);

	trx->op_info = "";

	/* For temporary tables or if there was an error, we need to reset
	the dict operation flags. */
	trx->ddl = false;
	trx->dict_operation = TRX_DICT_OP_NONE;
	ut_ad(trx->state == TRX_STATE_NOT_STARTED);

	srv_wake_master_thread();

	return(err);
}
