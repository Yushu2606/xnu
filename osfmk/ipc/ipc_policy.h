/*
 * Copyright (c) 2023 Apple Computer, Inc. All rights reserved.
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

#ifndef _IPC_IPC_POLICY_H_
#define _IPC_IPC_POLICY_H_

#include <kern/assert.h>
#include <ipc/port.h>
#include <ipc/ipc_types.h>
#include <ipc/ipc_port.h>


__BEGIN_DECLS __ASSUME_PTR_ABI_SINGLE_BEGIN
#pragma GCC visibility push(hidden)

/*!
 * @file <ipc/ipc_policy.h>
 *
 * @description
 * This file exports interfaces that implement various security policies
 * for Mach IPC.
 */


#pragma mark compile time globals and configurations

/*!
 * @const IPC_HAS_LEGACY_MACH_MSG_TRAP
 * Whether the legacy mach_msg_trap() is (somewhat) supported
 */
#if XNU_TARGET_OS_OSX
#define IPC_HAS_LEGACY_MACH_MSG_TRAP    1
#else
#define IPC_HAS_LEGACY_MACH_MSG_TRAP    0
#endif /* XNU_TARGET_OS_OSX */

/*!
 * @const IPC_KOBJECT_DESC_MAX
 * The maximum number of inline descriptors
 * allowed in an incoming MACH64_SEND_KOBJECT_CALL message.
 */
#define IPC_KOBJECT_DESC_MAX          3
/*!
 * @const IPC_KOBJECT_RDESC_MAX
 * The maximum number of inline descriptors
 * allowed in a reply to a MACH64_SEND_KOBJECT_CALL message.
 */
#define IPC_KOBJECT_RDESC_MAX        32

/*!
 * @const IPC_KMSG_MAX_BODY_SPACE
 * Maximum size of ipc kmsg body sizes (not including trailer or aux).
 */
#define IPC_KMSG_MAX_BODY_SPACE ((64 * 1024 * 1024 * 3) / 4 - MAX_TRAILER_SIZE)

/*!
 * @const IPC_KMSG_MAX_AUX_DATA_SPACE
 * Maximum size for the auxiliary data of an IPC kmsg.
 */
#define IPC_KMSG_MAX_AUX_DATA_SPACE  1024

/*!
 * @const IPC_KMSG_MAX_OOL_PORT_COUNT
 * The maximum number of ports that can be sent at once in a message.
 */
#define IPC_KMSG_MAX_OOL_PORT_COUNT  16383

/*!
 * @const IPC_POLICY_ENHANCED_V0
 * This policy represents platform binaries, hardened-runtime and
 * everything below it.
 */
#define IPC_POLICY_ENHANCED_V0 \
	(IPC_SPACE_POLICY_ENHANCED | IPC_SPACE_POLICY_ENHANCED_V0)

/*!
 * @const IPC_POLICY_ENHANCED_V1
 * This policy represents ES features exposed to 3P in FY2024 release.
 */
#define IPC_POLICY_ENHANCED_V1 \
	(IPC_SPACE_POLICY_ENHANCED | IPC_SPACE_POLICY_ENHANCED_V1)

/*!
 * @const IPC_POLICY_ENHANCED_V2
 * This policy represents ES features exposed to 3P in FY2025 release.
 */
#define IPC_POLICY_ENHANCED_V2 \
	(IPC_SPACE_POLICY_ENHANCED | IPC_SPACE_POLICY_ENHANCED_V2)

#pragma mark policy tunables

__options_decl(ipc_control_port_options_t, uint32_t, {
	ICP_OPTIONS_NONE                = 0x00,

	/* policy for IPC_SPACE_POLICY_{PLATFORM,HARDENED} */
	ICP_OPTIONS_PINNED_1P_SOFT      = 0x01,
	ICP_OPTIONS_PINNED_1P_HARD      = 0x02,
	ICP_OPTIONS_IMMOVABLE_1P_SOFT   = 0x04,
	ICP_OPTIONS_IMMOVABLE_1P_HARD   = 0x08,

	/* policy for other processes */
	ICP_OPTIONS_PINNED_3P_SOFT      = 0x10,
	ICP_OPTIONS_PINNED_3P_HARD      = 0x20,
	ICP_OPTIONS_IMMOVABLE_3P_SOFT   = 0x40,
	ICP_OPTIONS_IMMOVABLE_3P_HARD   = 0x80,
});

