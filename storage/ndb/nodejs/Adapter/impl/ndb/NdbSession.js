/*
 Copyright (c) 2012, Oracle and/or its affiliates. All rights
 reserved.
 
 This program is free software; you can redistribute it and/or
 modify it under the terms of the GNU General Public License
 as published by the Free Software Foundation; version 2 of
 the License.
 
 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 GNU General Public License for more details.
 
 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 02110-1301  USA
 */

/*global unified_debug, path, build_dir, api_dir */

"use strict";

var adapter        = require(path.join(build_dir, "ndb_adapter.node")),
    ndboperation   = require("./NdbOperation.js"),
    dbtxhandler    = require("./NdbTransactionHandler.js"),
    ndbconnection  = require("./NdbConnectionPool.js"),
    util           = require("util"),
    assert         = require("assert"),
    udebug         = unified_debug.getLogger("NdbSession.js"),
    stats          = require(path.join(api_dir,"stats.js")).getWriter(["spi","ndb","DBSession"]),
    NdbSession;


/** 
  An Ndb object is "single-threaded," meaning the NDB API does not expect
  two uv worker threads to call into it at the same time.  To enforce this,
  we serialize Ndb operations on the execQueue.

  A session has a single transaction visible to the user at any time: 
  NdbSession.tx, which is created in NdbSession.getTransactionHandler() 
  and persists until the user calls commit or rollback (or the transaction
  auto-commits), at which point NdbSession.tx is reset to null.

  It is possible for the user, in one session, to effectively open transactions
  in a loop.  We serialize calls to start a transaction in txQueue, and don't
  allow the next one to open until the current one is closed.
*/


/*** Methods exported by this module but not in the public DBSession SPI ***/


/* newDBSession(sessionImpl) 
   Called from NdbConnectionPool.js to create a DBSession object
*/
exports.newDBSession = function(pool, impl) {
  udebug.log("newDBSession(connectionPool, sessionImpl)");
  var dbSess = new NdbSession();
  dbSess.parentPool = pool;
  dbSess.impl = impl;
  dbSess.execQueue = [];
  dbSess.maxNdbTransactions = 1;  
  dbSess.txQueue = [];
  return dbSess;
};


/* txIsOpen(NdbTransactionHandler) 
   txisClosed(NdbTransactionHandler) 
*/
exports.txIsOpen = function(ndbTransactionHandler) {
  var self = ndbTransactionHandler.dbSession;
  self.openNdbTransactions += 1;
  assert(self.openNdbTransactions <= self.maxNdbTransactions);
};

exports.txIsClosed = function(ndbTransactionHandler) {
  var self = ndbTransactionHandler.dbSession;
  assert(self.openNdbTransactions > 0);
  self.openNdbTransactions -= 1;
};


/* closeActiveTransaction(NdbTransactionHandler) 
*/
exports.closeActiveTransaction = function(ndbTransactionHandler) {
  var self = ndbTransactionHandler.dbSession;
  self.tx = null;
};


/* txCanRunImmediately(NdbTransactionHandler)
*/
exports.txCanRunImmediately = function(ndbTransactionHandler) {
  var self = ndbTransactionHandler.dbSession;
  return (self.openNdbTransactions < self.maxNdbTransactions);
};


/* enqueueTransaction() 
*/
exports.enqueueTransaction = function(ndbTransactionHandler, 
                                      dbOperationList, callback) {
  udebug.log("enqueueTransaction");
  var self = ndbTransactionHandler.dbSession;

  var queueItem = {
    tx:              ndbTransactionHandler,
    dbOperationList: dbOperationList,
    callback:        callback
  };
    
  self.txQueue.push(queueItem);
};


/* runQueuedTransaction()
*/
exports.runQueuedTransaction = function(ndbTransactionHandler) {
  var self = ndbTransactionHandler.dbSession;
  var item = self.txQueue.pop();
  if(item) {
    udebug.log("runQueuedTransaction popped a tx from queue - remaining",
               self.txQueue.length);
    item.tx.execute(item.dbOperationList, item.callback);
  }  
};


/* DBSession Simple Constructor
*/
NdbSession = function() { 
  udebug.log("constructor");
  stats.incr("created");
};

/* NdbSession prototype 
*/
NdbSession.prototype = {
  impl                : null,
  tx                  : null,
  parentPool          : null,
  execQueue           : null,
  openNdbTransactions : 0
};

/*  getConnectionPool() 
    IMMEDIATE
    RETURNS the DBConnectionPool from which this DBSession was created.
*/
NdbSession.prototype.getConnectionPool = function() {
  udebug.log("getConnectionPool");
  return this.parentPool;
};


