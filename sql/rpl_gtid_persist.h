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

#ifndef RPL_GTID_PERSIST_H_
#define RPL_GTID_PERSIST_H_

#include <string>
using std::string;

class Open_tables_backup;


class Gtid_table_access_context
{

public:
  static const LEX_STRING DB_NAME;
  static const LEX_STRING TABLE_NAME;

  Gtid_table_access_context() : m_drop_thd_object(NULL) { };
  virtual ~Gtid_table_access_context() { };

  /**
    Initialize the gtid table access context as following:
      - Create a new THD if current_thd is NULL
      - Disable binlog temporarily if we are going to modify the table
      - Open and lock a table.

    @param[in/out] thd        Thread requesting to open the table
    @param         lock_type  How to lock the table
    @param[out]    table      We will store the open table here

    @return
      @retval TRUE  failed
      @retval FALSE success
  */
  bool init(THD **thd, TABLE **table, bool is_write);
  /**
    De-initialize the gtid table access context as following:
      - Close the table
      - Reenable binlog if needed
      - Destroy the created THD if needed.

    @param thd         Thread requesting to close the table
    @param table       Table to be closed
    @param error       If there was an error while updating the table
    @param need_commit Need to commit current transaction if it is true
  */
  void deinit(THD *thd, TABLE *table, bool error, bool need_commit);
private:
  /* Pointer to new created THD. */
  THD *m_drop_thd_object;
  /* Modify the table if it is true. */
  bool m_is_write;
  /* Save the lock info. */
  Open_tables_backup m_backup;
  /* Save binlog options. */
  ulonglong m_tmp_disable_binlog__save_options;
  /**
    Creates a new thread in the bootstrap process or in the mysqld startup,
    a thread is created in order to be able to access a table.

    @return
      @retval THD* Pointer to thread structure
  */
  THD *create_thd();
  /**
    Destroys the created thread and restores the
    system_thread information.

    @param thd Thread requesting to be destroyed
  */
  void drop_thd(THD *thd);
  /**
    Opens and locks a table.

    It's assumed that the caller knows what they are doing:
    - whether it was necessary to reset-and-backup the open tables state
    - whether the requested lock does not lead to a deadlock
    - whether this open mode would work under LOCK TABLES, or inside a
    stored function or trigger.

    Note that if the table can't be locked successfully this operation will
    close it. Therefore it provides guarantee that it either opens and locks
    table or fails without leaving any tables open.

    @param      thd           Thread requesting to open the table
    @param      lock_type     How to lock the table
    @param[out] table         We will store the open table here

    @return
      @retval TRUE open and lock failed - an error message is pushed into the
                                          stack
      @retval FALSE success
  */
  bool open_table(THD *thd, enum thr_lock_type lock_type, TABLE **table);
  /**
    Commits the changes, unlocks the table and closes it. This method
    needs to be called even if the open_table fails, in order to ensure
    the lock info is properly restored.

    @param thd    Thread requesting to close the table
    @param table  Table to be closed
    @param error  If there was an error while updating
                    the table
    @param need_commit Need to commit current transaction if
                       it is true.

    If there is an error, rolls back the current statement. Otherwise,
    commits it. However, if a new thread was created and there is an
    error, the transaction must be rolled back. Otherwise, it must be
    committed. In this case, the changes were not done on behalf of
    any user transaction and if not finished, there would be pending
    changes.

  */
  void close_table(THD *thd, TABLE *table, bool error, bool need_commit);
  /* Prevent user from invoking default assignment function. */
  Gtid_table_access_context &operator=(const Gtid_table_access_context &info);
  /* Prevent user from invoking default constructor function. */
  Gtid_table_access_context(const Gtid_table_access_context &info);
};


class Gtid_table_persistor
{

public:
  static const uint number_fields= 3;

  Gtid_table_persistor() : m_count(0) { };
  virtual ~Gtid_table_persistor() { };

  /**
    Insert the gtid into table.

    @param thd  Thread requesting to save gtid into the table
    @param gtid holds the sidno and the gno.

    @retval
      0    OK
    @retval
      1    The table was not found.
    @retval
      -1   Error
  */
  int save(THD *thd, Gtid *gtid);
  /**
    Insert the gtid set into table.

    @param gtid_set  contains a set of gtid, which holds
                     the sidno and the gno.

    @retval
      0    OK
    @retval
      -1   Error
  */
  int save(Gtid_set *gtid_set);
  /**
    Delete all rows from the table.

    @param  thd Thread requesting to reset the table

    @retval
      0    OK
    @retval
      1    The table was not found.
    @retval
      -1   Error
  */
  int reset(THD *thd);