/*!
 * @brief
 * Policy for task and thread control ports.
 */
extern ipc_control_port_options_t ipc_control_port_options;

/*!
 * @brief
 * Whether service port defense in depth is enabled.
 */
extern bool service_port_defense_enabled;

/*!
 * @brief
 * Whether out-of-line port array descriptor
 * restrictions are enabled.
 */
extern bool ool_port_array_enforced;

#pragma mark policy utils

/*!
 * @brief
 * Denote that a path is unreachable.
 *
 * @discussion
 * If this codepath is ever reached, it will reliably panic,
 * even on release kernels.
 */
#define ipc_unreachable(reason)         mach_assert_abort(reason)

/*!
 * @brief
 * Performs an invariant check that stays on release kernels.
 */
#define ipc_release_assert(expr)        release_assert(expr)


#pragma mark policy options


/*!
 * @brief
 * Convert mach_msg policy options (originally derived from the current_task space) back into the space namespace
 *
 * @param opts      the options to convert
 *
 * @return          the options for the space
 */
extern ipc_space_policy_t ipc_convert_msg_options_to_space(
	mach_msg_option64_t     opts);

/*!
 * @brief
 * Computes the IPC policy for a given task.
 *
 * @param task          the current task
 */
extern ipc_space_policy_t ipc_policy_for_task(
	task_t                  task);

/*!
 * @brief
 * Derive the current policy flags for the current process.
 *
 * @discussion
 * This function will derive the proper in-kernel mach_msg options
 * from user specified flags and the current context.
 *
 * @param task          the current task
 * @param user_flags    flags passed in from userspace
 */
extern mach_msg_option64_t ipc_current_msg_options(
	task_t                  task,
	mach_msg_option64_t     user_flags);

/*!
 * @brief
 * Preflight send options for invalid combinations
 *
 * @discussion
 * If the send options have "obviously" incorrect parameters,
 * then a mach port guard exception (@c kGUARD_EXC_INVALID_OPTIONS) is raised.
 *
 * @param opts          the mach_msg() options, after sanitization
 *                      via @c ipc_current_msg_options().
 * @returns
 * - MACH_MSG_SUCCESS   success,
 * - MACH_SEND_INVALID_OPTIONS
 *                      for failure cases if MACH64_MACH_MSG2 is set
 * - KERN_NOT_SUPPORTED for failure cases if MACH64_MACH_MSG2 is not set
 */
extern mach_msg_return_t ipc_preflight_msg_option64(
	mach_msg_option64_t     opts);

/*!
 * @brief
 * Determines whether ipc policies should be applied
 *
 * @discussion
 * This checks whether the current policy level matches the policy level
 * of this particular feature, but this helper also allows for various
 * ways for a task to be opted out of ipc security policies, such as if
 * they have the IPC_SPACE_POLICY_SIMULATED, *_TRANSLATED, or *_OPTED_OUT flags.
 *
 * @param current_policy        the policy level for the task/space that we are enforcing on
 * @param requested_level       the policy level that is required to be opted into this enforcement
 *
 * @returns
 * - true       if the current policy level matches the requested policy
 *              level for this feature, and the task is not opted out
 * - false      otherwise
 */
extern bool ipc_should_apply_policy(
	const ipc_space_policy_t current_policy,
	const ipc_space_policy_t requested_level);


#pragma mark legacy trap policies
#if IPC_HAS_LEGACY_MACH_MSG_TRAP

/*!
 * @brief
 * Whether the current task is allowed to use the legacy @c mach_msg_trap().
 *
 * @description
 * If using the legacy mach_msg_trap() is disallowed, this will raise
 * a mach port guard exception (@c kGUARD_EXC_INVALID_OPTIONS).
 *
 * @discussion
 * Nothing should be locked.
 *
 * @param msgid         the message ID of the message being sent
 *                      with the legacy interface.
 * @param opts          the mach_msg() options passed to the legacy interface,
 *                      after sanitization via @c ipc_current_msg_options().
 * @returns
 * - MACH_MSG_SUCCESS   success,
 * - KERN_NOT_SUPPORTED for failure cases.
 */
