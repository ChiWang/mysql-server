/* Copyright (c) 2014, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; version 2 of the
   License.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
   02110-1301 USA */

#include "log.h"
#include "key.h"
#include "sql_base.h"
#include "sql_parse.h"
#include "replication.h"
#include "rpl_gtid_persist.h"
#include "debug_sync.h"
#include "sql_class.h"
#include "my_global.h"

pthread_t compress_thread_id= 0;
static bool terminate_compress_thread= false;
const LEX_STRING Gtid_table_persistor::TABLE_NAME= {C_STRING_WITH_LEN("gtid_executed")};
const LEX_STRING Gtid_table_persistor::DB_NAME= {C_STRING_WITH_LEN("mysql")};

void init_thd(THD **p_thd)
{
  DBUG_ENTER("init_thd");
  THD *thd= *p_thd;
  thd->thread_stack= reinterpret_cast<char *>(p_thd);
  thd->set_command(COM_DAEMON);
  thd->security_ctx->skip_grants();
  thd->system_thread= SYSTEM_THREAD_COMPRESS_GTID_TABLE;
  thd->store_globals();
  thd->set_time();
  DBUG_VOID_RETURN;
}


void deinit_thd(THD *thd)
{
  DBUG_ENTER("deinit_thd");
  thd->release_resources();
  thd->restore_globals();
  delete thd;
  my_pthread_setspecific_ptr(THR_THD,  NULL);
  DBUG_VOID_RETURN;
}


void Gtid_table_persistor::close_table(THD* thd, TABLE* table,
                                       Open_tables_backup* backup,
                                       bool error, bool need_commit)
{
  Query_tables_list query_tables_list_backup;

  DBUG_ENTER("Gtid_table_persistor::close_table");

  if (table)
  {
    if (error)
      ha_rollback_trans(thd, false);
    else
    {
      /*
        To make the commit not to block with global read lock set
        "ignore_global_read_lock" flag to true.
      */
      ha_commit_trans(thd, false, true);
    }

    if (need_commit)
    {
      if (error)
        ha_rollback_trans(thd, true);
      else
      {
        ha_commit_trans(thd, true, true);
      }
    }

    /*
      In order not to break execution of current statement we have to
      backup/reset/restore Query_tables_list part of LEX, which is
      accessed and updated in the process of closing tables.
    */
    thd->lex->reset_n_backup_query_tables_list(&query_tables_list_backup);
    close_thread_tables(thd);
    thd->lex->restore_backup_query_tables_list(&query_tables_list_backup);
    thd->restore_backup_open_tables_state(backup);
  }
  thd->is_operating_gtid_table= false;

  DBUG_VOID_RETURN;
}


