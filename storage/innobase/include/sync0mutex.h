/*****************************************************************************

Copyright (c) 2012, Oracle and/or its affiliates. All Rights Reserved.

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

/******************************************************************//**
@file include/sync0mutex.h
InnoDB mutex implementation header file

Created 2012-08-15 Sunny Bains.
***********************************************************************/

#include "univ.i"
#include "os0event.h"
#include "sync0types.h"
#include "ut0counter.h"
#include "ib_mutex.h"

#ifndef sync0mutex_h
#define sync0mutex_h

#define mutex_create(N, M)		mutex_init((M), (N), __FILE__, __LINE__)

#define mutex_enter(M)			(M)->enter(__FILE__, __LINE__)

#define mutex_enter_nowait(M)		(M)->trylock(__FILE__, __LINE__)

#define mutex_exit(M)			(M)->exit()

#define mutex_free(M)			mutex_destroy(M)

#ifdef UNIV_DEBUG
/**
Checks that the mutex has been initialized. */
#define mutex_validate(M)		(M)->validate()

/**
Checks that the current thread owns the mutex. Works only
in the debug version. */
#define mutex_own(M)			(M)->is_owned()
#else
#define mutex_own(M)			/* No op */
#define mutex_validate(M)		/* No op */
#endif /* UNIV_DEBUG */

/**
Creates, or rather, initializes a mutex object in a specified memory
location (which must be appropriately aligned). The mutex is initialized
in the reset state. Explicit freeing of the mutex with mutex_free is
necessary only if the memory block containing it is freed.
@param mutex - mutex to initialise
@param name - name of the mutex */

template <typename Mutex>
void mutex_init(
	Mutex*		mutex,		/*!< in: pointer to memory */
	const char*	name,		/*!< in: mutex name */
	const char*	file_name,	/*!< in: file name where created */
	ulint		line)		/*!< in: file line where created */
{
	new(mutex) Mutex();

	mutex->init(name, file_name, line);
}

/**
Removes a mutex object from the mutex list. The mutex is checked to
be in the reset state.
@param mutex - mutex instance to destroy */
template <typename Mutex>
void mutex_destroy(
	Mutex*		mutex)
{
	// Cleanup
	mutex->destroy();
}

#ifdef UNIV_SYNC_DEBUG
/**
Prints debug info of currently reserved mutexes. 
@param file - where to print */
UNIV_INTERN void mutex_list_print_info(FILE* file);
#endif /* UNIV_SYNC_DEBUG */

/**
@return total number of spin rounds since startup. */
UNIV_INTERN ib_uint64_t mutex_spin_round_count_get();

/**
@return total number of spin wait calls since startup. */
UNIV_INTERN ib_uint64_t mutex_spin_wait_count_get();

/**
@return total number of OS waits since startup. */
UNIV_INTERN ib_uint64_t mutex_os_wait_count_get();

extern ib_counter_t<ib_int64_t, IB_N_SLOTS>	mutex_os_wait_count;
extern ib_counter_t<ib_int64_t, IB_N_SLOTS>	mutex_spin_wait_count;
extern ib_counter_t<ib_int64_t, IB_N_SLOTS>	mutex_spin_round_count;

#ifndef UNIV_NONINL
#include "sync0mutex.ic"
#endif /* !UNIV_NOINL */

#endif /* !sync0mutex_h */