extern mach_msg_return_t ipc_policy_allow_legacy_send_trap(
	mach_msg_id_t           msgid,
	mach_msg_option64_t     opts);


#endif /* IPC_HAS_LEGACY_MACH_MSG_TRAP */
#pragma mark policy array

/*!
 * @brief
 * Decides the policy around receive right movability.
 *
 * @const IPC_MOVE_POLICY_NEVER
 * Such ports are born in the IO_STATE_IN_SPACE_IMMOVABLE state.
 * Moving or arming port-destroyed notification on such rights
 * is disallowed.
 *
 * @const IPC_MOVE_POLICY_ONCE
 * Such ports are born in the IO_STATE_IN_SPACE state.
 *
 * Arming port destroyed notification on such ports is allowed,
 * and they will move to IO_STATE_IN_SPACE_IMMOVABLE after their first move.
 *
 * Their state will remain IO_STATE_IN_SPACE_IMMOVABLE after a port-destroyed
 * notification fires.
 *
 * @const IPC_MOVE_POLICY_ONCE_OR_AFTER_PD
 * Such ports are born in the IO_STATE_IN_SPACE state.
 *
 * This behaves like @c IPC_MOVE_POLICY_ONCE, but resets to IO_STATE_IN_SPACE
 * after a port-destroyed notification is delivered.
 *
 * @const IPC_MOVE_POLICY_ALWAYS
 * The port is always movable.
 */
__enum_decl(ipc_move_policy_t, uint32_t, {
	IPC_MOVE_POLICY_NEVER,
	IPC_MOVE_POLICY_ONCE,
	IPC_MOVE_POLICY_ONCE_OR_AFTER_PD,
	IPC_MOVE_POLICY_ALWAYS,
});

/*!
 * @brief
 * Type for port policies
 */
typedef const struct ipc_object_policy {
	const char             *pol_name;

	/** see iko_op_stable */
	unsigned long           pol_kobject_stable : 1;
	/** see iko_op_permanent */
	unsigned long           pol_kobject_permanent : 1;

	/** whether the port is movable */
	ipc_move_policy_t       pol_movability : 2;


	/** `mach_port_request_notification` protections */

	/**
	 * allow arming a `MACH_NOTIFY_PORT_DESTROYED` notification
	 * on this receive right
	 */
	unsigned long           pol_notif_port_destroy  : 1;
	/**
	 * allow arming a `MACH_NOTIFY_NO_SENDERS` notification
	 * on this receive right
	 */
	unsigned long           pol_notif_no_senders    : 1;
	/**
	 * allow arming a `MACH_NOTIFY_DEAD_NAME/MACH_NOTIFY_SEND_POSSIBLE`
	 * notification on this receive right
	 */
	unsigned long           pol_notif_dead_name     : 1;


	/** whether the port requires incoming messages to use an IOT_REPLY_PORT properly */
	unsigned long           pol_enforce_reply_semantics : 1;

	/**
	 * whether send rights created on this port are movable,
	 * immovable ports still allow "movement" via MAKE_SEND(_ONCE)
	 */
	unsigned long           pol_movable_send : 1;

	/** required entitlement for platform restrictions binaries to create this port */
	const char *pol_construct_entitlement;

	/** see iko_op_no_senders */
	void                  (*pol_kobject_no_senders)(
		ipc_port_t              port,
		mach_port_mscount_t     mscount);

	/** destroys the label for this port */
	void                  (*pol_label_free)(
		ipc_object_label_t      label);
} *ipc_object_policy_t;

/*!
 * @brief
 * Array of policies per port type.
 */
extern struct ipc_object_policy ipc_policy_array[IOT_UNKNOWN];

/*!
 * @brief
 * Returns the policy for a given type/object/port/...
 */
__attribute__((overloadable, always_inline, const))
static inline ipc_object_policy_t
ipc_policy(ipc_object_type_t otype)
{
	ipc_release_assert(otype < IOT_UNKNOWN);
	return &ipc_policy_array[otype];
}

__attribute__((overloadable, always_inline, const))
static inline ipc_object_policy_t
ipc_policy(ipc_object_label_t label)
{
	return ipc_policy(label.io_type);
}

