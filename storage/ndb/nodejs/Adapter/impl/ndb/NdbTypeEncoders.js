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

/*global unified_debug, build_dir, path */
/*jslint bitwise: true */

"use strict";

var nativeEnc = require(path.join(build_dir, "ndb_adapter.node")).ndb.encoders,
    // charsetMap = null,
    udebug     = unified_debug.getLogger("NdbTypeEncoders.js");

function init() {
  udebug.log("init()");
  // charsetMap = new util.CharsetMap();
  udebug.log("init() done");
}

function NdbEncoder() {}
NdbEncoder.prototype = {
  read           : function(col, buffer, offset) {},
  write          : function(col, value, buffer, offset) {}
};


function NdbStringEncoder() {}
NdbStringEncoder.prototype = {
  lengthEncoder  : new NdbEncoder(),
  lengthBytes    : 0,
  read           : function(col, buffer, offset) {},
  write          : function(col, value, buffer, offset) {}
};


/* Encoder Factory for TINYINT, SMALLINT, INT  -- signed and unsigned */
function makeIntEncoder(type,size) {
  type = type + String(size);
  if(size > 8) {
    type = type + "LE";
  }
  
  var rd_func = "read" + type;
  var wr_func = "write" + type;

  var enc = new NdbEncoder();
  
  enc.read = function read (col, buffer, offset) {
    return buffer[rd_func](offset);
  };
  enc.write = function write (col, value, buffer, offset) { 
    udebug.log("write", wr_func, value);
    return buffer[wr_func](value, offset);
  };

  return enc;
}

/* Medium encoder 
*/
var mediumEncoder = new NdbEncoder();
mediumEncoder.write = function(col, value, buffer, offset) {
  buffer[offset]   = (value & 0xFF);
  buffer[offset+1] = (value & 0xFF00) >> 8;
  buffer[offset+2] = (value & 0xFF0000) >> 16;
};

mediumEncoder.read = function(col, buffer, offset) {
  var value = buffer[offset];
  value |= (buffer[offset+1] << 8);
  value |= (buffer[offset+2] << 16);
  return value;
};


/* BIGINT.  NOT COMPLETE YET. */
var bigIntEncoder = new NdbEncoder();
bigIntEncoder.write = function write(col, value, buffer, offset) {
  buffer.writeInt32LE(value & 0xFFFFFF, offset);
  buffer.writeInt32LE(0, offset+4);  // fixme! 
};

bigIntEncoder.read = function(col, buffer, offset) {
   var value = buffer.readInt32LE(offset);
   return value.toString();
};

/* FLOAT */
var floatEncoder = new NdbEncoder();
floatEncoder.write = function write(col, value, buffer, offset) {
  nativeEnc.writeFloat(value, buffer, offset);
};

floatEncoder.read = function read(col, buffer, offset) {
  return nativeEnc.readFloat(buffer, offset);
};


/* DOUBLE */
var doubleEncoder = new NdbEncoder();
doubleEncoder.write = function write(col, value, buffer, offset) {
  nativeEnc.writeDouble(value, buffer, offset);
};

doubleEncoder.read = function read(col, buffer, offset) {
  return nativeEnc.readDouble(buffer, offset);
};


// pad string with spaces for CHAR column
var spaces = String('                                             ');
function pad(str, finalLen) {
  var result = String(str);

  while(result.length - finalLen > spaces.length) {
    result = result.concat(spaces);
  }
  if(result.length < finalLen) {
    result = result.concat(spaces.slice(0, finalLen - result.length));
  }
  return result;
}

// write string to buffer through Charset recoder
function strWrite(col, value, buffer, offset, length) {
  udebug.log("strWrite");
  // if(! charsetMap) { init(); }
  buffer.write(value, offset, length, 'ascii');
// charsetMap.recodeIn(value, col.charsetNumber, buffer, offset, length);  
}

// read string from buffer through Charset recoder
function strRead(col, buffer, offset, length) {
  var status, r;
  // if(! charsetMap) { init(); }
  // r = charsetMap.recodeOut(buffer, offset, length, col.charsetNumber, status);
  r = buffer.toString('ascii', offset, offset+length);
  return r;
}


/* CHAR */
var CharEncoder = new NdbEncoder();

CharEncoder.write = function(col, obj, buffer, offset) {
  var value = pad(obj, col.length);   /* pad with spaces */
  strWrite(col, value, buffer, offset, col.length);
};

CharEncoder.read = function(col, buffer, offset) {
  return strRead(col, buffer, offset, col.length);
};


/* VARCHAR */
var VarcharEncoder = new NdbStringEncoder();
VarcharEncoder.lengthEncoder = makeIntEncoder("UInt",8);
VarcharEncoder.lengthBytes = 1;

VarcharEncoder.write = function(col, value, buffer, offset) {
  udebug.log("Encoder write");
  var len = value.length;
  this.lengthEncoder.write(col, len, buffer, offset);
  strWrite(col, value, buffer, offset + this.lengthBytes, len);
};

VarcharEncoder.read = function(col, buffer, offset) {
  var len = this.lengthEncoder.read(col, buffer, offset);
  return strRead(col, buffer, offset + this.lengthBytes, len);
};


/* LONGVARCHAR */
var LongVarcharEncoder = new NdbStringEncoder();
LongVarcharEncoder.lengthEncoder = makeIntEncoder("UInt",16);
LongVarcharEncoder.lengthBytes = 2;
LongVarcharEncoder.write = VarcharEncoder.write;
LongVarcharEncoder.read = VarcharEncoder.read;