bool Gtid_table_persistor::open_table(THD *thd, enum thr_lock_type lock_type,
                                      TABLE **table,
                                      Open_tables_backup *backup)
{
  DBUG_ENTER("Gtid_table_persistor::open_table");

  TABLE_LIST tables;
  Query_tables_list query_tables_list_backup;

  /* Allow to operate the gtid table when disconnecting the session. */
  uint flags= (MYSQL_OPEN_IGNORE_GLOBAL_READ_LOCK |
               MYSQL_LOCK_IGNORE_GLOBAL_READ_ONLY |
               MYSQL_OPEN_IGNORE_FLUSH |
               MYSQL_LOCK_IGNORE_TIMEOUT |
               MYSQL_OPEN_IGNORE_KILLED);

  /*
    We need to use new Open_tables_state in order not to be affected
    by LOCK TABLES/prelocked mode.
    Also in order not to break execution of current statement we also
    have to backup/reset/restore Query_tables_list part of LEX, which
    is accessed and updated in the process of opening and locking
    tables.
  */
  thd->lex->reset_n_backup_query_tables_list(&query_tables_list_backup);
  thd->reset_n_backup_open_tables_state(backup);

  thd->is_operating_gtid_table= true;
  tables.init_one_table(
      DB_NAME.str, DB_NAME.length,
      TABLE_NAME.str, TABLE_NAME.length,
      TABLE_NAME.str, lock_type);

  tables.open_strategy= TABLE_LIST::OPEN_IF_EXISTS;
  if (!open_n_lock_single_table(thd, &tables, tables.lock_type, flags))
  {
    close_thread_tables(thd);
    thd->restore_backup_open_tables_state(backup);
    thd->lex->restore_backup_query_tables_list(&query_tables_list_backup);
    sql_print_warning("Gtid table is not ready to be used. Table "
                      "'%s.%s' cannot be opened.", DB_NAME.str,
                      TABLE_NAME.str);
    DBUG_RETURN(true);
  }

  DBUG_ASSERT(tables.table->s->table_category == TABLE_CATEGORY_RPL_INFO);

  if (tables.table->s->fields < Gtid_table_persistor::number_fields)
  {
    /*
      Safety: this can only happen if someone started the server and then
      altered the table.
    */
    ha_rollback_trans(thd, false);
    close_thread_tables(thd);
    thd->restore_backup_open_tables_state(backup);
    thd->lex->restore_backup_query_tables_list(&query_tables_list_backup);
    my_error(ER_COL_COUNT_DOESNT_MATCH_CORRUPTED_V2, MYF(0),
             tables.table->s->db.str, tables.table->s->table_name.str,
             Gtid_table_persistor::number_fields, tables.table->s->fields);
    DBUG_RETURN(true);
  }

  thd->lex->restore_backup_query_tables_list(&query_tables_list_backup);

  *table= tables.table;
  tables.table->use_all_columns();
  DBUG_RETURN(false);
}


int Gtid_table_persistor::fill_fields(Field **fields, const char *sid,
                                      rpl_gno gno_start, rpl_gno gno_end)
{
  DBUG_ENTER("Gtid_table_persistor::fill_field");

  /* Store SID */
  fields[0]->set_notnull();
  if (fields[0]->store(sid, rpl_sid::TEXT_LENGTH, &my_charset_bin))
  {
    my_error(ER_RPL_INFO_DATA_TOO_LONG, MYF(0), fields[0]->field_name);
    goto err;
  }

  /* Store gno_start */
  fields[1]->set_notnull();
  if (fields[1]->store(gno_start, true /* unsigned = true*/))
  {
    my_error(ER_RPL_INFO_DATA_TOO_LONG, MYF(0), fields[1]->field_name);
    goto err;
  }

  /* Store gno_end */
  fields[2]->set_notnull();
  if (fields[2]->store(gno_end, true /* unsigned = true*/))
  {
    my_error(ER_RPL_INFO_DATA_TOO_LONG, MYF(0), fields[2]->field_name);
    goto err;
  }

  DBUG_RETURN(0);
err:
  DBUG_RETURN(-1);
}


int Gtid_table_persistor::write_row(TABLE* table, const char *sid,
                                    rpl_gno gno_start, rpl_gno gno_end)
{
  DBUG_ENTER("Gtid_table_persistor::write_row");
  int error= 0;
  Field **fields= NULL;

  fields= table->field;
  empty_record(table);

  if(fill_fields(fields, sid, gno_start, gno_end))
    goto err;

  /* Inserts a new row into the gtid table. */
  if ((error= table->file->ha_write_row(table->record[0])))
  {
    table->file->print_error(error, MYF(0));
    /*
      This makes sure that the error is -1 and not the status returned
      by the handler.
    */
    goto err;
  }

  DBUG_RETURN(0);
err:
  DBUG_RETURN(-1);
}