__attribute__((overloadable, always_inline, const))
static inline ipc_object_policy_t
ipc_policy(ipc_object_t object)
{
	return ipc_policy(object->io_type);
}

__attribute__((overloadable, always_inline, const))
static inline ipc_object_policy_t
ipc_policy(ipc_port_t port)
{
	return ipc_policy(ip_type(port));
}


#pragma mark ipc policy telemetry [temporary]

/* The bootarg to disable ALL ipc policy violation telemetry */
extern bool ipcpv_telemetry_enabled;

/* Enables reply port/voucher/persona debugging code */
extern bool enforce_strict_reply;

extern bool prp_enforcement_enabled;

extern bool bootstrap_port_telemetry_enabled;

/*!
 * @brief
 * Identifier of the type of ipc policy violation in a CA telemetry event
 *
 * Currently we only send reply port related violations to CA. This enum can
 * be extended to report more violations in the future.
 */
__enum_closed_decl(ipc_policy_violation_id_t, uint8_t, {
	IPCPV_VIOLATION_NONE, /* 0, denote no violations */

	/* Kobject Reply Port and Move Reply Port violators Start */
	IPCPV_REPLY_PORT_SEMANTICS, /* 1, normal reply port semantics violator */
	/* [2-5] were previously used; should be avoided to avoid telemetry confusion */
	__UNUSED2, /* previously used, should be avoided */
	__UNUSED3, /* previously used, should be avoided */
	__UNUSED4, /* previously used, should be avoided */
	__UNUSED5, /* previously used, should be avoided */
	/* services opted out of reply port semantics previously should have fixed their violations */
	IPCPV_REPLY_PORT_SEMANTICS_OPTOUT, /* 6 */
	/* Kobject Reply Port and Move Reply Port Violators End */

	/* Service Port Defense Violators Start */
	IPCPV_MOVE_SERVICE_PORT, /* 7 */
	IPCPV_SERVICE_PORT_PD_NOTIFICATION, /* 8, for future telemetry */
	/* Service Port Defense Violators End */

	/*
	 * [9-12] were previously used for OOL port array restrictions;
	 * these should be avoided to avoid telemetry confusion
	 */
	__UNUSED6, /* 9 previously used, should be avoided */
	__UNUSED7, /* 10 previously used, should be avoided */
	__UNUSED8, /* 11 previously used, should be avoided */
	__UNUSED9, /* 12 previously used, should be avoided */
	/* OOL ports array violators End */

	/* Bootstrap port reply port semantics violators Start */
	IPCPV_BOOTSTRAP_PORT, /* 13 */
	/* Bootstrap port reply port semantics violators End */

	_IPCPV_VIOLATION_COUNT,
});

/*!
 * @brief
 * Record ipc policy violations into a buffer for sending to CA at a later time.
 *
 * @discussion
 * The ipc telemetry lock is not locked.
 *
 * @param violation_id      type of ipc policy violation
 * @param service_port      service port involved in violation, if any
 * @param aux_data          additional data to include in the CA event:
 *                          violator msgh_id for reply port defense
 */
extern void ipc_stash_policy_violations_telemetry(
	ipc_policy_violation_id_t   violation_id,
	ipc_port_t                  service_port,
	int                         aux_data);

#if DEBUG || DEVELOPMENT
/*!
 * @brief
 * Helper function to record the total number of ipcpv violation occured.
 * Telemetry count should be 0 in presub testing as we shouldn't emit any
 * telemetry for known issue.
 */
extern void ipc_inc_telemetry_count(void);
#endif /* DEBUG || DEVELOPMENT */

/*!
 * @brief
 * Check if the ipc space has emitted a certain type of telemetry.
 *
 * @param is      ipc space in question
 * @param type    ipc policy violation type
 */
__attribute__((always_inline))
static inline bool
ipc_space_has_telemetry_type(ipc_space_t is, uint8_t type)
{
	if (!ipcpv_telemetry_enabled) {
		return true;
	}

#if DEBUG || DEVELOPMENT
	ipc_inc_telemetry_count();
#endif

	return (os_atomic_or_orig(&is->is_telemetry, type, relaxed) & type) != 0;
}

#pragma mark MACH_SEND_MSG policies

