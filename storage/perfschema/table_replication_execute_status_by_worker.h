/*
   Copyright (c) 2000, 2013, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */


#ifndef TABLE_REPLICATION_EXECUTE_STATUS_BY_EXECUTOR_H
#define TABLE_REPLICATION_EXECUTE_STATUS_BY_EXECUTOR_H

/**
  @file storage/perfschema/table_replication_execute_status_by_worker.h
  Table replication_execute_status_by_worker (declarations).
*/

#include "pfs_column_types.h"
#include "pfs_engine_table.h"
#include "rpl_mi.h"
#include "mysql_com.h"
#include "rpl_rli_pdb.h"

/**
  @addtogroup Performance_schema_tables
  @{
*/

#ifndef ENUM_RPL_YES_NO
#define ENUM_RPL_YES_NO
/** enumerated values for Service_State of worker thread*/
enum enum_rpl_yes_no {
  PS_RPL_YES= 1, /* Service_State= on */
  PS_RPL_NO /* Service_State= off */
};
#endif

/**
  A row in worker's table. The fields with string values have an additional
  length field denoted by <field_name>_length.
*/
struct st_row_worker {
  /** Worker_Id is added to the table because thread is killed at STOP SLAVE
      but the status needs to show up, so worker_id is used as a permanent
      identifier.
  */
  ulonglong Worker_Id;
  /** Thread_Id field is declared char instead of int because it shows NULL
      when Service_State= off.
  */
  char Thread_Id[sizeof(ulonglong)+1];
  uint Thread_Id_length;
  enum_rpl_yes_no Service_State;
  char Last_Seen_Transaction[Gtid::MAX_TEXT_LENGTH+1];
  uint Last_Seen_Transaction_length;
  uint Last_Error_Number;
  char Last_Error_Message[MAX_SLAVE_ERRMSG];
  uint Last_Error_Message_length;
  char Last_Error_Timestamp[11]; /* TODO: Change to timestamp data type. */
  uint Last_Error_Timestamp_length;
};

/** Table PERFORMANCE_SCHEMA.replication_execute_status_by_worker */
class table_replication_execute_status_by_worker: public PFS_engine_table
{
private:
  void fill_rows(Slave_worker *);

  /** Table share lock. */
  static THR_LOCK m_table_lock;
  /** Fields definition. */
  static TABLE_FIELD_DEF m_field_def;
  /** current row*/
  st_row_worker m_row;

  /** Current position. */
  PFS_simple_index m_pos;
  /** Next position. */
  PFS_simple_index m_next_pos;

protected:
  /**
    Read the current row values.
    @param table            Table handle
    @param buf              row buffer
    @param fields           Table fields
    @param read_all         true if all columns are read.
  */

  virtual int read_row_values(TABLE *table,
                              unsigned char *buf,
                              Field **fields,
                              bool read_all);

  table_replication_execute_status_by_worker();

public:
  ~table_replication_execute_status_by_worker();

  /** Table share. */
  static PFS_engine_table_share m_share;
  static PFS_engine_table* create();
  static ha_rows get_row_count();
  virtual int rnd_next();
  virtual int rnd_pos(const void *pos);
  virtual void reset_position(void);

};

/** @} */
#endif