int Gtid_table_persistor::update_row(TABLE* table, const char *sid,
                                     rpl_gno gno_start, rpl_gno gno_end,
                                     rpl_gno new_gno_end)
{
  DBUG_ENTER("Gtid_table_persistor::update_row");
  int error= 0;
  Field **fields= NULL;
  uchar user_key[MAX_KEY_LENGTH];

  fields= table->field;
  empty_record(table);

  if(fill_fields(fields, sid, gno_start, gno_end))
    DBUG_RETURN(-1);

  key_copy(user_key, table->record[0], table->key_info,
           table->key_info->key_length);

  if ((error= table->file->ha_index_init(0, 1)))
  {
    table->file->print_error(error, MYF(0));
    DBUG_PRINT("info", ("ha_index_init error"));
    goto end;
  }

  if ((error= table->file->ha_index_read_map(table->record[0], user_key,
                                             HA_WHOLE_KEY,
                                             HA_READ_KEY_EXACT)))
  {
    DBUG_PRINT ("info", ("Row not found"));
    goto end;
  }
  else
  {
    DBUG_PRINT("info", ("Row found"));
    store_record(table, record[1]);
  }

  /* Store new_gno_end */
  fields[2]->set_notnull();
  if ((error= fields[2]->store(new_gno_end, true /* unsigned = true*/)))
  {
    my_error(ER_RPL_INFO_DATA_TOO_LONG, MYF(0), fields[2]->field_name);
    goto end;
  }

  /* Update a row in the gtid table. */
  if ((error= table->file->ha_update_row(table->record[1], table->record[0])))
  {
    table->file->print_error(error, MYF(0));
    /*
      This makes sure that the error is -1 and not the status returned
      by the handler.
    */
    goto end;
  }

end:
  table->file->ha_index_end();
  if (error)
    DBUG_RETURN(-1);
  else
    DBUG_RETURN(0);
}


int Gtid_table_persistor::save(THD *thd, Gtid *gtid)
{
  DBUG_ENTER("Gtid_table_persistor::save(THD *thd, Gtid *gtid)");
  int error= 0;
  TABLE *table= NULL;
  char buf[rpl_sid::TEXT_LENGTH + 1];
  ulong saved_mode;
  Open_tables_backup backup;
  THD *drop_thd_object= NULL;

  /* Get source id */
  global_sid_lock->rdlock();
  rpl_sid sid= global_sid_map->sidno_to_sid(gtid->sidno);
  global_sid_lock->unlock();
  sid.to_string(buf);

  if (!thd)
    thd= drop_thd_object= this->create_thd();
  tmp_disable_binlog(thd);
  saved_mode= thd->variables.sql_mode;

  if (this->open_table(thd, TL_WRITE, &table, &backup))
  {
    error= 1;
    goto end;
  }

  /* Save the gtid info into table. */
  error= write_row(table, buf, gtid->gno, gtid->gno);

end:
  this->close_table(thd, table, &backup, 0 != error);
  reenable_binlog(thd);
  thd->variables.sql_mode= saved_mode;
  if (drop_thd_object)
    this->drop_thd(drop_thd_object);
  /* Do not protect m_count for improving transactions' concurrency */
  if (0 == error)
  {
    m_count++;
    if ((executed_gtids_compression_period != 0) &&
        (m_count >= executed_gtids_compression_period ||
         DBUG_EVALUATE_IF("compress_gtid_table", 1, 0) ||
         DBUG_EVALUATE_IF("fetch_compression_thread_stage_info", 1, 0) ||
         DBUG_EVALUATE_IF("simulate_error_on_compress_gtid_table", 1, 0) ||
         DBUG_EVALUATE_IF("simulate_crash_on_compress_gtid_table", 1, 0)))
    {
      m_count= 0;
      mysql_cond_signal(&COND_compress_gtid_table);
    }
  }

  DBUG_RETURN(error);
}