/*!
 * @brief
 * Validation function that runs after the message header bytes have been copied
 * from user, but before any other content or right is copied in.
 *
 * @discussion
 * Nothing should be locked.
 *
 * @param hdr           the user message header bytes, before anything
 *                      else has been copied in.
 * @param dsc_count     the number of inline descriptors for the user message.
 * @param opts          the mach_msg() options, after sanitization
 *                      via @c ipc_current_msg_options().
 *
 * @returns
 * - MACH_MSG_SUCCESS   the message passed validation
 * - MACH_SEND_TOO_LARGE
 *                      a MACH64_SEND_KOBJECT_CALL had too many descriptors.
 * - MACH_MSG_VM_KERNEL the message would use more than ipc_kmsg_max_vm_space
 *                      of kernel wired memory.
 */
extern mach_msg_return_t ipc_validate_kmsg_header_schema_from_user(
	mach_msg_user_header_t *hdr,
	mach_msg_size_t         dsc_count,
	mach_msg_option64_t     opts);

/*!
 * @brief
 * Validation function that runs after the message bytes has been copied from
 * user, but before any right is copied in.
 *
 * @discussion
 * Nothing should be locked.
 *
 * @param kdata         the "kernel data" part of the incoming message.
 *                      the descriptors data is copied in "kernel" format.
 * @param send_uctx     the IPC kmsg send context for the current send operation.
 * @param opts          the mach_msg() options, after sanitization
 *                      via @c ipc_current_msg_options().
 *
 * @returns
 * - MACH_MSG_SUCCESS   the message passed validation
 * - MACH_SEND_TOO_LARGE
 *                      a MACH64_SEND_KOBJECT_CALL had too many descriptors.
 * - MACH_MSG_VM_KERNEL the message would use more than ipc_kmsg_max_vm_space
 *                      of kernel wired memory.
 */
extern mach_msg_return_t ipc_validate_kmsg_schema_from_user(
	mach_msg_header_t      *kdata,
	mach_msg_send_uctx_t   *send_uctx,
	mach_msg_option64_t     opts);

/*!
 * @brief
 * Validation function that runs after the rights in the message header have
 * been copied in.
 *
 * @discussion
 * Nothing should be locked.
 *
 * @param hdr           the copied in message header.
 * @param send_uctx     the IPC kmsg send context for the current send operation.
 * @param opts          the mach_msg() options, after sanitization
 *                      via @c ipc_current_msg_options().
 * @returns
 * - MACH_MSG_SUCCESS   the message passed validation
 * - MACH_SEND_INVALID_OPTIONS
 *                      some options are incompatible with the destination
 *                      of the message. a kGUARD_EXC_INVALID_OPTIONS guard
 *                      will be raised.
 * - MACH_SEND_MSG_FILTERED
 *                      the message failed a filtering check.
 *                      a kGUARD_EXC_MSG_FILTERED guard might be raised.
 */
extern mach_msg_return_t ipc_validate_kmsg_header_from_user(
	mach_msg_header_t      *hdr,
	mach_msg_send_uctx_t   *send_uctx,
	mach_msg_option64_t     opts);


#pragma mark port type policies and callouts

/*!
 * @brief
 * Frees a label value according to the port type callout.
 *
 * @param label         The label to destroy.
 */
static inline void
ipc_port_label_free(ipc_object_label_t label)
{
	if (label.iol_pointer) {
		ipc_policy(label)->pol_label_free(label);
	}
}

#pragma mark send immovability

/*!
 * @brief
 * Returns whether an entry for this port should be marked as immovable send
 *
 * @param task      The task where the new entry is being created/copied out
 * @param port      The port that the entry is being created/copied out for
 *
 * @returns
 *      - true  The send right entry should be marked as immovable
 *      - false The send right entry should not be marked as immovable
 */
extern bool ipc_should_mark_immovable_send(
	task_t      task,
	ipc_port_t  port,
	ipc_object_label_t label);

/*!
 * @brief
 * Determine whether we need to protect this port from being stashed as a naked
 * send right in the kernel. We disallow this if the port is supposed to be immovable send
 * as this would allow userspace to bypass the immovable send checks and move the send
 * right to another process.
 *
 * @param port      The port that we want to protect
 *
 * @returns
 *  - true          The port is allowed to be stashed
 *  - false         The port is immovable send and should not be stashed
 */
