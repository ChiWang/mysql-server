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

#include <stdio.h>

#include <node.h>
#include "v8_binder.h"

#include "adapter_global.h"
#include "AsyncMethodCall.h"
#include "async_common.h"
#include "unified_debug.h"

void work_thd_run(uv_work_t *req) {
  AsyncCall *m = (AsyncCall *) req->data;

  m->run();
  m->handleErrors();
}


void main_thd_complete(uv_work_t *req) {
  v8::HandleScope scope;
  v8::TryCatch try_catch;
  try_catch.SetVerbose(true);

  AsyncCall *m = (AsyncCall *) req->data;

  m->doAsyncCallback(v8::Context::GetCurrent()->Global());

  /* cleanup */
  delete m;
  delete req;

  /* exceptions */
  if(try_catch.HasCaught()) {
    try_catch.ReThrow();
  }
}


void main_thd_complete_newapi(uv_work_t *req, int) {
  main_thd_complete(req);
}