int Gtid_table_persistor::compress_first_consecutive_gtids(TABLE* table)
{
  DBUG_ENTER("Gtid_table_persistor::compress_first_consecutive_gtids");
  int ret= 0;
  int err= 0;
  /* Record the source id in the first consecutive gtid. */
  string sid;
  /* Record the first GNO in the first consecutive gtid. */
  rpl_gno gno_start= 0;
  /* Record the last GNO in the first consecutive gtid. */
  rpl_gno gno_end= 0;
  /* Record the last GNO in the last consecutive gtid. */
  rpl_gno last_gno_end= 0;
  /* Record the gtid interval in the previous gtid. */
  string prev_sid;
  rpl_gno prev_gno_start= 0;
  rpl_gno prev_gno_end= 0;
  /* Record the gtid interval in the current gtid. */
  string cur_sid;
  rpl_gno cur_gno_start= 0;
  rpl_gno cur_gno_end= 0;
  /* True if we find the first consecutive gtid. */
  bool find_first_consecutive_row= false;

  if ((err= table->file->ha_index_init(0, true)))
    DBUG_RETURN(-1);

  /* Read each row by the PK(sid, gno_start) in increasing order. */
  err= table->file->ha_index_first(table->record[0]);
  while(!err)
  {
    get_gtid_interval(table, cur_sid, cur_gno_start, cur_gno_end);
    /*
      Check if gtid intervals of previous gtid and current gtid
      are consecutive.
    */
    if (prev_sid == cur_sid && prev_gno_end + 1 == cur_gno_start)
    {
      if (!find_first_consecutive_row)
      {
        /* Set the gtid interval in the first consecutive gtid. */
        sid= prev_sid;
        gno_start= prev_gno_start;
        gno_end= prev_gno_end;
        find_first_consecutive_row= true;
      }
      /* Delete the consecutive gtid. We do not delete the first
         consecutive gtid, so that we can update it later. */
      if ((err= table->file->ha_delete_row(table->record[0])))
      {
        table->file->print_error(err, MYF(0));
        break;
      }
    }
    else if (find_first_consecutive_row)
      break;

    prev_sid= cur_sid;
    prev_gno_start= cur_gno_start;
    prev_gno_end= cur_gno_end;
    err= table->file->ha_index_next(table->record[0]);
  }

  /*
    Set the last GNO to the last consecutive gtid in the first consecutive
    gtids range in both 'err == HA_ERR_END_OF_FILE' and 'err == 0' cases.
  */
  if (find_first_consecutive_row)
    last_gno_end= prev_gno_end;

  table->file->ha_index_end();
  if (err != HA_ERR_END_OF_FILE && err != 0)
    ret= -1;
  else if (find_first_consecutive_row)
    /* Update the first consecutive gtid. */
    ret= update_row(table, sid.c_str(), gno_start, gno_end, last_gno_end);

  DBUG_RETURN(ret);
}


int Gtid_table_persistor::save(Gtid_set *gtid_set)
{
  DBUG_ENTER("Gtid_table_persistor::save(Gtid_set *gtid_set)");
  int error= 0;
  TABLE *table= NULL;
  ulong saved_mode;
  Open_tables_backup backup;
  THD *thd= current_thd, *drop_thd_object= NULL;

  if (!thd)
    thd= drop_thd_object= this->create_thd();
  tmp_disable_binlog(thd);
  saved_mode= thd->variables.sql_mode;

  if (this->open_table(thd, TL_WRITE, &table, &backup))
  {
    error= 1;
    goto end;
  }

  error= save(table, gtid_set);

end:
  this->close_table(thd, table, &backup, 0 != error);
  reenable_binlog(thd);
  thd->variables.sql_mode= saved_mode;
  if (drop_thd_object)
    this->drop_thd(drop_thd_object);
  DBUG_RETURN(error);
}


int Gtid_table_persistor::save(TABLE* table, Gtid_set *gtid_set)
{
  DBUG_ENTER("Gtid_table_persistor::save(TABLE* table, "
             "Gtid_set *gtid_set)");
  int error= 0;
  list<Gtid_interval> gtid_intervals;
  list<Gtid_interval>::iterator iter;

  /* Get GTID intervals from gtid_set. */
  gtid_set->get_gtid_intervals(&gtid_intervals);
  for (iter= gtid_intervals.begin(); iter != gtid_intervals.end(); iter++)
  {
    /* Get source id. */
    char buf[rpl_sid::TEXT_LENGTH + 1];
    rpl_sid sid= gtid_set->get_sid_map()->sidno_to_sid(iter->sidno);
    sid.to_string(buf);

    /* Save the gtid interval into table. */
    if ((error= write_row(table, buf, iter->gno_start, iter->gno_end)))
      break;
  }

  gtid_intervals.clear();
  DBUG_RETURN(error);
}