extern bool ipc_can_stash_naked_send(
	ipc_port_t port);


#pragma mark entry init

/*!
 * @brief
 * Initialize the security fields/flags on a new right entry created through the
 * new port creation path. This right could be any port or port set right.
 *
 * @param space         The space this entry is being created in
 * @param object        The *initialized* port/portset object that is getting a new entry
 * @param type          The type of this entry (send, send-once, receive, deadname, portset)
 * @param entry         Pointer to the entry that is being initialized
 * @param urefs         Number of refs this entry will be initialized to
 * @param name          The name this entry will occupy in the space
 */
extern void ipc_entry_init(
	ipc_space_t         space,
	ipc_object_t        object,
	mach_port_type_t    type,
	ipc_entry_t         entry,
	mach_port_urefs_t   urefs,
	mach_port_name_t    name);


#pragma mark receive immovability

/*!
 * @brief
 * Returns whether the receive right of a port is allowed to move out
 * of an ipc space.
 *
 * Condition: Space is write-locked and active. Port is not locked.
 *
 * @param space     The ipc space to copyin from
 * @param port      The port whose receive right is being moved
 *
 * @returns
 *      - true  The receive right can move out of the space
 *      - false The receive right can not move out of the space
 */
extern bool ipc_move_receive_allowed(
	ipc_space_t             space,
	ipc_port_t              port,
	mach_port_name_t        name);


#pragma mark policy guard violations

/*!
 * @brief
 * Marks the thread with AST_MACH_EXCEPTION for mach port guard violation.
 *
 * @discussion
 * Also saves exception info in thread structure.
 *
 * @param target        The port target of the exception (often a port name)
 * @param payload       A 64bit value that will be put in the guard "subcode".
 * @param reason        A valid mach_port_guard_exception_codes value.
 */
__cold
extern void mach_port_guard_exception(
	uint32_t                target,
	uint64_t                payload,
	unsigned                reason);

/*!
 * @brief
 * Deliver a soft or hard immovable guard exception.
 *
 * @param space         The space causing the immovable exception.
 *                      The guard isn't delivered if it isn't the current space.
 * @param name          The name of the port in @c space violating immovability.
 * @param port          The port violating immovability (must be pol_movable_send).
 */
__cold
extern void mach_port_guard_exception_immovable(
	ipc_space_t             space,
	mach_port_name_t        name,
	mach_port_t             port,
	mach_msg_type_name_t    disp,
	ipc_entry_t             entry);

/*!
 * @brief
 * Deliver a soft or hard mod_refs guard exception.
 *
 * @param space         The space causing the pinned exception.
 *                      The guard isn't delivered if it isn't the current space,
 *                      or the task disables guards on pinned violations.
 * @param name          The name of the entry in @c space violating pinned rules.
 * @param payload       A valid @c MPG_FLAGS_MOD_REFS_PINNED_* value.
 */
__cold
extern void mach_port_guard_exception_pinned(
	ipc_space_t             space,
	mach_port_name_t        name,
	uint64_t                payload);

#pragma mark exception port policy

/*!
 * @brief
 * Check whether the port can be a valid exception port for a given task.
 *
 * @param task          The task registering an exception port.
 * @param port          The port being registered as exception port.
 */
extern bool ipc_is_valid_exception_port(
	task_t task,
	ipc_port_t port);

#pragma mark notification policies

/*!
 * @brief
 * Check if requesting a port destroyed notification on pd_port is allowed.
 *
 * @discussion
 * pd_port is locked and active.
 * This function must raise a guard exception along every error path
 *
 * @param pd_port		the port to be reaped after destroy
 * @param notify_port	the notify port that pd_port will be sent to after deat
 *
 * @returns
 * - KERN_SUCCESS       port destroyed notification is allowed to be requested
 * on this pd_port with this notify_port
 * - KERN_FAILURE       pd_port already has a pd notification
 * - KERN_INVALID_RIGHT some violation in the security policy
 */
extern kern_return_t ipc_allow_register_pd_notification(
	ipc_port_t              pd_port,
	ipc_port_t              notify_port);

#pragma GCC visibility pop
__ASSUME_PTR_ABI_SINGLE_END __END_DECLS

#endif  /* _IPC_IPC_POLICY_H_ */
