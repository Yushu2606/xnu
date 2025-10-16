/*
 * Copyright (c) 2024 Apple Inc. All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. The rights granted to you under the License
 * may not be used to create, or enable the creation or redistribution of,
 * unlawful or unlicensed copies of an Apple operating system, or to
 * circumvent, violate, or enable the circumvention or violation of, any
 * terms of an Apple operating system software license agreement.
 *
 * Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */

#if CONFIG_EXCLAVES

#pragma once

#include <sys/cdefs.h>

#include <mach/kern_return.h>

#include "kern/exclaves.tightbeam.h"

__BEGIN_DECLS

/*!
 * @function exclaves_aoe_setup
 *
 * @abstract
 * Called from a thread in an Always-On conclave. Returns the number of message
 * and worker threads required.
 *
 * @param num_message
 * Returns the number of message threads that should be spawned.
 *
 * @param num_worker
 * Returns the number of worker threads that should be spawned.
 *
 * @return
 * KERN_SUCCESS or error code on failure.
 */
extern kern_return_t
exclaves_aoe_setup(uint8_t *num_message, uint8_t *num_worker);

/*!
 * @function exclaves_aoe_teardown
 *
 * @abstract
 * Cleans up state (if any) initialised by exclaves_aoe_setup().
 */
extern void
exclaves_aoe_teardown(void);

/*!
 * @function exclaves_aoe_message_loop
 *
 * @abstract
 * Called from an AOE message thread. Can return with an error if AOE is not
 * supported or uninitialised. Once successfully setup will only ever return if
 * the thread was aborted.
 * Used to handle message delivery.
 */
extern kern_return_t
exclaves_aoe_message_loop(void);

/*!
 * @function exclaves_aoe_work_loop
 *
 * @abstract
 * Called from an AOE worker thread. Can return with an error if AOE is not
 * supported or uninitialised. Once successfully setup will only ever return if
 * the thread was aborted.
 * Worker threads for message processing.
 */
extern kern_return_t
exclaves_aoe_work_loop(void);

/*!
 * @function exclaves_aoe_upcall_work_available
 *
 * @abstract
 * Upcall invoked when AOE proxy has recieved new work that needs to be processed.
 *
 * @param work_info
 * Information on the type of work available.
 *
 * @param completion
 * Tightbeam completion callback.
 *
 * @return
 * TB_ERROR_SUCCESS or error code on failure.
 */
extern tb_error_t
    exclaves_aoe_upcall_work_available(const xnuupcallsv2_aoeworkinfo_s * work_info,
    tb_error_t (^completion)(void));

__END_DECLS

#endif /* CONFIG_EXCLAVES */