/**
  Simulate error and crash in the middle of the transaction
  of compressing gtid table.

  @param  thd Thread requesting to compress the table

  @return
    @retval 0    OK.
    @retval -1   Error.
*/
#ifndef DBUG_OFF
static int dbug_test_on_compress(THD* thd)
{
  DBUG_ENTER("dbug_test_on_compress");
  /*
    Sleep a little, so that notified user thread executed the statement
    completely.
  */
  DBUG_EXECUTE_IF("fetch_compression_thread_stage_info", sleep(5););
  DBUG_EXECUTE_IF("fetch_compression_thread_stage_info",
                  {
                    const char act[]= "now signal fetch_thread_stage";
                    DBUG_ASSERT(opt_debug_sync_timeout > 0);
                    DBUG_ASSERT(!debug_sync_set_action(thd,
                                                       STRING_WITH_LEN(act)));
                  };);
  /* Sleep a little, so that we can always fetch the correct stage info. */
  DBUG_EXECUTE_IF("fetch_compression_thread_stage_info", sleep(1););

  /*
    Simulate error in the middle of the transaction of
    compressing gtid table.
  */
  DBUG_EXECUTE_IF("simulate_error_on_compress_gtid_table",
                  DBUG_RETURN(-1););
  /*
    Wait until notified user thread executed the statement completely,
    then go to crash.
  */
  DBUG_EXECUTE_IF("simulate_crash_on_compress_gtid_table",
                  {
                    const char act[]= "now wait_for notified_thread_complete";
                    DBUG_ASSERT(opt_debug_sync_timeout > 0);
                    DBUG_ASSERT(!debug_sync_set_action(thd,
                                                       STRING_WITH_LEN(act)));
                  };);
  DBUG_EXECUTE_IF("simulate_crash_on_compress_gtid_table", DBUG_SUICIDE(););

  DBUG_RETURN(0);
}
#endif


int Gtid_table_persistor::compress(THD *thd)
{
  DBUG_ENTER("Gtid_table_persistor::compress");
  int error= 0;
  Sid_map sid_map(NULL);
  Gtid_set gtid_deleted(&sid_map);
  TABLE *table= NULL;
  ulong saved_mode;
  Open_tables_backup backup;
  THD *drop_thd_object= NULL;

  if (!thd)
    thd= drop_thd_object= this->create_thd();
  tmp_disable_binlog(thd);
  saved_mode= thd->variables.sql_mode;

  mysql_mutex_lock(&LOCK_compress_gtid_table);
  if (this->open_table(thd, TL_WRITE, &table, &backup))
  {
    error= 1;
    goto end;
  }

  /*
    Reset stage_compressing_gtid_table to overwrite
    stage_system_lock set in open_table(...).
  */
  THD_STAGE_INFO(thd, stage_compressing_gtid_table);

  /*
    In first consecutive gtids range, delete consecutive gtids from
    the second consecutive gtid, then update the first gtid.
  */
  if ((error= compress_first_consecutive_gtids(table)))
    goto end;

#ifndef DBUG_OFF
  error= dbug_test_on_compress(thd);
#endif

end:
  this->close_table(thd, table, &backup, 0 != error, true);
  mysql_mutex_unlock(&LOCK_compress_gtid_table);
  DBUG_EXECUTE_IF("compress_gtid_table",
                  {
                    const char act[]= "now signal complete_compression";
                    DBUG_ASSERT(opt_debug_sync_timeout > 0);
                    DBUG_ASSERT(!debug_sync_set_action(thd,
                                                       STRING_WITH_LEN(act)));
                  };);
  reenable_binlog(thd);
  thd->variables.sql_mode= saved_mode;
  if (drop_thd_object)
    this->drop_thd(drop_thd_object);
  DBUG_RETURN(error);
}


int Gtid_table_persistor::reset(THD *thd)
{
  DBUG_ENTER("Gtid_table_persistor::reset");
  int error= 0;
  TABLE *table= NULL;
  ulong saved_mode;
  Open_tables_backup backup;
  THD *drop_thd_object= NULL;

  if (!thd)
    thd= drop_thd_object= this->create_thd();
  tmp_disable_binlog(thd);
  saved_mode= thd->variables.sql_mode;

  mysql_mutex_lock(&LOCK_compress_gtid_table);
  if (this->open_table(thd, TL_WRITE, &table, &backup))
  {
    error= 1;
    goto end;
  }

  error= delete_all(table);

end:
  this->close_table(thd, table, &backup, 0 != error, true);
  mysql_mutex_unlock(&LOCK_compress_gtid_table);
  reenable_binlog(thd);
  thd->variables.sql_mode= saved_mode;
  if (drop_thd_object)
    this->drop_thd(drop_thd_object);
  DBUG_RETURN(error);
}