  /**
    Fetch gtids from gtid table and store them into
    gtid_executed set.

    @param[out]  gtid_set store gtids fetched from the gtid table.

    @retval
      0    OK
    @retval
      1    The table was not found.
    @retval
      -1   Error
  */
  int fetch_gtids(Gtid_set *gtid_set);
  /**
    Compress the gtid table completely by employing one
    or more transactions.

    @param  thd Thread requesting to compress the table

    @retval
      0    OK
    @retval
      1    The table was not found.
    @retval
      -1   Error
  */
  int compress(THD *thd);

private:
  /* Count the append size of the table */
  ulong m_count;
  /**
    Compress the gtid table, read each row by the PK(sid, gno_start)
    in increasing order, compress the first consecutive range of
    gtids within a single transaction.

    @param      thd          Thread requesting to compress the table
    @param[out] is_complete  True if the gtid table is compressd completely.

    @retval
      0    OK
    @retval
      1    The table was not found.
    @retval
      -1   Error
  */
  int compress_in_single_transaction(THD *thd, bool &is_complete);
  /**
    Read each row by the PK(sid, gno_start) in increasing order,
    compress the first consecutive range of gtids.
    For example,
      1 1
      2 2
      3 3
      6 6
      7 7
      8 8
    After the compression, the gtids in the table is compressed as following:
      1 3
      6 6
      7 7
      8 8

    @param      table        Reference to a table object.
    @param[out] is_complete  True if the gtid table is compressd completely.

    @return
      @retval 0    OK.
      @retval -1   Error.
  */
  int compress_first_consecutive_range(TABLE *table, bool &is_complete);
  /**
    Fill a gtid interval into fields of the gtid table.

    @param  fields   Reference to table fileds.
    @param  sid      The source id of the gtid interval.
    @param  gno_star The first GNO of the gtid interval.
    @param  gno_end  The last GNO of the gtid interval.

    @return
      @retval 0    OK.
      @retval -1   Error.
  */
  int fill_fields(Field **fields, const char *sid,
                  rpl_gno gno_start, rpl_gno gno_end);
  /**
    Write a gtid interval into the gtid table.

    @param  table    Reference to a table object.
    @param  sid      The source id of the gtid interval.
    @param  gno_star The first GNO of the gtid interval.
    @param  gno_end  The last GNO of the gtid interval.

    @return
      @retval 0    OK.
      @retval -1   Error.
  */
  int write_row(TABLE *table, const char *sid,
                rpl_gno gno_start, rpl_gno gno_end);
  /**
    Update a gtid interval in the gtid table.
    - locate the gtid interval by primary key (sid, gno_start)
      to update it with the new_gno_end.

    @param  table        Reference to a table object.
    @param  sid          The source id of the gtid interval.
    @param  gno_star     The first GNO of the gtid interval.
    @param  new_gno_end  The new last GNO of the gtid interval.

    @return
      @retval 0    OK.
      @retval -1   Error.
  */
  int update_row(TABLE *table, const char *sid,
                 rpl_gno gno_start, rpl_gno new_gno_end);
  /**
    Delete all rows in the gtid_executed table.

    @param  table Reference to a table object.

    @return
      @retval 0    OK.
      @retval -1   Error.
  */
  int delete_all(TABLE *table);
  /**
    Encode the current row fetched from the table into gtid text.

    @param  table Reference to a table object.
    @retval Return the encoded gtid text.
  */
  string encode_gtid_text(TABLE *table);
  /**
    Get gtid interval from the the current row of the table.

    @param  table         Reference to a table object.
    @param  sid[out]      The source id of the gtid interval.
    @param  gno_star[out] The first GNO of the gtid interval.
    @param  gno_end[out]  The last GNO of the gtid interval.
  */
  void get_gtid_interval(TABLE *table, string &sid,
                         rpl_gno &gno_start, rpl_gno &gno_end);
  /**
    Insert the gtid set into table.

    @param table          The gtid_executed table.
    @param gtid_executed  Contains a set of gtid, which holds
                          the sidno and the gno.

    @retval
      0    OK
    @retval
      -1   Error
  */
  int save(TABLE *table, Gtid_set *gtid_set);
  /* Prevent user from invoking default assignment function. */
  Gtid_table_persistor &operator=(const Gtid_table_persistor &info);
  /* Prevent user from invoking default constructor function. */
  Gtid_table_persistor(const Gtid_table_persistor &info);
};

extern Gtid_table_persistor *gtid_table_persistor;
void create_compress_gtid_table_thread();
void terminate_compress_gtid_table_thread();

#endif /* RPL_GTID_PERSIST_H_ */