/* close() 
   ASYNC. Optional callback.
*/
NdbSession.prototype.close = function(userCallback) {
  udebug.log("close");
  
  ndbconnection.closeNdbSession(this.parentPool, this);

  if(userCallback) {
    userCallback(null, null);
  }
};


/* buildReadOperation(DBIndexHandler dbIndexHandler, 
                      Object keys,
                      DBTransactionHandler transaction,
                      function(error, DBOperation) userCallback)
   IMMEDIATE
   Define an operation which when executed will fetch a row.

   RETURNS a DBOperation 
*/
NdbSession.prototype.buildReadOperation = function(dbIndexHandler, keys,
                                                   tx, callback) {
  udebug.log("buildReadOperation");
  var lockMode = "SHARED";
  var op = ndboperation.newReadOperation(tx, dbIndexHandler, keys, lockMode);
  op.userCallback = callback;
  return op;
};


/* buildInsertOperation(DBTableHandler tableHandler, 
                        Object row,
                        DBTransactionHandler transaction,
                        function(error, DBOperation) userCallback)
   IMMEDIATE
   Define an operation which when executed will insert a row.
 
   RETURNS a DBOperation 
*/
NdbSession.prototype.buildInsertOperation = function(tableHandler, row,
                                                    tx, callback) {
  udebug.log("buildInsertOperation " + tableHandler.dbTable.name);
  var op = ndboperation.newInsertOperation(tx, tableHandler, row);
  op.userCallback = callback;
  return op;
};


/* buildWriteOperation(DBIndexHandler dbIndexHandler, 
                       Object row,
                       DBTransactionHandler transaction,
                       function(error, DBOperation) userCallback)
   IMMEDIATE
   Define an operation which when executed will update or insert
 
   RETURNS a DBOperation 
*/
NdbSession.prototype.buildWriteOperation = function(dbIndexHandler, row, 
                                                    tx, callback) {
  udebug.log("buildWriteOperation");
  var op = ndboperation.newWriteOperation(tx, dbIndexHandler, row);
  op.userCallback = callback;
  return op;
};


/* buildUpdateOperation(DBIndexHandler dbIndexHandler,
                        Object keys, 
                        Object values,
                        DBTransactionHandler transaction,
                        function(error, DBOperation) userCallback)
   IMMEDIATE
   Define an operation which when executed will access a row using the keys
   object and update the values provided in the values object.
  
   RETURNS a DBOperation 
*/
NdbSession.prototype.buildUpdateOperation = function(dbIndexHandler, 
                                                     keys, row, tx, userData) {
  udebug.log("buildUpdateOperation");
  var op = ndboperation.newUpdateOperation(tx, dbIndexHandler, keys, row);
  op.userCallback = userData;
  return op;
};


/* buildDeleteOperation(DBIndexHandler dbIndexHandler, 
                        Object keys,
                        DBTransactionHandler transaction,
                        function(error, DBOperation) userCallback)
   IMMEDIATE 
   Define an operation which when executed will delete a row
 
   RETURNS a DBOperation 
*/  
NdbSession.prototype.buildDeleteOperation = function(dbIndexHandler, keys,
                                                     tx, callback) {
  udebug.log("buildDeleteOperation");  
  var op = ndboperation.newDeleteOperation(tx, dbIndexHandler, keys);
  op.userCallback = callback;
  return op;
};


/* getTransactionHandler() 
   IMMEDIATE
   
   RETURNS the current transaction handler, creating it if necessary
*/
NdbSession.prototype.getTransactionHandler = function() {
  udebug.log("getTransactionHandler");
  if(this.tx) {
    udebug.log("getTransactionHandler -- return existing");
  }
  else {
    udebug.log("getTransactionHandler -- return new");
    this.tx = new dbtxhandler.DBTransactionHandler(this);
  }
  return this.tx;
};


/* begin() 
   IMMEDIATE
   
   Begin a user transaction context; exit autocommit mode.
*/
NdbSession.prototype.begin = function() {
  var tx = this.getTransactionHandler();
  assert(tx.executedOperations.length === 0);
  tx.autocommit = false;
};


/* commit(callback) 
   ASYNC
   
   Commit a user transaction.
   Callback is optional; if supplied, will receive (err).
*/
NdbSession.prototype.commit = function(userCallback) {
  this.tx.commit(userCallback);
};


/* rollback(callback) 
   ASYNC
   
   Roll back a user transaction.
   Callback is optional; if supplied, will receive (err).
*/
NdbSession.prototype.rollback = function (userCallback) {
  this.tx.rollback(userCallback);
};