THD *Gtid_table_persistor::create_thd()
{
  THD *thd= NULL;
  thd= new THD;
  init_thd(&thd);
  /*
    This is equivalent to a new "statement". For that reason, we call
    both lex_start() and mysql_reset_thd_for_next_command.
  */
  lex_start(thd);
  mysql_reset_thd_for_next_command(thd);

  return(thd);
}


void Gtid_table_persistor::drop_thd(THD *thd)
{
  DBUG_ENTER("Gtid_table_persistor::drop_thd");
  deinit_thd(thd);
  DBUG_VOID_RETURN;
}


string Gtid_table_persistor::encode_gtid_text(TABLE* table)
{
  DBUG_ENTER("Gtid_table_persistor::encode_gtid_text");
  char buff[MAX_FIELD_WIDTH];
  String str(buff, sizeof(buff), &my_charset_bin);

  /* Fetch gtid interval from the table */
  table->field[0]->val_str(&str);
  string gtid_text(str.c_ptr_safe());
  gtid_text.append(Gtid_set::default_string_format.sid_gno_separator);
  table->field[1]->val_str(&str);
  gtid_text.append(str.c_ptr_safe());
  gtid_text.append(Gtid_set::default_string_format.gno_start_end_separator);
  table->field[2]->val_str(&str);
  gtid_text.append(str.c_ptr_safe());

  DBUG_RETURN(gtid_text);
}


void Gtid_table_persistor::get_gtid_interval(TABLE* table, string& sid,
                                            rpl_gno& gno_start,
                                            rpl_gno& gno_end)
{
  DBUG_ENTER("Gtid_table_persistor::get_gtid_interval");
  char buff[MAX_FIELD_WIDTH];
  String str(buff, sizeof(buff), &my_charset_bin);

  /* Fetch gtid interval from the table */
  table->field[0]->val_str(&str);
  sid= string(str.c_ptr_safe());
  gno_start= table->field[1]->val_int();
  gno_end= table->field[2]->val_int();
  DBUG_VOID_RETURN;
}


int Gtid_table_persistor::fetch_gtids(Gtid_set *gtid_set)
{
  DBUG_ENTER("Gtid_table_persistor::fetch_gtids");
  int ret= 0;
  int err= 0;
  TABLE *table= NULL;
  //ulong saved_mode;
  Open_tables_backup backup;
  THD *thd= current_thd, *drop_thd_object= NULL;

  if (!thd)
    thd= drop_thd_object= this->create_thd();

  //tmp_disable_binlog(thd);
  //saved_mode= thd->variables.sql_mode;

  if (this->open_table(thd, TL_READ, &table, &backup))
  {
    ret= 1;
    goto end;
  }

  if ((err= table->file->ha_rnd_init(true)))
  {
    ret= -1;
    goto end;
  }

  while(!(err= table->file->ha_rnd_next(table->record[0])))
  {
    /* Store the gtid into the gtid_set */
    global_sid_lock->wrlock();
    if (gtid_set->add_gtid_text(encode_gtid_text(table).c_str()) !=
        RETURN_STATUS_OK)
    {
      global_sid_lock->unlock();
      break;
    }
    global_sid_lock->unlock();
  }

  table->file->ha_rnd_end();
  if (err != HA_ERR_END_OF_FILE)
    ret= -1;

end:
  this->close_table(thd, table, &backup, 0 != ret, true);
  //reenable_binlog(thd);
  //thd->variables.sql_mode= saved_mode;
  if (drop_thd_object)
    this->drop_thd(drop_thd_object);
  DBUG_RETURN(ret);
}