/* Temporal Types */
function factor_HHMMSS(date, int_date) {
  date.setHours(Math.floor(int_date / 10000));
  date.setMinutes(Math.floor(int_date/100) % 100);
  date.setSeconds(int_date % 100);
}

function factor_YYYYMMDD(date, int_date) {
  date.setFullYear(Math.floor(int_date/10000) % 10000);
  date.setMonth(Math.floor(int_date / 100) % 100);
  date.setDate(int_date % 100);
}

function YYYYMMDD(date) {
  return (date.year * 10000) +
         (date.month * 100) +
         (date.day);
}

function HHMMSS(date) {
  return (date.hour * 10000) + (date.minute * 100) + date.second;
}

/* Stub encoder for DateTime.   Requires 64-bit number support.
*/
var dateTimeEncoder = new NdbEncoder();
dateTimeEncoder.read = function(col, buffer, offset) {
  return 0;
};
dateTimeEncoder.write = function(col, value, buffer, offset) {
  return 0;
};

/* Encoder for Timestamp 
*/
var timeStampEncoder = new NdbEncoder(); 
timeStampEncoder.intEncoder = makeIntEncoder("UInt", 32);

timeStampEncoder.read = function(col, buffer, offset) {
  var i = this.intEncoder.read(col, buffer, offset);
  return new Date(i * 1000);
};

timeStampEncoder.write = function(col, value, buffer, offset) {
  var intValue = Math.floor(value.getTime() / 1000);
  this.intEncoder.write(col, intValue, buffer, offset);
};


/* Time2 (MySQL 5.6)
   Time2 is a 3-byte time plus a 0-to-3 byte fraction
   TODO: Support the fractional part
*/
var time2Encoder = new NdbEncoder();
time2Encoder.auxEncoder = mediumEncoder;

time2Encoder.read = function(col, buffer, offset) { 
  return this.auxEncoder.read(col, buffer, offset);
}

time2Encoder.write = function(col, value, buffer, offset) {
  return this.auxEncoder.write(col, value, buffer, offset);
}

/* Datetime2  (MySQL 5.6)
   big-endian date(5 bytes).fraction(0-3 bytes)
   TODO: Support the fractional part
   TODO: This is probably a *bad* implementation since the 40-bit number
    is too big for a 31-bit V8 small int.   
*/
var datetime2Encoder = new NdbEncoder();
datetime2Encoder.write = function(col, value, buffer, offset) {
  buffer[offset]   = (value & 0xFF);
  buffer[offset+1] = (value & 0xFF00) >> 8;
  buffer[offset+2] = (value & 0xFF0000) >> 16;
  buffer[offset+3] = (value & 0xFF000000) >> 24;
  buffer[offset+4] = (value & 0xFF00000000) >> 32;
};

datetime2Encoder.read = function(col, buffer, offset) {
  var value = buffer[offset];
  value |= (buffer[offset+1] << 8);
  value |= (buffer[offset+2] << 16);
  value |= (buffer[offset+3] << 24);
  value |= (buffer[offset+4] << 32);
  return value;
};

/* Timestamp2 (MySQL 5.6)
   Timestamp2 is a 64-bit unix time in ??? units
   TODO: fix & verify
*/
var timestamp2Encoder = new  NdbEncoder();
timestamp2Encoder.write = function(col, value, buffer, offset) {  
  bigIntEncoder.write(col, value.getTime(), buffer, offset);
}

timestamp2Encoder.read = function(col, buffer, offset) {
  return new Date(bigIntEncoder.read(col, buffer, offset));
}  


var defaultTypeEncoders = [
  null,                                   // 0 
  makeIntEncoder("Int",8),                // 1  TINY INT
  makeIntEncoder("UInt",8),               // 2  TINY UNSIGNED
  makeIntEncoder("Int",16),               // 3  SMALL INT
  makeIntEncoder("UInt",16),              // 4  SMALL UNSIGNED
  mediumEncoder,                          // 5  MEDIUM INT
  mediumEncoder,                          // 6  MEDIUM UNSIGNED
  makeIntEncoder("Int",32),               // 7  INT
  makeIntEncoder("UInt",32),              // 8  UNSIGNED
  bigIntEncoder,         /*fixme*/        // 9  BIGINT
  bigIntEncoder,         /*fixme*/        // 10 BIG UNSIGNED
  floatEncoder,                           // 11 FLOAT
  doubleEncoder,                          // 12 DOUBLE
  null,                                   // OLDDECIMAL
  CharEncoder,                            // 14 CHAR
  VarcharEncoder,                         // 15 VARCHAR
  null,                                   // 16
  null,                                   // 17
  dateTimeEncoder,                        // 18 DATETIME
  null,                                   // 19
  null,                                   // 20
  null,                                   // 21 TEXT
  null,                                   // 22 
  LongVarcharEncoder,                     // 23 LONGVARCHAR
  null,                                   // 24 LONGVARBINARY
  null,                                   // 25
  null,                                   // 26
  timeStampEncoder,                       // 27 TIMESTMAP
  null,                                   // 28 OLDDECIMAL UNSIGNED
  null,                                   // 29 DECIMAL
  null,                                   // 30 DECIMAL UNSIGNED
  time2Encoder,                           // 31 TIME2
  datetime2Encoder,                       // 32 DATETIME2
  timestamp2Encoder,                      // 33 TIMESTAMP2
];

exports.defaultForType = defaultTypeEncoders;