int Gtid_table_persistor::delete_all(TABLE* table)
{
  DBUG_ENTER("Gtid_table_persistor::delete_all");
  int ret= 0;
  int err= 0;

  if ((err= table->file->ha_rnd_init(true)))
    DBUG_RETURN(-1);

  /*
    Delete all rows in the gtid table. We cannot use truncate(),
    since it is a non-transactional DDL operation.
  */
  while(!(err= table->file->ha_rnd_next(table->record[0])))
  {
    /* Delete current row. */
    if ((err= table->file->ha_delete_row(table->record[0])))
    {
      table->file->print_error(err, MYF(0));
      sql_print_error("Failed to delete the row: '%s' from the gtid "
                      "table.", encode_gtid_text(table).c_str());
      break;
    }
  }

  table->file->ha_rnd_end();
  if (err != HA_ERR_END_OF_FILE)
    ret= -1;

  DBUG_RETURN(ret);
}


pthread_handler_t compress_gtid_table(void *arg)
{
  THD *thd=(THD*) arg;
  mysql_thread_set_psi_id(thd->thread_id);
  my_thread_init();
  DBUG_ENTER("compress_gtid_table");
  init_thd(&thd);
  for (;;)
  {
    if (terminate_compress_thread)
      break;

    mysql_mutex_lock(&LOCK_compress_gtid_table);
    THD_ENTER_COND(thd, &COND_compress_gtid_table,
                   &LOCK_compress_gtid_table,
                   &stage_suspending, NULL);
    mysql_mutex_assert_owner(&LOCK_compress_gtid_table);
    mysql_cond_wait(&COND_compress_gtid_table, &LOCK_compress_gtid_table);
    mysql_mutex_assert_owner(&LOCK_compress_gtid_table);
    THD_EXIT_COND(thd, NULL);

    if (terminate_compress_thread)
      break;

    THD_STAGE_INFO(thd, stage_compressing_gtid_table);
    /* Compressing the gtid table. */
    if (gtid_state->compress(thd))
    {
      sql_print_warning("Failed to compress the gtid table.");
      DBUG_EXECUTE_IF("simulate_error_on_compress_gtid_table",
                      {
                        const char act[]= "now signal compression_failed";
                        DBUG_ASSERT(opt_debug_sync_timeout > 0);
                        DBUG_ASSERT(!debug_sync_set_action(current_thd,
                                                           STRING_WITH_LEN(act)));
                      };);
    }
  }

  deinit_thd(thd);
  DBUG_LEAVE;
  my_thread_end();
  pthread_exit(0);
  return 0;
}


void create_compress_gtid_table_thread()
{
  pthread_attr_t attr;
  int error;
  THD *thd;
  if (!(thd=new THD))
  {
    sql_print_error("Failed to compress the gtid table, because "
                    "it is failed to allocate the THD.");
    return;
  }
  thd->thread_id= thd->variables.pseudo_thread_id= pthread_self();
  THD_CHECK_SENTRY(thd);

  if ((error= pthread_attr_init(&attr)) ||
      (error= pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE)) ||
      (error= pthread_attr_setscope(&attr, PTHREAD_SCOPE_SYSTEM)) ||
      (error= mysql_thread_create(key_thread_compress_gtid_table,
                                  &compress_thread_id, &attr,
                                  compress_gtid_table, (void*) thd)))
    sql_print_warning("Can't create thread to compress gtid table "
                      "(errno= %d)", error);

    (void) pthread_attr_destroy(&attr);
}


void terminate_compress_gtid_table_thread()
{
  DBUG_ENTER("terminate_compress_gtid_table_thread");
  int error= 0;
  terminate_compress_thread= true;
  /* Notify suspended compression thread. */
  mysql_cond_signal(&COND_compress_gtid_table);

  if (compress_thread_id != 0)
  {
#ifdef _WIN32
    HANDLE handle= pthread_get_handle(compress_thread_id);
    if (handle)
      error= pthread_join_with_handle(handle);
#else
    error= pthread_join(compress_thread_id, NULL);
#endif
    compress_thread_id= 0;
  }

  if (0 != error)
    sql_print_warning("Could not join gtid table compression thread. "
                      "error:%d", error);

  DBUG_VOID_RETURN;
}


