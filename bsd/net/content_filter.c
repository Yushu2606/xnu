/*
 * Copyright (c) 2013-2022, 2024, 2025 Apple Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_LICENSE_HEADER_END@
 */

/*
 * THEORY OF OPERATION
 *
 * The socket content filter subsystem provides a way for user space agents to
 * make filtering decisions based on the content of the data being sent and
 * received by INET/INET6 sockets.
 *
 * A content filter user space agents gets a copy of the data and the data is
 * also kept in kernel buffer until the user space agents makes a pass or drop
 * decision. This unidirectional flow of content avoids unnecessary data copies
 * back to the kernel.
 *
 * A user space filter agent opens a kernel control socket with the name
 * CONTENT_FILTER_CONTROL_NAME to attach to the socket content filter subsystem.
 * When connected, a "struct content_filter" is created and set as the
 * "unitinfo" of the corresponding kernel control socket instance.
 *
 * The socket content filter subsystem exchanges messages with the user space
 * filter agent until an ultimate pass or drop decision is made by the
 * user space filter agent.
 *
 * It should be noted that messages about many INET/INET6 sockets can be multiplexed
 * over a single kernel control socket.
 *
 * Notes:
 * - The current implementation supports all INET/INET6 sockets (i.e. TCP,
 *   UDP, ICMP, etc).
 * - The current implementation supports up to two simultaneous content filters
 *   for iOS devices and eight simultaneous content filters for OSX.
 *
 *
 * NECP FILTER CONTROL UNIT
 *
 * A user space filter agent uses the Network Extension Control Policy (NECP)
 * database to specify which INET/INET6 sockets need to be filtered. The NECP
 * criteria may be based on a variety of properties like user ID or proc UUID.
 *
 * The NECP "filter control unit" is used by the socket content filter subsystem
 * to deliver the relevant INET/INET6 content information to the appropriate
 * user space filter agent via its kernel control socket instance.
 * This works as follows:
 *
 * 1) The user space filter agent specifies an NECP filter control unit when
 *    in adds its filtering rules to the NECP database.
 *
 * 2) The user space filter agent also sets its NECP filter control unit on the
 *    content filter kernel control socket via the socket option
 *    CFIL_OPT_NECP_CONTROL_UNIT.
 *
 * 3) The NECP database is consulted to find out if a given INET/INET6 socket
 *    needs to be subjected to content filtering and returns the corresponding
 *    NECP filter control unit  -- the NECP filter control unit is actually
 *    stored in the INET/INET6 socket structure so the NECP lookup is really simple.
 *
 * 4) The NECP filter control unit is then used to find the corresponding
 *    kernel control socket instance.
 *
 * Note: NECP currently supports a single filter control unit per INET/INET6 socket
 *       but this restriction may be soon lifted.
 *
 *
 * THE MESSAGING PROTOCOL
 *
 * The socket content filter subsystem and a user space filter agent
 * communicate over the kernel control socket via an asynchronous
 * messaging protocol (this is not a request-response protocol).
 * The socket content filter subsystem sends event messages to the user
 * space filter agent about the INET/INET6 sockets it is interested to filter.
 * The user space filter agent sends action messages to either allow
 * data to pass or to disallow the data flow (and drop the connection).
 *
 * All messages over a content filter kernel control socket share the same
 * common header of type "struct cfil_msg_hdr". The message type tells if
 * it's a event message "CFM_TYPE_EVENT" or a action message "CFM_TYPE_ACTION".
 * The message header field "cfm_sock_id" identifies a given INET/INET6 flow.
 * For TCP, flows are per-socket.  For UDP and other datagrame protocols, there
 * could be multiple flows per socket.
 *
 * Note the message header length field may be padded for alignment and can
 * be larger than the actual content of the message.
 * The field "cfm_op" describe the kind of event or action.
 *
 * Here are the kinds of content filter events:
 * - CFM_OP_SOCKET_ATTACHED: a new INET/INET6 socket is being filtered
 * - CFM_OP_SOCKET_CLOSED: A INET/INET6 socket is closed
 * - CFM_OP_DATA_OUT: A span of data is being sent on a INET/INET6 socket
 * - CFM_OP_DATA_IN: A span of data is being or received on a INET/INET6 socket
 *
 *
 * EVENT MESSAGES
 *
 * The CFM_OP_DATA_OUT and CFM_OP_DATA_IN event messages contains a span of
 * data that is being sent or received. The position of this span of data
 * in the data flow is described by a set of start and end offsets. These
 * are absolute 64 bits offsets. The first byte sent (or received) starts
 * at offset 0 and ends at offset 1. The length of the content data
 * is given by the difference between the end offset and the start offset.
 *
 * After a CFM_OP_SOCKET_ATTACHED is delivered, CFM_OP_DATA_OUT and
 * CFM_OP_DATA_OUT events are not delivered until a CFM_OP_DATA_UPDATE
 * action message is sent by the user space filter agent.
 *
 * Note: absolute 64 bits offsets should be large enough for the foreseeable
 * future.  A 64-bits counter will wrap after 468 years at 10 Gbit/sec:
 *   2E64 / ((10E9 / 8) * 60 * 60 * 24 * 365.25) = 467.63
 *
 * They are two kinds of primary content filter actions:
 * - CFM_OP_DATA_UPDATE: to update pass or peek offsets for each direction.
 * - CFM_OP_DROP: to shutdown socket and disallow further data flow
 *
 * There is also an action to mark a given client flow as already filtered
 * at a higher level, CFM_OP_BLESS_CLIENT.
 *
 *
 * ACTION MESSAGES
 *
 * The CFM_OP_DATA_UPDATE action messages let the user space filter
 * agent allow data to flow up to the specified pass offset -- there
 * is a pass offset for outgoing data and a pass offset for incoming data.
 * When a new INET/INET6 socket is attached to the content filter and a flow is
 * created, each pass offset is initially set to 0 so no data is allowed to pass by
 * default.  When the pass offset is set to CFM_MAX_OFFSET via a CFM_OP_DATA_UPDATE
 * then the data flow becomes unrestricted.
 *
 * Note that pass offsets can only be incremented. A CFM_OP_DATA_UPDATE message
 * with a pass offset smaller than the pass offset of a previous
 * CFM_OP_DATA_UPDATE message is silently ignored.
 *
 * A user space filter agent also uses CFM_OP_DATA_UPDATE action messages
 * to tell the kernel how much data it wants to see by using the peek offsets.
 * Just like pass offsets, there is a peek offset for each direction.
 * When a new INET/INET6 flow is created, each peek offset is initially set to 0
 * so no CFM_OP_DATA_OUT and CFM_OP_DATA_IN event messages are dispatched by default
 * until a CFM_OP_DATA_UPDATE action message with a greater than 0 peek offset is sent
 * by the user space filter agent.  When the peek offset is set to CFM_MAX_OFFSET via
 * a CFM_OP_DATA_UPDATE then the flow of update data events becomes unrestricted.
 *
 * Note that peek offsets cannot be smaller than the corresponding pass offset.
 * Also a peek offsets cannot be smaller than the corresponding end offset
 * of the last CFM_OP_DATA_OUT/CFM_OP_DATA_IN message dispatched. Trying
 * to set a too small peek value is silently ignored.
 *
 *
 * PER FLOW "struct cfil_info"
 *
 * As soon as a INET/INET6 socket gets attached to a content filter, a
 * "struct cfil_info" is created to hold the content filtering state for this
 * socket.  For UDP and other datagram protocols, as soon as traffic is seen for
 * each new flow identified by its 4-tuple of source address/port and destination
 * address/port, a "struct cfil_info" is created.  Each datagram socket may
 * have multiple flows maintained in a hash table of "struct cfil_info" entries.
 *
 * The content filtering state is made of the following information
 * for each direction:
 * - The current pass offset;
 * - The first and last offsets of the data pending, waiting for a filtering
 *   decision;
 * - The inject queue for data that passed the filters and that needs
 *   to be re-injected;
 * - A content filter specific state in a set of  "struct cfil_entry"
 *
 *
 * CONTENT FILTER STATE "struct cfil_entry"
 *
 * The "struct cfil_entry" maintains the information most relevant to the
 * message handling over a kernel control socket with a user space filter agent.
 *
 * The "struct cfil_entry" holds the NECP filter control unit that corresponds
 * to the kernel control socket unit it corresponds to and also has a pointer
 * to the corresponding "struct content_filter".
 *
 * For each direction, "struct cfil_entry" maintains the following information:
 * - The pass offset
 * - The peek offset
 * - The offset of the last data peeked at by the filter
 * - A queue of data that's waiting to be delivered to the  user space filter
 *   agent on the kernel control socket
 * - A queue of data for which event messages have been sent on the kernel
 *   control socket and are pending for a filtering decision.
 *
 *
 * CONTENT FILTER QUEUES
 *
 * Data that is being filtered is steered away from the INET/INET6 socket buffer
 * and instead will sit in one of three content filter queues until the data
 * can be re-injected into the INET/INET6 socket buffer.
 *
 * A content filter queue is represented by "struct cfil_queue" that contains
 * a list of mbufs and the start and end offset of the data span of
 * the list of mbufs.
 *
 * The data moves into the three content filter queues according to this
 * sequence:
 * a) The "cfe_ctl_q" of "struct cfil_entry"
 * b) The "cfe_pending_q" of "struct cfil_entry"
 * c) The "cfi_inject_q" of "struct cfil_info"
 *
 * Note: The sequence (a),(b) may be repeated several times if there is more
 * than one content filter attached to the INET/INET6 socket.
 *
 * The "cfe_ctl_q" queue holds data than cannot be delivered to the
 * kernel conntrol socket for two reasons:
 * - The peek offset is less that the end offset of the mbuf data
 * - The kernel control socket is flow controlled
 *
 * The "cfe_pending_q" queue holds data for which CFM_OP_DATA_OUT or
 * CFM_OP_DATA_IN have been successfully dispatched to the kernel control
 * socket and are waiting for a pass action message fromn the user space
 * filter agent. An mbuf length must be fully allowed to pass to be removed
 * from the cfe_pending_q.
 *
 * The "cfi_inject_q" queue holds data that has been fully allowed to pass
 * by the user space filter agent and that needs to be re-injected into the
 * INET/INET6 socket.
 *
 *
 * IMPACT ON FLOW CONTROL
 *
 * An essential aspect of the content filer subsystem is to minimize the
 * impact on flow control of the INET/INET6 sockets being filtered.
 *
 * The processing overhead of the content filtering may have an effect on
 * flow control by adding noticeable delays and cannot be eliminated --
 * care must be taken by the user space filter agent to minimize the
 * processing delays.
 *
 * The amount of data being filtered is kept in buffers while waiting for
 * a decision by the user space filter agent. This amount of data pending
 * needs to be subtracted from the amount of data available in the
 * corresponding INET/INET6 socket buffer. This is done by modifying
 * sbspace() and tcp_sbspace() to account for amount of data pending
 * in the content filter.
 *
 *
 * LOCKING STRATEGY
 *
 * The global state of content filter subsystem is protected by a single
 * read-write lock "cfil_lck_rw". The data flow can be done with the
 * cfil read-write lock held as shared so it can be re-entered from multiple
 * threads.
 *
 * The per INET/INET6 socket content filterstate -- "struct cfil_info" -- is
 * protected by the socket lock.
 *
 * A INET/INET6 socket lock cannot be taken while the cfil read-write lock
 * is held. That's why we have some sequences where we drop the cfil read-write
 * lock before taking the INET/INET6 lock.
 *
 * It is also important to lock the INET/INET6 socket buffer while the content
 * filter is modifying the amount of pending data. Otherwise the calculations
 * in sbspace() and tcp_sbspace()  could be wrong.
 *
 * The "cfil_lck_rw" protects "struct content_filter" and also the fields
 * "cfe_link" and "cfe_filter" of "struct cfil_entry".
 *
 * Actually "cfe_link" and "cfe_filter" are protected by both by
 * "cfil_lck_rw" and the socket lock: they may be modified only when
 * "cfil_lck_rw" is exclusive and the socket is locked.
 *
 * To read the other fields of "struct content_filter" we have to take
 * "cfil_lck_rw" in shared mode.
 *
 * DATAGRAM SPECIFICS:
 *
 * The socket content filter supports all INET/INET6 protocols.  However
 * the treatments for TCP sockets and for datagram (UDP, ICMP, etc) sockets
 * are slightly different.
 *
 * Each datagram socket may have multiple flows.  Each flow is identified
 * by the flow's source address/port and destination address/port tuple
 * and is represented as a "struct cfil_info" entry.  For each socket,
 * a hash table is used to maintain the collection of flows under that socket.
 *
 * Each datagram flow is uniquely identified by it's "struct cfil_info" cfi_sock_id.
 * The highest 32-bits of the cfi_sock_id contains the socket's so_gencnt.  This portion
 * of the cfi_sock_id is used locate the socket during socket lookup.  The lowest 32-bits
 * of the cfi_sock_id contains a hash of the flow's 4-tuple.  This portion of the cfi_sock_id
 * is used as the hash value for the flow hash table lookup within the parent socket.
 *
 * Since datagram sockets may not be connected, flow states may not be maintained in the
 * socket structures and thus have to be saved for each packet.  These saved states will be
 * used for both outgoing and incoming reinjections.  For outgoing packets, destination
 * address/port as well as the current socket states will be saved.  During reinjection,
 * these saved states will be used instead.  For incoming packets, control and address
 * mbufs will be chained to the data.  During reinjection, the whole chain will be queued
 * onto the incoming socket buffer.
 *
 * LIMITATIONS
 *
 * - Support all INET/INET6 sockets, such as TCP, UDP, ICMP, etc
 *
 * - Does not support TCP unordered messages
 */

/*
 *	TO DO LIST
 *
 *	Deal with OOB
 *
 */

#include <sys/types.h>
#include <sys/kern_control.h>
#include <sys/queue.h>
#include <sys/domain.h>
#include <sys/protosw.h>
#include <sys/syslog.h>
#include <sys/systm.h>
#include <sys/param.h>
#include <sys/mbuf.h>

#include <kern/locks.h>
#include <kern/zalloc.h>
#include <kern/debug.h>

#include <net/ntstat.h>
#include <net/content_filter.h>
#include <net/content_filter_crypto.h>

#define _IP_VHL
#include <netinet/ip.h>
#include <netinet/in_pcb.h>
#include <netinet/tcp.h>
#include <netinet/tcp_var.h>
#include <netinet/udp.h>
#include <netinet/udp_var.h>
#include <netinet6/in6_pcb.h>
#include <kern/socket_flows.h>

#include <string.h>
#include <libkern/libkern.h>
#include <kern/sched_prim.h>
#include <kern/task.h>
#include <mach/task_info.h>

#include <net/sockaddr_utils.h>

#define MAX_CONTENT_FILTER 8

extern int tcp_msl;
extern struct inpcbinfo ripcbinfo;
struct cfil_entry;

/*
 * The structure content_filter represents a user space content filter
 * It's created and associated with a kernel control socket instance
 */
struct content_filter {
	kern_ctl_ref            cf_kcref;
	u_int32_t               cf_kcunit;
	u_int32_t               cf_flags;

	uint32_t                cf_necp_control_unit;

	uint32_t                cf_sock_count;
	TAILQ_HEAD(, cfil_entry) cf_sock_entries;

	cfil_crypto_state_t cf_crypto_state;
};

#define CFF_ACTIVE              0x01
#define CFF_DETACHING           0x02
#define CFF_FLOW_CONTROLLED     0x04
#define CFF_PRESERVE_CONNECTIONS 0x08

struct content_filter *content_filters[MAX_CONTENT_FILTER];
uint32_t cfil_active_count = 0; /* Number of active content filters */
uint32_t cfil_sock_attached_count = 0;  /* Number of sockets attachements */
uint32_t cfil_sock_attached_stats_count = 0;    /* Number of sockets requested periodic stats report */
uint32_t cfil_close_wait_timeout = 1000; /* in milliseconds */

static kern_ctl_ref cfil_kctlref = NULL;

static LCK_GRP_DECLARE(cfil_lck_grp, "content filter");
static LCK_RW_DECLARE(cfil_lck_rw, &cfil_lck_grp);

#define CFIL_RW_LCK_MAX 8

int cfil_rw_nxt_lck = 0;
void* cfil_rw_lock_history[CFIL_RW_LCK_MAX];

int cfil_rw_nxt_unlck = 0;
void* cfil_rw_unlock_history[CFIL_RW_LCK_MAX];

static KALLOC_TYPE_DEFINE(content_filter_zone, struct content_filter, NET_KT_DEFAULT);

MBUFQ_HEAD(cfil_mqhead);

struct cfil_queue {
	uint64_t                q_start; /* offset of first byte in queue */
	uint64_t                q_end; /* offset of last byte in queue */
	struct cfil_mqhead      q_mq;
};

/*
 * struct cfil_entry
 *
 * The is one entry per content filter
 */
struct cfil_entry {
	TAILQ_ENTRY(cfil_entry) cfe_link;
	SLIST_ENTRY(cfil_entry) cfe_order_link;
	struct content_filter   *cfe_filter;

	struct cfil_info        *cfe_cfil_info;
	uint32_t                cfe_flags;
	uint32_t                cfe_necp_control_unit;
	struct timeval          cfe_last_event; /* To user space */
	struct timeval          cfe_last_action; /* From user space */
	uint64_t                cfe_byte_inbound_count_reported; /* stats already been reported */
	uint64_t                cfe_byte_outbound_count_reported; /* stats already been reported */
	struct timeval          cfe_stats_report_ts; /* Timestamp for last stats report */
	uint32_t                cfe_stats_report_frequency; /* Interval for stats report in msecs */
	boolean_t               cfe_laddr_sent;

	struct cfe_buf {
		/*
		 * cfe_pending_q holds data that has been delivered to
		 * the filter and for which we are waiting for an action
		 */
		struct cfil_queue       cfe_pending_q;
		/*
		 * This queue is for data that has not be delivered to
		 * the content filter (new data, pass peek or flow control)
		 */
		struct cfil_queue       cfe_ctl_q;

		uint64_t                cfe_pass_offset;
		uint64_t                cfe_peek_offset;
		uint64_t                cfe_peeked;
	} cfe_snd, cfe_rcv;
};

#define CFEF_CFIL_ATTACHED              0x0001  /* was attached to filter */
#define CFEF_SENT_SOCK_ATTACHED         0x0002  /* sock attach event was sent */
#define CFEF_DATA_START                 0x0004  /* can send data event */
#define CFEF_FLOW_CONTROLLED            0x0008  /* wait for flow control lift */
#define CFEF_SENT_DISCONNECT_IN         0x0010  /* event was sent */
#define CFEF_SENT_DISCONNECT_OUT        0x0020  /* event was sent */
#define CFEF_SENT_SOCK_CLOSED           0x0040  /* closed event was sent */
#define CFEF_CFIL_DETACHED              0x0080  /* filter was detached */


#define CFI_ADD_TIME_LOG(cfil, t1, t0, op)                                                                                      \
	        struct timeval64 _tdiff;                                                                                          \
	        size_t offset = (cfil)->cfi_op_list_ctr;                                                                        \
	        if (offset < CFI_MAX_TIME_LOG_ENTRY) {                                                                          \
	                timersub(t1, t0, &_tdiff);                                                                              \
	                (cfil)->cfi_op_time[offset] = (uint32_t)(_tdiff.tv_sec * 1000 + _tdiff.tv_usec / 1000);                 \
	                (cfil)->cfi_op_list[offset] = (unsigned char)op;                                                        \
	                (cfil)->cfi_op_list_ctr ++;                                                                             \
	        }

/*
 * struct cfil_info
 *
 * There is a struct cfil_info per socket
 */
struct cfil_info {
	TAILQ_ENTRY(cfil_info)  cfi_link;
	TAILQ_ENTRY(cfil_info)  cfi_link_stats;
	struct socket           *cfi_so;
	uint64_t                cfi_flags;
	uint64_t                cfi_sock_id;
	struct timeval64        cfi_first_event;
	uint32_t                cfi_op_list_ctr;
	uint32_t                cfi_op_time[CFI_MAX_TIME_LOG_ENTRY];    /* time interval in microseconds since first event */
	unsigned char           cfi_op_list[CFI_MAX_TIME_LOG_ENTRY];
	union sockaddr_in_4_6   cfi_so_attach_faddr;                    /* faddr at the time of attach */
	union sockaddr_in_4_6   cfi_so_attach_laddr;                    /* laddr at the time of attach */

	int                     cfi_dir;
	uint64_t                cfi_byte_inbound_count;
	uint64_t                cfi_byte_outbound_count;
	struct timeval          cfi_timestamp;

	boolean_t               cfi_isSignatureLatest;                  /* Indicates if signature covers latest flow attributes */
	u_int32_t               cfi_filter_control_unit;
	u_int32_t               cfi_filter_policy_gencount;
	u_int32_t               cfi_debug;
	struct cfi_buf {
		/*
		 * cfi_pending_first and cfi_pending_last describe the total
		 * amount of data outstanding for all the filters on
		 * this socket and data in the flow queue
		 * cfi_pending_mbcnt counts in sballoc() "chars of mbufs used"
		 */
		uint64_t                cfi_pending_first;
		uint64_t                cfi_pending_last;
		uint32_t                cfi_pending_mbcnt;
		uint32_t                cfi_pending_mbnum;
		uint32_t                cfi_tail_drop_cnt;
		/*
		 * cfi_pass_offset is the minimum of all the filters
		 */
		uint64_t                cfi_pass_offset;
		/*
		 * cfi_inject_q holds data that needs to be re-injected
		 * into the socket after filtering and that can
		 * be queued because of flow control
		 */
		struct cfil_queue       cfi_inject_q;
	} cfi_snd, cfi_rcv;

	struct cfil_entry       cfi_entries[MAX_CONTENT_FILTER];
	struct soflow_hash_entry *cfi_hash_entry;
	SLIST_HEAD(, cfil_entry) cfi_ordered_entries;
	os_refcnt_t             cfi_ref_count;
} __attribute__((aligned(8)));

#define CFIF_DROP                       0x0001  /* drop action applied */
#define CFIF_CLOSE_WAIT                 0x0002  /* waiting for filter to close */
#define CFIF_SOCK_CLOSED                0x0004  /* socket is closed */
#define CFIF_RETRY_INJECT_IN            0x0010  /* inject in failed */
#define CFIF_RETRY_INJECT_OUT           0x0020  /* inject out failed */
#define CFIF_SHUT_WR                    0x0040  /* shutdown write */
#define CFIF_SHUT_RD                    0x0080  /* shutdown read */
#define CFIF_SOCKET_CONNECTED           0x0100  /* socket is connected */
#define CFIF_INITIAL_VERDICT            0x0200  /* received initial verdict */
#define CFIF_NO_CLOSE_WAIT              0x0400  /* do not wait to close */
#define CFIF_SO_DELAYED_DEAD            0x0800  /* Delayed socket DEAD marking */
#define CFIF_SO_DELAYED_TCP_TIME_WAIT   0x1000  /* Delayed TCP FIN TIME WAIT */

#define CFI_MASK_GENCNT         0xFFFFFFFF00000000      /* upper 32 bits */
#define CFI_SHIFT_GENCNT        32
#define CFI_MASK_FLOWHASH       0x00000000FFFFFFFF      /* lower 32 bits */
#define CFI_SHIFT_FLOWHASH      0

#define CFI_ENTRY_KCUNIT(i, e) ((uint32_t)(((e) - &((i)->cfi_entries[0])) + 1))

static KALLOC_TYPE_DEFINE(cfil_info_zone, struct cfil_info, NET_KT_DEFAULT);

TAILQ_HEAD(cfil_sock_head, cfil_info) cfil_sock_head;
TAILQ_HEAD(cfil_sock_head_stats, cfil_info) cfil_sock_head_stats;

#define CFIL_QUEUE_VERIFY(x) if (cfil_debug) cfil_queue_verify(x)
#define CFIL_INFO_VERIFY(x) if (cfil_debug) cfil_info_verify(x)

/*
 * UDP Socket Support
 */
#define IS_ICMP(so) (so && (SOCK_CHECK_TYPE(so, SOCK_RAW) || SOCK_CHECK_TYPE(so, SOCK_DGRAM)) && \
	                                   (SOCK_CHECK_PROTO(so, IPPROTO_ICMP) || SOCK_CHECK_PROTO(so, IPPROTO_ICMPV6)))
#define IS_RAW(so)  (so && SOCK_CHECK_TYPE(so, SOCK_RAW) && SOCK_CHECK_PROTO(so, IPPROTO_RAW))

#define OPTIONAL_IP_HEADER(so) (!IS_TCP(so) && !IS_UDP(so))
#define GET_SO_PROTOCOL(so) (so ? SOCK_PROTO(so) : IPPROTO_IP)
#define GET_SO_INP_PROTOCOL(so) ((so && sotoinpcb(so)) ? sotoinpcb(so)->inp_ip_p : IPPROTO_IP)
#define GET_SO_PROTO(so) ((GET_SO_PROTOCOL(so) != IPPROTO_IP) ? GET_SO_PROTOCOL(so) : GET_SO_INP_PROTOCOL(so))
#define IS_INP_V6(inp) (inp && (inp->inp_vflag & INP_IPV6))

#define UNCONNECTED(inp) (inp && (((inp->inp_vflag & INP_IPV4) && (inp->inp_faddr.s_addr == INADDR_ANY)) || \
	                                                          ((inp->inp_vflag & INP_IPV6) && IN6_IS_ADDR_UNSPECIFIED(&inp->in6p_faddr))))
#define IS_ENTRY_ATTACHED(cfil_info, kcunit) (cfil_info != NULL && (kcunit <= MAX_CONTENT_FILTER) && \
	                                                                                  cfil_info->cfi_entries[kcunit - 1].cfe_filter != NULL)
#define IS_DNS(local, remote) (check_port(local, 53) || check_port(remote, 53) || check_port(local, 5353) || check_port(remote, 5353))
#define IS_INITIAL_TFO_DATA(so) (so && (so->so_flags1 & SOF1_PRECONNECT_DATA) && (so->so_state & SS_ISCONNECTING))
#define NULLADDRESS(addr) ((addr.sa.sa_len == 0) || \
	                   (addr.sa.sa_family == AF_INET && addr.sin.sin_addr.s_addr == 0) || \
	                   (addr.sa.sa_family == AF_INET6 && IN6_IS_ADDR_UNSPECIFIED(&addr.sin6.sin6_addr)))

#define SKIP_FILTER_FOR_TCP_SOCKET(so) \
    (so == NULL || \
     (!SOCK_CHECK_DOM(so, PF_INET) && !SOCK_CHECK_DOM(so, PF_INET6)) || \
      !SOCK_CHECK_TYPE(so, SOCK_STREAM) || \
      !SOCK_CHECK_PROTO(so, IPPROTO_TCP) || \
      (so->so_flags & SOF_MP_SUBFLOW) != 0 || \
      (so->so_flags1 & SOF1_CONTENT_FILTER_SKIP) != 0)

/*
 * Special handling for 0.0.0.0-faddr TCP flows.  This flows will be changed to loopback addr by TCP and
 * may result in an immediate TCP RESET and socket close.  This leads to CFIL blocking the owner thread for
 * 1 sec waiting for ack from user-space provider (ack recevied by CFIL but socket already removed from
 * global socket list).  To avoid this, identify these flows and do not perform the close-wait blocking.
 * These flows are identified as destined to Loopback address and were disconnected shortly after connect
 * (before initial-verdict received).
 */
#define IS_LOOPBACK_FADDR(inp) \
    (inp && ((IS_INP_V6(inp) && IN6_IS_ADDR_LOOPBACK(&inp->in6p_faddr)) || (ntohl(inp->inp_faddr.s_addr) == INADDR_LOOPBACK)))

#define SET_NO_CLOSE_WAIT(inp, cfil_info) \
    if (inp && cfil_info && !(cfil_info->cfi_flags & CFIF_INITIAL_VERDICT) && IS_LOOPBACK_FADDR(inp)) { \
	cfil_info->cfi_flags |= CFIF_NO_CLOSE_WAIT; \
    }

#define IS_NO_CLOSE_WAIT(cfil_info) (cfil_info && (cfil_info->cfi_flags & CFIF_NO_CLOSE_WAIT))

os_refgrp_decl(static, cfil_refgrp, "CFILRefGroup", NULL);

#define CFIL_INFO_FREE(cfil_info) \
    if (cfil_info && (os_ref_release(&cfil_info->cfi_ref_count) == 0)) { \
	cfil_info_free(cfil_info); \
    }

#define SOCKET_PID(so) ((so->so_flags & SOF_DELEGATED) ? so->e_pid : so->last_pid)
#define MATCH_PID(so) (so && (cfil_log_pid == SOCKET_PID(so)))
#define MATCH_PORT(inp, local, remote) \
    ((inp && ntohs(inp->inp_lport) == cfil_log_port) || (inp && ntohs(inp->inp_fport) == cfil_log_port) || \
	check_port(local, cfil_log_port) || check_port(remote, cfil_log_port))
#define MATCH_PROTO(so) (GET_SO_PROTO(so) == cfil_log_proto)

#define DEBUG_FLOW(inp, so, local, remote) \
    ((cfil_log_port && MATCH_PORT(inp, local, remote)) || (cfil_log_pid && MATCH_PID(so)) || (cfil_log_proto && MATCH_PROTO(so)))

#define SO_DELAYED_DEAD_SET(so, set) do {                               \
	if (so->so_cfil) {                                              \
	        if (set) {                                              \
	                so->so_cfil->cfi_flags |= CFIF_SO_DELAYED_DEAD; \
	        } else {                                                \
	                so->so_cfil->cfi_flags &= ~CFIF_SO_DELAYED_DEAD; \
	        }                                                       \
	} else if (so->so_flow_db) {                                    \
	        if (set) {                                              \
	                so->so_flow_db->soflow_db_flags |= SOFLOWF_SO_DELAYED_DEAD; \
	        } else {                                                \
	                so->so_flow_db->soflow_db_flags &= ~SOFLOWF_SO_DELAYED_DEAD; \
	        }                                                       \
	}                                                               \
} while (0)

#define SO_DELAYED_DEAD_GET(so) \
    (so->so_cfil ? (so->so_cfil->cfi_flags & CFIF_SO_DELAYED_DEAD) : \
	            (so->so_flow_db) ? (so->so_flow_db->soflow_db_flags & SOFLOWF_SO_DELAYED_DEAD) : false)

#define SO_DELAYED_TCP_TIME_WAIT_SET(so, set) do {                      \
	if (so->so_cfil) {                                              \
	        if (set) {                                              \
	                so->so_cfil->cfi_flags |= CFIF_SO_DELAYED_TCP_TIME_WAIT; \
	        } else {                                                \
	                so->so_cfil->cfi_flags &= ~CFIF_SO_DELAYED_TCP_TIME_WAIT; \
	        }                                                       \
	}                                                               \
} while (0)

#define SO_DELAYED_TCP_TIME_WAIT_GET(so) \
    (so->so_cfil ? (so->so_cfil->cfi_flags & CFIF_SO_DELAYED_TCP_TIME_WAIT) : false)

/*
 * Periodic Statistics Report:
 */
static struct thread *cfil_stats_report_thread;
#define CFIL_STATS_REPORT_INTERVAL_MIN_MSEC  500   // Highest report frequency
#define CFIL_STATS_REPORT_RUN_INTERVAL_NSEC  (CFIL_STATS_REPORT_INTERVAL_MIN_MSEC * NSEC_PER_MSEC)
#define CFIL_STATS_REPORT_MAX_COUNT          50    // Max stats to be reported per run

/* This buffer must have same layout as struct cfil_msg_stats_report */
struct cfil_stats_report_buffer {
	struct cfil_msg_hdr        msghdr;
	uint32_t                   count;
	struct cfil_msg_sock_stats stats[CFIL_STATS_REPORT_MAX_COUNT];
};
static struct cfil_stats_report_buffer *global_cfil_stats_report_buffers[MAX_CONTENT_FILTER];
static uint32_t global_cfil_stats_counts[MAX_CONTENT_FILTER];

/*
 * UDP Garbage Collection:
 */
#define UDP_FLOW_GC_ACTION_TO        10  // Flow Action Timeout (no action from user space) in seconds
#define UDP_FLOW_GC_MAX_COUNT        100 // Max UDP flows to be handled per run

/*
 * UDP flow queue thresholds
 */
#define UDP_FLOW_GC_MBUF_CNT_MAX  (2 << MBSHIFT) // Max mbuf byte count in flow queue (2MB)
#define UDP_FLOW_GC_MBUF_NUM_MAX  (UDP_FLOW_GC_MBUF_CNT_MAX >> MCLSHIFT) // Max mbuf count in flow queue (1K)
#define UDP_FLOW_GC_MBUF_SHIFT    5             // Shift to get 1/32 of platform limits
/*
 * UDP flow queue threshold globals:
 */
static unsigned int cfil_udp_gc_mbuf_num_max = UDP_FLOW_GC_MBUF_NUM_MAX;
static unsigned int cfil_udp_gc_mbuf_cnt_max = UDP_FLOW_GC_MBUF_CNT_MAX;

/*
 * CFIL specific mbuf tag:
 * Save state of socket at the point of data entry into cfil.
 * Use saved state for reinjection at protocol layer.
 */
struct cfil_tag {
	union sockaddr_in_4_6 cfil_faddr;
	uint32_t cfil_so_state_change_cnt;
	uint32_t cfil_so_options;
	int cfil_inp_flags;
};

/*
 * Global behavior flags:
 */
#define CFIL_BEHAVIOR_FLAG_PRESERVE_CONNECTIONS 0x00000001
static uint32_t cfil_behavior_flags = 0;

#define DO_PRESERVE_CONNECTIONS (cfil_behavior_flags & CFIL_BEHAVIOR_FLAG_PRESERVE_CONNECTIONS)

/*
 * Statistics
 */

struct cfil_stats cfil_stats;

/*
 * For troubleshooting
 */
int cfil_log_level = LOG_ERR;
int cfil_log_port = 0;
int cfil_log_pid = 0;
int cfil_log_proto = 0;
int cfil_log_data = 0;
int cfil_log_stats = 0;
int cfil_debug = 1;

/*
 * Sysctls for logs and statistics
 */
static int sysctl_cfil_filter_list(struct sysctl_oid *, void *, int,
    struct sysctl_req *);
static int sysctl_cfil_sock_list(struct sysctl_oid *, void *, int,
    struct sysctl_req *);

SYSCTL_NODE(_net, OID_AUTO, cfil, CTLFLAG_RW | CTLFLAG_LOCKED, 0, "cfil");

SYSCTL_INT(_net_cfil, OID_AUTO, log, CTLFLAG_RW | CTLFLAG_LOCKED,
    &cfil_log_level, 0, "");

SYSCTL_INT(_net_cfil, OID_AUTO, log_port, CTLFLAG_RW | CTLFLAG_LOCKED,
    &cfil_log_port, 0, "");

SYSCTL_INT(_net_cfil, OID_AUTO, log_pid, CTLFLAG_RW | CTLFLAG_LOCKED,
    &cfil_log_pid, 0, "");

SYSCTL_INT(_net_cfil, OID_AUTO, log_proto, CTLFLAG_RW | CTLFLAG_LOCKED,
    &cfil_log_proto, 0, "");

SYSCTL_INT(_net_cfil, OID_AUTO, log_data, CTLFLAG_RW | CTLFLAG_LOCKED,
    &cfil_log_data, 0, "");

SYSCTL_INT(_net_cfil, OID_AUTO, log_stats, CTLFLAG_RW | CTLFLAG_LOCKED,
    &cfil_log_stats, 0, "");

SYSCTL_INT(_net_cfil, OID_AUTO, debug, CTLFLAG_RW | CTLFLAG_LOCKED,
    &cfil_debug, 0, "");

SYSCTL_UINT(_net_cfil, OID_AUTO, sock_attached_count, CTLFLAG_RD | CTLFLAG_LOCKED,
    &cfil_sock_attached_count, 0, "");

SYSCTL_UINT(_net_cfil, OID_AUTO, active_count, CTLFLAG_RD | CTLFLAG_LOCKED,
    &cfil_active_count, 0, "");

SYSCTL_UINT(_net_cfil, OID_AUTO, close_wait_timeout, CTLFLAG_RW | CTLFLAG_LOCKED,
    &cfil_close_wait_timeout, 0, "");

SYSCTL_UINT(_net_cfil, OID_AUTO, behavior_flags, CTLFLAG_RW | CTLFLAG_LOCKED,
    &cfil_behavior_flags, 0, "");

static int cfil_sbtrim = 1;
SYSCTL_UINT(_net_cfil, OID_AUTO, sbtrim, CTLFLAG_RW | CTLFLAG_LOCKED,
    &cfil_sbtrim, 0, "");

SYSCTL_PROC(_net_cfil, OID_AUTO, filter_list, CTLFLAG_RD | CTLFLAG_LOCKED,
    0, 0, sysctl_cfil_filter_list, "S,cfil_filter_stat", "");

SYSCTL_PROC(_net_cfil, OID_AUTO, sock_list, CTLFLAG_RD | CTLFLAG_LOCKED,
    0, 0, sysctl_cfil_sock_list, "S,cfil_sock_stat", "");

SYSCTL_STRUCT(_net_cfil, OID_AUTO, stats, CTLFLAG_RD | CTLFLAG_LOCKED,
    &cfil_stats, cfil_stats, "");

/*
 * Forward declaration to appease the compiler
 */
static int cfil_action_data_pass(struct socket *, struct cfil_info *, uint32_t, int,
    uint64_t, uint64_t);
static int cfil_action_drop(struct socket *, struct cfil_info *, uint32_t);
static int cfil_action_bless_client(uint32_t, struct cfil_msg_hdr *);
static int cfil_action_set_crypto_key(uint32_t, struct cfil_msg_hdr *);
static int cfil_dispatch_closed_event(struct socket *, struct cfil_info *, int);
static int cfil_data_common(struct socket *, struct cfil_info *, int, struct sockaddr *,
    struct mbuf *, struct mbuf *, uint32_t);
static int cfil_data_filter(struct socket *, struct cfil_info *, uint32_t, int,
    struct mbuf *, uint32_t);
static void fill_ip_sockaddr_4_6(union sockaddr_in_4_6 *,
    struct in_addr, u_int16_t);
static void fill_ip6_sockaddr_4_6(union sockaddr_in_4_6 *,
    struct in6_addr *, u_int16_t, uint32_t);

static int cfil_dispatch_attach_event(struct socket *, struct cfil_info *, uint32_t, int);
static void cfil_info_free(struct cfil_info *);
static struct cfil_info * cfil_info_alloc(struct socket *, struct soflow_hash_entry *);
static int cfil_info_attach_unit(struct socket *, uint32_t, struct cfil_info *);
static struct socket * cfil_socket_from_sock_id(cfil_sock_id_t, bool);
static struct socket * cfil_socket_from_client_uuid(uuid_t, bool *);
static int cfil_service_pending_queue(struct socket *, struct cfil_info *, uint32_t, int);
static int cfil_data_service_ctl_q(struct socket *, struct cfil_info *, uint32_t, int);
static void cfil_info_verify(struct cfil_info *);
static int cfil_update_data_offsets(struct socket *, struct cfil_info *, uint32_t, int,
    uint64_t, uint64_t);
static int cfil_acquire_sockbuf(struct socket *, struct cfil_info *, int);
static void cfil_release_sockbuf(struct socket *, int);
static int cfil_filters_attached(struct socket *);

static void cfil_rw_lock_exclusive(lck_rw_t *);
static void cfil_rw_unlock_exclusive(lck_rw_t *);
static void cfil_rw_lock_shared(lck_rw_t *);
static void cfil_rw_unlock_shared(lck_rw_t *);
static boolean_t cfil_rw_lock_shared_to_exclusive(lck_rw_t *);
static void cfil_rw_lock_exclusive_to_shared(lck_rw_t *);

static unsigned int cfil_data_length(struct mbuf *, int *, int *);
static struct cfil_info *cfil_sock_udp_get_info(struct socket *, uint32_t, bool, struct soflow_hash_entry *, struct sockaddr *, struct sockaddr *);
static errno_t cfil_sock_udp_handle_data(bool, struct socket *, struct sockaddr *, struct sockaddr *,
    struct mbuf *, struct mbuf *, uint32_t, struct soflow_hash_entry *);
static int32_t cfil_sock_udp_data_pending(struct sockbuf *, bool);
static void cfil_sock_udp_is_closed(struct socket *);
static int cfil_sock_udp_notify_shutdown(struct socket *, int, int, int);
static int cfil_sock_udp_shutdown(struct socket *, int *);
static void cfil_sock_udp_close_wait(struct socket *);
static void cfil_sock_udp_buf_update(struct sockbuf *);
static int cfil_filters_udp_attached(struct socket *, bool);
static void cfil_get_flow_address_v6(struct soflow_hash_entry *, struct inpcb *,
    struct in6_addr **, struct in6_addr **,
    u_int16_t *, u_int16_t *);
static void cfil_get_flow_address(struct soflow_hash_entry *, struct inpcb *,
    struct in_addr *, struct in_addr *,
    u_int16_t *, u_int16_t *);
static void cfil_info_log(int, struct cfil_info *, const char *);
void cfil_filter_show(u_int32_t);
void cfil_info_show(void);
bool cfil_info_action_timed_out(struct cfil_info *, int);
bool cfil_info_buffer_threshold_exceeded(struct cfil_info *);
struct m_tag *cfil_dgram_save_socket_state(struct cfil_info *, struct mbuf *);
boolean_t cfil_dgram_peek_socket_state(struct mbuf *m, int *inp_flags);
static void cfil_sock_received_verdict(struct socket *so);
static void cfil_fill_event_msg_addresses(struct soflow_hash_entry *, struct inpcb *,
    union sockaddr_in_4_6 *, union sockaddr_in_4_6 *,
    boolean_t, boolean_t);
static void cfil_stats_report_thread_func(void *, wait_result_t);
static void cfil_stats_report(void *v, wait_result_t w);
static bool cfil_dgram_gc_needed(struct socket *, struct soflow_hash_entry *, u_int64_t);
static bool cfil_dgram_gc_perform(struct socket *, struct soflow_hash_entry *);
static bool cfil_dgram_detach_entry(struct socket *, struct soflow_hash_entry *);
static bool cfil_dgram_detach_db(struct socket *, struct soflow_db *);
bool check_port(struct sockaddr *, u_short);
__printflike(3, 4)
static void cfil_log_printf(struct socket *so, struct cfil_info *info, const char *format, ...);

/*
 * Content filter global read write lock
 */

static void
cfil_rw_lock_exclusive(lck_rw_t *lck)
{
	void * __single lr_saved;

	lr_saved = __unsafe_forge_single(void *, __builtin_return_address(0));

	lck_rw_lock_exclusive(lck);

	cfil_rw_lock_history[cfil_rw_nxt_lck] = lr_saved;
	cfil_rw_nxt_lck = (cfil_rw_nxt_lck + 1) % CFIL_RW_LCK_MAX;
}

static void
cfil_rw_unlock_exclusive(lck_rw_t *lck)
{
	void * __single lr_saved;

	lr_saved = __unsafe_forge_single(void *, __builtin_return_address(0));

	lck_rw_unlock_exclusive(lck);

	cfil_rw_unlock_history[cfil_rw_nxt_unlck] = lr_saved;
	cfil_rw_nxt_unlck = (cfil_rw_nxt_unlck + 1) % CFIL_RW_LCK_MAX;
}

static void
cfil_rw_lock_shared(lck_rw_t *lck)
{
	void * __single lr_saved;

	lr_saved = __unsafe_forge_single(void *, __builtin_return_address(0));

	lck_rw_lock_shared(lck);

	cfil_rw_lock_history[cfil_rw_nxt_lck] = lr_saved;
	cfil_rw_nxt_lck = (cfil_rw_nxt_lck + 1) % CFIL_RW_LCK_MAX;
}

static void
cfil_rw_unlock_shared(lck_rw_t *lck)
{
	void * __single lr_saved;

	lr_saved = __unsafe_forge_single(void *, __builtin_return_address(0));

	lck_rw_unlock_shared(lck);

	cfil_rw_unlock_history[cfil_rw_nxt_unlck] = lr_saved;
	cfil_rw_nxt_unlck = (cfil_rw_nxt_unlck + 1) % CFIL_RW_LCK_MAX;
}

static boolean_t
cfil_rw_lock_shared_to_exclusive(lck_rw_t *lck)
{
	boolean_t upgraded;
	void * __single lr_saved;

	lr_saved = __unsafe_forge_single(void *, __builtin_return_address(0));

	upgraded = lck_rw_lock_shared_to_exclusive(lck);
	if (upgraded) {
		cfil_rw_unlock_history[cfil_rw_nxt_unlck] = lr_saved;
		cfil_rw_nxt_unlck = (cfil_rw_nxt_unlck + 1) % CFIL_RW_LCK_MAX;
	}
	return upgraded;
}

static void
cfil_rw_lock_exclusive_to_shared(lck_rw_t *lck)
{
	void * __single lr_saved;

	lr_saved = __unsafe_forge_single(void *, __builtin_return_address(0));

	lck_rw_lock_exclusive_to_shared(lck);

	cfil_rw_lock_history[cfil_rw_nxt_lck] = lr_saved;
	cfil_rw_nxt_lck = (cfil_rw_nxt_lck + 1) % CFIL_RW_LCK_MAX;
}

static void
cfil_rw_lock_assert_held(lck_rw_t *lck, int exclusive)
{
#if !MACH_ASSERT
#pragma unused(lck, exclusive)
#endif
	LCK_RW_ASSERT(lck,
	    exclusive ? LCK_RW_ASSERT_EXCLUSIVE : LCK_RW_ASSERT_HELD);
}

/*
 * Return the number of bytes in the mbuf chain using the same
 * method as m_length() or sballoc()
 *
 * Returns data len - starting from PKT start
 * - retmbcnt - optional param to get total mbuf bytes in chain
 * - retmbnum - optional param to get number of mbufs in chain
 */
static unsigned int
cfil_data_length(struct mbuf *m, int *retmbcnt, int *retmbnum)
{
	struct mbuf *m0;
	unsigned int pktlen = 0;
	int mbcnt;
	int mbnum;

	// Locate M_PKTHDR and mark as start of data if present
	for (m0 = m; m0 != NULL; m0 = m0->m_next) {
		if (m0->m_flags & M_PKTHDR) {
			m = m0;
			break;
		}
	}

	if (retmbcnt == NULL && retmbnum == NULL) {
		return m_length(m);
	}

	pktlen = 0;
	mbcnt = 0;
	mbnum = 0;
	for (m0 = m; m0 != NULL; m0 = m0->m_next) {
		pktlen += m0->m_len;
		mbnum++;
		mbcnt += _MSIZE;
		if (m0->m_flags & M_EXT) {
			mbcnt += m0->m_ext.ext_size;
		}
	}
	if (retmbcnt) {
		*retmbcnt = mbcnt;
	}
	if (retmbnum) {
		*retmbnum = mbnum;
	}
	return pktlen;
}

static struct mbuf *
cfil_data_start(struct mbuf *m)
{
	struct mbuf *m0;

	// Locate M_PKTHDR and use it as start of data if present
	for (m0 = m; m0 != NULL; m0 = m0->m_next) {
		if (m0->m_flags & M_PKTHDR) {
			return m0;
		}
	}
	return m;
}

/*
 * Common mbuf queue utilities
 */

static inline void
cfil_queue_init(struct cfil_queue *cfq)
{
	cfq->q_start = 0;
	cfq->q_end = 0;
	MBUFQ_INIT(&cfq->q_mq);
}

static inline uint64_t
cfil_queue_drain(struct cfil_queue *cfq)
{
	uint64_t drained = cfq->q_start - cfq->q_end;
	cfq->q_start = 0;
	cfq->q_end = 0;
	MBUFQ_DRAIN(&cfq->q_mq);

	return drained;
}

/* Return 1 when empty, 0 otherwise */
static inline int
cfil_queue_empty(struct cfil_queue *cfq)
{
	return MBUFQ_EMPTY(&cfq->q_mq);
}

static inline uint64_t
cfil_queue_offset_first(struct cfil_queue *cfq)
{
	return cfq->q_start;
}

static inline uint64_t
cfil_queue_offset_last(struct cfil_queue *cfq)
{
	return cfq->q_end;
}

static inline uint64_t
cfil_queue_len(struct cfil_queue *cfq)
{
	return cfq->q_end - cfq->q_start;
}

/*
 * Routines to verify some fundamental assumptions
 */

static void
cfil_queue_verify(struct cfil_queue *cfq)
{
	mbuf_t chain;
	mbuf_t m;
	mbuf_t n;
	uint64_t queuesize = 0;

	/* Verify offset are ordered */
	VERIFY(cfq->q_start <= cfq->q_end);

	/*
	 * When queue is empty, the offsets are equal otherwise the offsets
	 * are different
	 */
	VERIFY((MBUFQ_EMPTY(&cfq->q_mq) && cfq->q_start == cfq->q_end) ||
	    (!MBUFQ_EMPTY(&cfq->q_mq) &&
	    cfq->q_start != cfq->q_end));

	MBUFQ_FOREACH(chain, &cfq->q_mq) {
		size_t chainsize = 0;
		m = chain;
		unsigned int mlen = cfil_data_length(m, NULL, NULL);
		// skip the addr and control stuff if present
		m = cfil_data_start(m);

		if (m == NULL ||
		    m == (void *)M_TAG_FREE_PATTERN ||
		    m->m_next == (void *)M_TAG_FREE_PATTERN ||
		    m->m_nextpkt == (void *)M_TAG_FREE_PATTERN) {
			panic("%s - mq %p is free at %p", __func__,
			    &cfq->q_mq, m);
		}
		for (n = m; n != NULL; n = n->m_next) {
			if (!m_has_mtype(n, MTF_DATA | MTF_HEADER | MTF_OOBDATA)) {
				panic("%s - %p unsupported type %u", __func__,
				    n, n->m_type);
			}
			chainsize += n->m_len;
		}
		if (mlen != chainsize) {
			panic("%s - %p m_length() %u != chainsize %lu",
			    __func__, m, mlen, chainsize);
		}
		queuesize += chainsize;
	}
	OS_ANALYZER_SUPPRESS("81031590") if (queuesize != cfq->q_end - cfq->q_start) {
		panic("%s - %p queuesize %llu != offsetdiffs %llu", __func__,
		    m, queuesize, cfq->q_end - cfq->q_start);
	}
}

static void
cfil_queue_enqueue(struct cfil_queue *cfq, mbuf_t m, size_t len)
{
	CFIL_QUEUE_VERIFY(cfq);

	MBUFQ_ENQUEUE(&cfq->q_mq, m);
	cfq->q_end += len;

	CFIL_QUEUE_VERIFY(cfq);
}

static void
cfil_queue_remove(struct cfil_queue *cfq, mbuf_t m, size_t len)
{
	CFIL_QUEUE_VERIFY(cfq);

	VERIFY(cfil_data_length(m, NULL, NULL) == len);

	MBUFQ_REMOVE(&cfq->q_mq, m);
	MBUFQ_NEXT(m) = NULL;
	cfq->q_start += len;

	CFIL_QUEUE_VERIFY(cfq);
}

static mbuf_t
cfil_queue_first(struct cfil_queue *cfq)
{
	return MBUFQ_FIRST(&cfq->q_mq);
}

static mbuf_t
cfil_queue_next(struct cfil_queue *cfq, mbuf_t m)
{
#pragma unused(cfq)
	return MBUFQ_NEXT(m);
}

static void
cfil_entry_buf_verify(struct cfe_buf *cfe_buf)
{
	CFIL_QUEUE_VERIFY(&cfe_buf->cfe_ctl_q);
	CFIL_QUEUE_VERIFY(&cfe_buf->cfe_pending_q);

	/* Verify the queues are ordered so that pending is before ctl */
	VERIFY(cfe_buf->cfe_ctl_q.q_start >= cfe_buf->cfe_pending_q.q_end);

	/* The peek offset cannot be less than the pass offset */
	VERIFY(cfe_buf->cfe_peek_offset >= cfe_buf->cfe_pass_offset);

	/* Make sure we've updated the offset we peeked at  */
	VERIFY(cfe_buf->cfe_ctl_q.q_start <= cfe_buf->cfe_peeked);
}

static void
cfil_entry_verify(struct cfil_entry *entry)
{
	cfil_entry_buf_verify(&entry->cfe_snd);
	cfil_entry_buf_verify(&entry->cfe_rcv);
}

static void
cfil_info_buf_verify(struct cfi_buf *cfi_buf)
{
	CFIL_QUEUE_VERIFY(&cfi_buf->cfi_inject_q);

	VERIFY(cfi_buf->cfi_pending_first <= cfi_buf->cfi_pending_last);
}

static void
cfil_info_verify(struct cfil_info *cfil_info)
{
	int i;

	if (cfil_info == NULL) {
		return;
	}

	cfil_info_buf_verify(&cfil_info->cfi_snd);
	cfil_info_buf_verify(&cfil_info->cfi_rcv);

	for (i = 0; i < MAX_CONTENT_FILTER; i++) {
		cfil_entry_verify(&cfil_info->cfi_entries[i]);
	}
}

static void
verify_content_filter(struct content_filter *cfc)
{
	struct cfil_entry *entry;
	uint32_t count = 0;

	VERIFY(cfc->cf_sock_count >= 0);

	TAILQ_FOREACH(entry, &cfc->cf_sock_entries, cfe_link) {
		count++;
		VERIFY(cfc == entry->cfe_filter);
	}
	VERIFY(count == cfc->cf_sock_count);
}

/*
 * Kernel control socket callbacks
 */
static errno_t
cfil_ctl_connect(kern_ctl_ref kctlref, struct sockaddr_ctl *sac,
    void **unitinfo)
{
	errno_t error = 0;
	struct content_filter * __single cfc = NULL;

	CFIL_LOG(LOG_NOTICE, "");

	cfc = zalloc_flags(content_filter_zone, Z_WAITOK | Z_ZERO | Z_NOFAIL);

	cfil_rw_lock_exclusive(&cfil_lck_rw);

	if (sac->sc_unit == 0 || sac->sc_unit > MAX_CONTENT_FILTER) {
		CFIL_LOG(LOG_ERR, "bad sc_unit %u", sac->sc_unit);
		error = EINVAL;
	} else if (content_filters[sac->sc_unit - 1] != NULL) {
		CFIL_LOG(LOG_ERR, "sc_unit %u in use", sac->sc_unit);
		error = EADDRINUSE;
	} else {
		/*
		 * kernel control socket kcunit numbers start at 1
		 */
		content_filters[sac->sc_unit - 1] = cfc;

		cfc->cf_kcref = kctlref;
		cfc->cf_kcunit = sac->sc_unit;
		TAILQ_INIT(&cfc->cf_sock_entries);

		*unitinfo = cfc;
		cfil_active_count++;

		if (cfil_active_count == 1) {
			soflow_feat_set_functions(cfil_dgram_gc_needed, cfil_dgram_gc_perform,
			    cfil_dgram_detach_entry, cfil_dgram_detach_db);
		}

		// Allocate periodic stats buffer for this filter
		if (global_cfil_stats_report_buffers[cfc->cf_kcunit - 1] == NULL) {
			cfil_rw_unlock_exclusive(&cfil_lck_rw);

			struct cfil_stats_report_buffer * __single buf;

			buf = kalloc_type(struct cfil_stats_report_buffer,
			    Z_WAITOK | Z_ZERO | Z_NOFAIL);

			cfil_rw_lock_exclusive(&cfil_lck_rw);

			/* Another thread may have won the race */
			if (global_cfil_stats_report_buffers[cfc->cf_kcunit - 1] != NULL) {
				kfree_type(struct cfil_stats_report_buffer, buf);
			} else {
				global_cfil_stats_report_buffers[cfc->cf_kcunit - 1] = buf;
			}
		}
	}
	cfil_rw_unlock_exclusive(&cfil_lck_rw);

	if (error != 0 && cfc != NULL) {
		zfree(content_filter_zone, cfc);
	}

	if (error == 0) {
		OSIncrementAtomic(&cfil_stats.cfs_ctl_connect_ok);
	} else {
		OSIncrementAtomic(&cfil_stats.cfs_ctl_connect_fail);
	}

	CFIL_LOG(LOG_INFO, "return %d cfil_active_count %u kcunit %u",
	    error, cfil_active_count, sac->sc_unit);

	return error;
}

static void
cfil_update_behavior_flags(void)
{
	struct content_filter *cfc = NULL;

	// Update global flag
	bool preserve_connections = false;
	for (int i = 0; i < MAX_CONTENT_FILTER; i++) {
		cfc = content_filters[i];
		if (cfc != NULL) {
			if (cfc->cf_flags & CFF_PRESERVE_CONNECTIONS) {
				preserve_connections = true;
			} else {
				preserve_connections = false;
				break;
			}
		}
	}
	if (preserve_connections == true) {
		cfil_behavior_flags |= CFIL_BEHAVIOR_FLAG_PRESERVE_CONNECTIONS;
	} else {
		cfil_behavior_flags &= ~CFIL_BEHAVIOR_FLAG_PRESERVE_CONNECTIONS;
	}
	CFIL_LOG(LOG_INFO, "CFIL Preserve Connections - %s",
	    (cfil_behavior_flags & CFIL_BEHAVIOR_FLAG_PRESERVE_CONNECTIONS) ? "On" : "Off");
}

static errno_t
cfil_ctl_disconnect(kern_ctl_ref kctlref, u_int32_t kcunit, void *unitinfo)
{
#pragma unused(kctlref)
	errno_t error = 0;
	struct content_filter * __single cfc;
	struct cfil_entry *entry;
	uint64_t sock_flow_id = 0;

	CFIL_LOG(LOG_NOTICE, "");

	if (kcunit > MAX_CONTENT_FILTER) {
		CFIL_LOG(LOG_ERR, "kcunit %u > MAX_CONTENT_FILTER (%d)",
		    kcunit, MAX_CONTENT_FILTER);
		error = EINVAL;
		goto done;
	}

	cfc = (struct content_filter *)unitinfo;
	if (cfc == NULL) {
		goto done;
	}

	cfil_rw_lock_exclusive(&cfil_lck_rw);
	if (content_filters[kcunit - 1] != cfc || cfc->cf_kcunit != kcunit) {
		CFIL_LOG(LOG_ERR, "bad unit info %u)",
		    kcunit);
		cfil_rw_unlock_exclusive(&cfil_lck_rw);
		goto done;
	}
	cfc->cf_flags |= CFF_DETACHING;
	/*
	 * Remove all sockets from the filter
	 */
	while ((entry = TAILQ_FIRST(&cfc->cf_sock_entries)) != NULL) {
		cfil_rw_lock_assert_held(&cfil_lck_rw, 1);

		verify_content_filter(cfc);
		/*
		 * Accept all outstanding data by pushing to next filter
		 * or back to socket
		 *
		 * TBD: Actually we should make sure all data has been pushed
		 * back to socket
		 */
		if (entry->cfe_cfil_info && entry->cfe_cfil_info->cfi_so) {
			struct cfil_info *cfil_info = entry->cfe_cfil_info;
			struct socket *so = cfil_info->cfi_so;
			sock_flow_id = cfil_info->cfi_sock_id;

			/* Need to let data flow immediately */
			entry->cfe_flags |= CFEF_SENT_SOCK_ATTACHED |
			    CFEF_DATA_START;

			// Before we release global lock, retain the cfil_info -
			// We attempt to retain a valid cfil_info to prevent any deallocation until
			// we are done.  Abort retain if cfil_info has already entered the free code path.
			if (cfil_info == NULL || os_ref_retain_try(&cfil_info->cfi_ref_count) == false) {
				// Failing to retain cfil_info means detach is in progress already,
				// remove entry from filter list and move on.
				entry->cfe_filter = NULL;
				entry->cfe_necp_control_unit = 0;
				TAILQ_REMOVE(&cfc->cf_sock_entries, entry, cfe_link);
				cfc->cf_sock_count--;
				continue;
			}

			/*
			 * Respect locking hierarchy
			 */
			cfil_rw_unlock_exclusive(&cfil_lck_rw);

			// Search for socket from cfil_info sock_flow_id and lock so
			so = cfil_socket_from_sock_id(sock_flow_id, false);
			if (so == NULL || so != cfil_info->cfi_so) {
				cfil_rw_lock_exclusive(&cfil_lck_rw);

				// Socket has already been disconnected and removed from socket list.
				// Remove entry from filter list and move on.
				if (entry == TAILQ_FIRST(&cfc->cf_sock_entries)) {
					entry->cfe_filter = NULL;
					entry->cfe_necp_control_unit = 0;
					TAILQ_REMOVE(&cfc->cf_sock_entries, entry, cfe_link);
					cfc->cf_sock_count--;
				}

				goto release_cfil_info;
			}

			/*
			 * When cfe_filter is NULL the filter is detached
			 * and the entry has been removed from cf_sock_entries
			 */
			if ((so->so_cfil == NULL && so->so_flow_db == NULL) || entry->cfe_filter == NULL) {
				cfil_rw_lock_exclusive(&cfil_lck_rw);
				goto release;
			}

			(void) cfil_action_data_pass(so, cfil_info, kcunit, 1,
			    CFM_MAX_OFFSET,
			    CFM_MAX_OFFSET);

			(void) cfil_action_data_pass(so, cfil_info, kcunit, 0,
			    CFM_MAX_OFFSET,
			    CFM_MAX_OFFSET);

			cfil_rw_lock_exclusive(&cfil_lck_rw);

			/*
			 * Check again to make sure if the cfil_info is still valid
			 * as the socket may have been unlocked when when calling
			 * cfil_acquire_sockbuf()
			 */
			if (entry->cfe_filter == NULL ||
			    (so->so_cfil == NULL && soflow_db_get_feature_context(so->so_flow_db, sock_flow_id) == NULL)) {
				goto release;
			}

			/* The filter is now detached */
			entry->cfe_flags |= CFEF_CFIL_DETACHED;

			if (cfil_info->cfi_debug) {
				cfil_info_log(LOG_ERR, cfil_info, "CFIL: FILTER DISCONNECTED");
			}

			CFIL_LOG(LOG_NOTICE, "so %llx detached %u",
			    (uint64_t)VM_KERNEL_ADDRPERM(so), kcunit);
			if ((cfil_info->cfi_flags & CFIF_CLOSE_WAIT) &&
			    cfil_filters_attached(so) == 0) {
				CFIL_LOG(LOG_NOTICE, "so %llx waking",
				    (uint64_t)VM_KERNEL_ADDRPERM(so));
				wakeup((caddr_t)cfil_info);
			}

			/*
			 * Remove the filter entry from the content filter
			 * but leave the rest of the state intact as the queues
			 * may not be empty yet
			 */
			entry->cfe_filter = NULL;
			entry->cfe_necp_control_unit = 0;

			TAILQ_REMOVE(&cfc->cf_sock_entries, entry, cfe_link);
			cfc->cf_sock_count--;

			// This is the last filter disconnecting, clear the cfil_info
			// saved policy state so we will be able to drop this flow if
			// a new filter get installed.
			if (cfil_active_count == 1) {
				cfil_info->cfi_filter_control_unit = 0;
				cfil_info->cfi_filter_policy_gencount = 0;
			}
release:
			socket_unlock(so, 1);

release_cfil_info:
			/*
			 * Release reference on cfil_info.  To avoid double locking,
			 * temporarily unlock in case it has been detached and we
			 * end up freeing it which will take the global lock again.
			 */
			cfil_rw_unlock_exclusive(&cfil_lck_rw);
			CFIL_INFO_FREE(cfil_info);
			cfil_rw_lock_exclusive(&cfil_lck_rw);
		}
	}
	verify_content_filter(cfc);

	/* Free the stats buffer for this filter */
	if (global_cfil_stats_report_buffers[cfc->cf_kcunit - 1] != NULL) {
		kfree_type(struct cfil_stats_report_buffer,
		    global_cfil_stats_report_buffers[cfc->cf_kcunit - 1]);
		global_cfil_stats_report_buffers[cfc->cf_kcunit - 1] = NULL;
	}
	VERIFY(cfc->cf_sock_count == 0);

	/*
	 * Make filter inactive
	 */
	content_filters[kcunit - 1] = NULL;
	cfil_active_count--;
	cfil_update_behavior_flags();
	cfil_rw_unlock_exclusive(&cfil_lck_rw);

	if (cfc->cf_crypto_state != NULL) {
		cfil_crypto_cleanup_state(cfc->cf_crypto_state);
		cfc->cf_crypto_state = NULL;
	}

	zfree(content_filter_zone, cfc);
done:
	if (error == 0) {
		OSIncrementAtomic(&cfil_stats.cfs_ctl_disconnect_ok);
	} else {
		OSIncrementAtomic(&cfil_stats.cfs_ctl_disconnect_fail);
	}

	CFIL_LOG(LOG_INFO, "return %d cfil_active_count %u kcunit %u",
	    error, cfil_active_count, kcunit);

	return error;
}

/*
 * cfil_acquire_sockbuf()
 *
 * Prevent any other thread from acquiring the sockbuf
 * We use sb_cfil_thread as a semaphore to prevent other threads from
 * messing with the sockbuf -- see sblock()
 * Note: We do not set SB_LOCK here because the thread may check or modify
 * SB_LOCK several times until it calls cfil_release_sockbuf() -- currently
 * sblock(), sbunlock() or sodefunct()
 */
static int
cfil_acquire_sockbuf(struct socket *so, struct cfil_info *cfil_info, int outgoing)
{
	thread_t __single tp = current_thread();
	struct sockbuf *sb = outgoing ? &so->so_snd : &so->so_rcv;
	lck_mtx_t *mutex_held;
	int error = 0;

	/*
	 * Wait until no thread is holding the sockbuf and other content
	 * filter threads have released the sockbuf
	 */
	while ((sb->sb_flags & SB_LOCK) ||
	    (sb->sb_cfil_thread != NULL && sb->sb_cfil_thread != tp)) {
		if (so->so_proto->pr_getlock != NULL) {
			mutex_held = (*so->so_proto->pr_getlock)(so, PR_F_WILLUNLOCK);
		} else {
			mutex_held = so->so_proto->pr_domain->dom_mtx;
		}

		LCK_MTX_ASSERT(mutex_held, LCK_MTX_ASSERT_OWNED);

		sb->sb_wantlock++;
		VERIFY(sb->sb_wantlock != 0);

		msleep(&sb->sb_flags, mutex_held, PSOCK, "cfil_acquire_sockbuf",
		    NULL);

		VERIFY(sb->sb_wantlock != 0);
		sb->sb_wantlock--;
	}
	/*
	 * Use reference count for repetitive calls on same thread
	 */
	if (sb->sb_cfil_refs == 0) {
		VERIFY(sb->sb_cfil_thread == NULL);
		VERIFY((sb->sb_flags & SB_LOCK) == 0);

		sb->sb_cfil_thread = tp;
		sb->sb_flags |= SB_LOCK;
	}
	sb->sb_cfil_refs++;

	/* We acquire the socket buffer when we need to cleanup */
	if (cfil_info == NULL) {
		CFIL_LOG(LOG_ERR, "so %llx cfil detached",
		    (uint64_t)VM_KERNEL_ADDRPERM(so));
		error = 0;
	} else if (cfil_info->cfi_flags & CFIF_DROP) {
		CFIL_LOG(LOG_ERR, "so %llx drop set",
		    (uint64_t)VM_KERNEL_ADDRPERM(so));
		error = EPIPE;
	}

	return error;
}

static void
cfil_release_sockbuf(struct socket *so, int outgoing)
{
	struct sockbuf *sb = outgoing ? &so->so_snd : &so->so_rcv;
	thread_t __single tp = current_thread();

	socket_lock_assert_owned(so);

	if (sb->sb_cfil_thread != NULL && sb->sb_cfil_thread != tp) {
		panic("%s sb_cfil_thread %p not current %p", __func__,
		    sb->sb_cfil_thread, tp);
	}
	/*
	 * Don't panic if we are defunct because SB_LOCK has
	 * been cleared by sodefunct()
	 */
	if (!(so->so_flags & SOF_DEFUNCT) && !(sb->sb_flags & SB_LOCK)) {
		panic("%s SB_LOCK not set on %p", __func__,
		    sb);
	}
	/*
	 * We can unlock when the thread unwinds to the last reference
	 */
	sb->sb_cfil_refs--;
	if (sb->sb_cfil_refs == 0) {
		sb->sb_cfil_thread = NULL;
		sb->sb_flags &= ~SB_LOCK;

		if (sb->sb_wantlock > 0) {
			wakeup(&sb->sb_flags);
		}
	}
}

cfil_sock_id_t
cfil_sock_id_from_socket(struct socket *so)
{
	if ((so->so_flags & SOF_CONTENT_FILTER) && so->so_cfil) {
		return so->so_cfil->cfi_sock_id;
	} else {
		return CFIL_SOCK_ID_NONE;
	}
}

/*
 * cfil_socket_safe_lock -
 * This routine attempts to lock the socket safely.
 *
 * The passed in pcbinfo is assumed to be locked and must be unlocked once the
 * inp state is safeguarded and before we attempt to lock/unlock the socket.
 * This is to prevent getting blocked by socket_lock() while holding the pcbinfo
 * lock, avoiding potential deadlock with other processes contending for the same
 * resources.  This is also to avoid double locking the pcbinfo for rip sockets
 * since rip_unlock() will lock ripcbinfo if it needs to dispose inpcb when
 * so_usecount is 0.
 */
static bool
cfil_socket_safe_lock(struct inpcb *inp, struct inpcbinfo *pcbinfo)
{
	struct socket *so = NULL;

	VERIFY(pcbinfo != NULL);

	if (in_pcb_checkstate(inp, WNT_ACQUIRE, 0) != WNT_STOPUSING) {
		// Safeguarded the inp state, unlock pcbinfo before locking socket.
		lck_rw_done(&pcbinfo->ipi_lock);

		so = inp->inp_socket;
		socket_lock(so, 1);
		if (in_pcb_checkstate(inp, WNT_RELEASE, 1) != WNT_STOPUSING) {
			return true;
		}
	} else {
		// Failed to safeguarded the inp state, unlock pcbinfo and abort.
		lck_rw_done(&pcbinfo->ipi_lock);
	}

	if (so) {
		socket_unlock(so, 1);
	}
	return false;
}

static struct socket *
cfil_socket_from_sock_id(cfil_sock_id_t cfil_sock_id, bool udp_only)
{
	struct socket *so = NULL;
	u_int64_t gencnt = cfil_sock_id >> 32;
	u_int32_t flowhash = (u_int32_t)(cfil_sock_id & 0x0ffffffff);
	struct inpcb *inp = NULL;
	struct inpcbinfo *pcbinfo = NULL;

	if (udp_only) {
		goto find_udp;
	}

	pcbinfo = &tcbinfo;
	lck_rw_lock_shared(&pcbinfo->ipi_lock);
	LIST_FOREACH(inp, pcbinfo->ipi_listhead, inp_list) {
		if (inp->inp_state != INPCB_STATE_DEAD &&
		    inp->inp_socket != NULL &&
		    inp->inp_flowhash == flowhash &&
		    (inp->inp_socket->so_gencnt & 0x0ffffffff) == gencnt &&
		    inp->inp_socket->so_cfil != NULL) {
			if (cfil_socket_safe_lock(inp, pcbinfo)) {
				so = inp->inp_socket;
			}
			/* pcbinfo is already unlocked, we are done. */
			goto done;
		}
	}
	lck_rw_done(&pcbinfo->ipi_lock);
	if (so != NULL) {
		goto done;
	}

find_udp:

	pcbinfo = &udbinfo;
	lck_rw_lock_shared(&pcbinfo->ipi_lock);
	LIST_FOREACH(inp, pcbinfo->ipi_listhead, inp_list) {
		if (inp->inp_state != INPCB_STATE_DEAD &&
		    inp->inp_socket != NULL &&
		    inp->inp_socket->so_flow_db != NULL &&
		    (inp->inp_socket->so_gencnt & 0x0ffffffff) == gencnt) {
			if (cfil_socket_safe_lock(inp, pcbinfo)) {
				so = inp->inp_socket;
			}
			/* pcbinfo is already unlocked, we are done. */
			goto done;
		}
	}
	lck_rw_done(&pcbinfo->ipi_lock);
	if (so != NULL) {
		goto done;
	}

	pcbinfo = &ripcbinfo;
	lck_rw_lock_shared(&pcbinfo->ipi_lock);
	LIST_FOREACH(inp, pcbinfo->ipi_listhead, inp_list) {
		if (inp->inp_state != INPCB_STATE_DEAD &&
		    inp->inp_socket != NULL &&
		    inp->inp_socket->so_flow_db != NULL &&
		    (inp->inp_socket->so_gencnt & 0x0ffffffff) == gencnt) {
			if (cfil_socket_safe_lock(inp, pcbinfo)) {
				so = inp->inp_socket;
			}
			/* pcbinfo is already unlocked, we are done. */
			goto done;
		}
	}
	lck_rw_done(&pcbinfo->ipi_lock);

done:
	if (so == NULL) {
		OSIncrementAtomic(&cfil_stats.cfs_sock_id_not_found);
		CFIL_LOG(LOG_DEBUG,
		    "no socket for sock_id %llx gencnt %llx flowhash %x",
		    cfil_sock_id, gencnt, flowhash);
	}

	return so;
}

static struct socket *
cfil_socket_from_client_uuid(uuid_t necp_client_uuid, bool *cfil_attached)
{
	struct socket *so = NULL;
	struct inpcb *inp = NULL;
	struct inpcbinfo *pcbinfo = &tcbinfo;

	lck_rw_lock_shared(&pcbinfo->ipi_lock);
	LIST_FOREACH(inp, pcbinfo->ipi_listhead, inp_list) {
		if (inp->inp_state != INPCB_STATE_DEAD &&
		    inp->inp_socket != NULL &&
		    uuid_compare(inp->necp_client_uuid, necp_client_uuid) == 0) {
			*cfil_attached = (inp->inp_socket->so_cfil != NULL);
			if (cfil_socket_safe_lock(inp, pcbinfo)) {
				so = inp->inp_socket;
			}
			/* pcbinfo is already unlocked, we are done. */
			goto done;
		}
	}
	lck_rw_done(&pcbinfo->ipi_lock);
	if (so != NULL) {
		goto done;
	}

	pcbinfo = &udbinfo;
	lck_rw_lock_shared(&pcbinfo->ipi_lock);
	LIST_FOREACH(inp, pcbinfo->ipi_listhead, inp_list) {
		if (inp->inp_state != INPCB_STATE_DEAD &&
		    inp->inp_socket != NULL &&
		    uuid_compare(inp->necp_client_uuid, necp_client_uuid) == 0) {
			*cfil_attached = (inp->inp_socket->so_flow_db != NULL);
			if (cfil_socket_safe_lock(inp, pcbinfo)) {
				so = inp->inp_socket;
			}
			/* pcbinfo is already unlocked, we are done. */
			goto done;
		}
	}
	lck_rw_done(&pcbinfo->ipi_lock);

done:
	return so;
}

static void
cfil_info_stats_toggle(struct cfil_info *cfil_info, struct cfil_entry *entry, uint32_t report_frequency)
{
	struct cfil_info *cfil = NULL;
	Boolean found = FALSE;
	int kcunit;

	if (cfil_info == NULL) {
		return;
	}

	if (report_frequency) {
		if (entry == NULL) {
			return;
		}

		// Update stats reporting frequency.
		if (entry->cfe_stats_report_frequency != report_frequency) {
			entry->cfe_stats_report_frequency = report_frequency;
			if (entry->cfe_stats_report_frequency < CFIL_STATS_REPORT_INTERVAL_MIN_MSEC) {
				entry->cfe_stats_report_frequency = CFIL_STATS_REPORT_INTERVAL_MIN_MSEC;
			}
			microuptime(&entry->cfe_stats_report_ts);

			// Insert cfil_info into list only if it is not in yet.
			TAILQ_FOREACH(cfil, &cfil_sock_head_stats, cfi_link_stats) {
				if (cfil == cfil_info) {
					return;
				}
			}

			TAILQ_INSERT_TAIL(&cfil_sock_head_stats, cfil_info, cfi_link_stats);

			// Wake up stats thread if this is first flow added
			if (cfil_sock_attached_stats_count == 0) {
				thread_wakeup((caddr_t)&cfil_sock_attached_stats_count);
			}
			cfil_sock_attached_stats_count++;

			if (cfil_info->cfi_debug && cfil_log_stats) {
				CFIL_LOG(LOG_ERR, "CFIL: VERDICT RECEIVED - STATS FLOW INSERTED: <so %llx sockID %llu <%llx>> stats frequency %d msecs",
				    cfil_info->cfi_so ? (uint64_t)VM_KERNEL_ADDRPERM(cfil_info->cfi_so) : 0,
				    cfil_info->cfi_sock_id, cfil_info->cfi_sock_id,
				    entry->cfe_stats_report_frequency);
			}
		}
	} else {
		// Turn off stats reporting for this filter.
		if (entry != NULL) {
			// Already off, no change.
			if (entry->cfe_stats_report_frequency == 0) {
				return;
			}

			entry->cfe_stats_report_frequency = 0;
			// If cfil_info still has filter(s) asking for stats, no need to remove from list.
			for (kcunit = 1; kcunit <= MAX_CONTENT_FILTER; kcunit++) {
				if (cfil_info->cfi_entries[kcunit - 1].cfe_stats_report_frequency > 0) {
					return;
				}
			}
		}

		// No more filter asking for stats for this cfil_info, remove from list.
		if (!TAILQ_EMPTY(&cfil_sock_head_stats)) {
			found = FALSE;
			TAILQ_FOREACH(cfil, &cfil_sock_head_stats, cfi_link_stats) {
				if (cfil == cfil_info) {
					found = TRUE;
					break;
				}
			}
			if (found) {
				cfil_sock_attached_stats_count--;
				TAILQ_REMOVE(&cfil_sock_head_stats, cfil_info, cfi_link_stats);
				if (cfil_info->cfi_debug && cfil_log_stats) {
					CFIL_LOG(LOG_ERR, "CFIL: VERDICT RECEIVED - STATS FLOW DELETED: <so %llx sockID %llu <%llx>> stats frequency reset",
					    cfil_info->cfi_so ? (uint64_t)VM_KERNEL_ADDRPERM(cfil_info->cfi_so) : 0,
					    cfil_info->cfi_sock_id, cfil_info->cfi_sock_id);
				}
			}
		}
	}
}

static errno_t
cfil_ctl_send(kern_ctl_ref kctlref, u_int32_t kcunit, void *unitinfo, mbuf_t m,
    int flags)
{
#pragma unused(kctlref, flags)
	errno_t error = 0;
	struct cfil_msg_hdr *msghdr;
	struct content_filter *cfc = (struct content_filter *)unitinfo;
	struct socket *so;
	struct cfil_msg_action * __single action_msg;
	struct cfil_entry *entry;
	struct cfil_info * __single cfil_info = NULL;
	unsigned int data_len = 0;

	CFIL_LOG(LOG_INFO, "");

	if (cfc == NULL) {
		CFIL_LOG(LOG_ERR, "no unitinfo");
		error = EINVAL;
		goto done;
	}

	if (kcunit > MAX_CONTENT_FILTER) {
		CFIL_LOG(LOG_ERR, "kcunit %u > MAX_CONTENT_FILTER (%d)",
		    kcunit, MAX_CONTENT_FILTER);
		error = EINVAL;
		goto done;
	}
	if (m == NULL) {
		CFIL_LOG(LOG_ERR, "null mbuf");
		error = EINVAL;
		goto done;
	}
	data_len = m_length(m);

	if (data_len < sizeof(struct cfil_msg_hdr)) {
		CFIL_LOG(LOG_ERR, "too short %u", data_len);
		error = EINVAL;
		goto done;
	}
	msghdr = mtod(m, struct cfil_msg_hdr *);
	if (msghdr->cfm_version != CFM_VERSION_CURRENT) {
		CFIL_LOG(LOG_ERR, "bad version %u", msghdr->cfm_version);
		error = EINVAL;
		goto done;
	}
	if (msghdr->cfm_type != CFM_TYPE_ACTION) {
		CFIL_LOG(LOG_ERR, "bad type %u", msghdr->cfm_type);
		error = EINVAL;
		goto done;
	}
	if (msghdr->cfm_len > data_len) {
		CFIL_LOG(LOG_ERR, "bad length %u", msghdr->cfm_len);
		error = EINVAL;
		goto done;
	}

	/* Validate action operation */
	switch (msghdr->cfm_op) {
	case CFM_OP_DATA_UPDATE:
		OSIncrementAtomic(
			&cfil_stats.cfs_ctl_action_data_update);
		break;
	case CFM_OP_DROP:
		OSIncrementAtomic(&cfil_stats.cfs_ctl_action_drop);
		break;
	case CFM_OP_BLESS_CLIENT:
		if (msghdr->cfm_len != sizeof(struct cfil_msg_bless_client)) {
			OSIncrementAtomic(&cfil_stats.cfs_ctl_action_bad_len);
			error = EINVAL;
			CFIL_LOG(LOG_ERR, "bad len: %u for op %u",
			    msghdr->cfm_len,
			    msghdr->cfm_op);
			goto done;
		}
		error = cfil_action_bless_client(kcunit, msghdr);
		goto done;
	case CFM_OP_SET_CRYPTO_KEY:
		if (msghdr->cfm_len != sizeof(struct cfil_msg_set_crypto_key)) {
			OSIncrementAtomic(&cfil_stats.cfs_ctl_action_bad_len);
			error = EINVAL;
			CFIL_LOG(LOG_ERR, "bad len: %u for op %u",
			    msghdr->cfm_len,
			    msghdr->cfm_op);
			goto done;
		}
		error = cfil_action_set_crypto_key(kcunit, msghdr);
		goto done;
	default:
		OSIncrementAtomic(&cfil_stats.cfs_ctl_action_bad_op);
		CFIL_LOG(LOG_ERR, "bad op %u", msghdr->cfm_op);
		error = EINVAL;
		goto done;
	}
	if (msghdr->cfm_len != sizeof(struct cfil_msg_action)) {
		OSIncrementAtomic(&cfil_stats.cfs_ctl_action_bad_len);
		error = EINVAL;
		CFIL_LOG(LOG_ERR, "bad len: %u for op %u",
		    msghdr->cfm_len,
		    msghdr->cfm_op);
		goto done;
	}
	cfil_rw_lock_shared(&cfil_lck_rw);
	if (cfc != (void *)content_filters[kcunit - 1]) {
		CFIL_LOG(LOG_ERR, "unitinfo does not match for kcunit %u",
		    kcunit);
		error = EINVAL;
		cfil_rw_unlock_shared(&cfil_lck_rw);
		goto done;
	}
	cfil_rw_unlock_shared(&cfil_lck_rw);

	// Search for socket (TCP+UDP and lock so)
	so = cfil_socket_from_sock_id(msghdr->cfm_sock_id, false);
	if (so == NULL) {
		CFIL_LOG(LOG_NOTICE, "bad sock_id %llx",
		    msghdr->cfm_sock_id);
		error = EINVAL;
		goto done;
	}

	cfil_info = so->so_flow_db != NULL ?
	    soflow_db_get_feature_context(so->so_flow_db, msghdr->cfm_sock_id) : so->so_cfil;

	// We should not obtain global lock here in order to avoid deadlock down the path.
	// But we attempt to retain a valid cfil_info to prevent any deallocation until
	// we are done.  Abort retain if cfil_info has already entered the free code path.
	if (cfil_info && os_ref_retain_try(&cfil_info->cfi_ref_count) == false) {
		socket_unlock(so, 1);
		goto done;
	}

	if (cfil_info == NULL) {
		CFIL_LOG(LOG_NOTICE, "so %llx <id %llu> not attached",
		    (uint64_t)VM_KERNEL_ADDRPERM(so), msghdr->cfm_sock_id);
		error = EINVAL;
		goto unlock;
	} else if (cfil_info->cfi_flags & CFIF_DROP) {
		CFIL_LOG(LOG_NOTICE, "so %llx drop set",
		    (uint64_t)VM_KERNEL_ADDRPERM(so));
		error = EINVAL;
		goto unlock;
	}

	if (cfil_info->cfi_debug) {
		cfil_info_log(LOG_ERR, cfil_info, "CFIL: RECEIVED MSG FROM FILTER");
	}

	entry = &cfil_info->cfi_entries[kcunit - 1];
	if (entry->cfe_filter == NULL) {
		CFIL_LOG(LOG_NOTICE, "so %llx no filter",
		    (uint64_t)VM_KERNEL_ADDRPERM(so));
		error = EINVAL;
		goto unlock;
	}

	if (entry->cfe_flags & CFEF_SENT_SOCK_ATTACHED) {
		entry->cfe_flags |= CFEF_DATA_START;
	} else {
		CFIL_LOG(LOG_ERR,
		    "so %llx attached not sent for %u",
		    (uint64_t)VM_KERNEL_ADDRPERM(so), kcunit);
		error = EINVAL;
		goto unlock;
	}

	microuptime(&entry->cfe_last_action);
	CFI_ADD_TIME_LOG(cfil_info, &entry->cfe_last_action, &cfil_info->cfi_first_event, msghdr->cfm_op);

	action_msg = (struct cfil_msg_action *)msghdr;

	switch (msghdr->cfm_op) {
	case CFM_OP_DATA_UPDATE:

		if (cfil_info->cfi_debug) {
			cfil_info_log(LOG_ERR, cfil_info, "CFIL: RECEIVED CFM_OP_DATA_UPDATE");
			CFIL_LOG(LOG_ERR, "CFIL: VERDICT RECEIVED: <so %llx sockID %llu <%llx>> <IN peek:%llu pass:%llu, OUT peek:%llu pass:%llu>",
			    (uint64_t)VM_KERNEL_ADDRPERM(so),
			    cfil_info->cfi_sock_id, cfil_info->cfi_sock_id,
			    action_msg->cfa_in_peek_offset, action_msg->cfa_in_pass_offset,
			    action_msg->cfa_out_peek_offset, action_msg->cfa_out_pass_offset);
		}

		/*
		 * Received verdict, at this point we know this
		 * socket connection is allowed.  Unblock thread
		 * immediately before proceeding to process the verdict.
		 */
		cfil_sock_received_verdict(so);

		if (action_msg->cfa_out_peek_offset != 0 ||
		    action_msg->cfa_out_pass_offset != 0) {
			error = cfil_action_data_pass(so, cfil_info, kcunit, 1,
			    action_msg->cfa_out_pass_offset,
			    action_msg->cfa_out_peek_offset);
		}
		if (error == EJUSTRETURN) {
			error = 0;
		}
		if (error != 0) {
			break;
		}
		if (action_msg->cfa_in_peek_offset != 0 ||
		    action_msg->cfa_in_pass_offset != 0) {
			error = cfil_action_data_pass(so, cfil_info, kcunit, 0,
			    action_msg->cfa_in_pass_offset,
			    action_msg->cfa_in_peek_offset);
		}
		if (error == EJUSTRETURN) {
			error = 0;
		}

		// Toggle stats reporting according to received verdict.
		cfil_rw_lock_exclusive(&cfil_lck_rw);
		cfil_info_stats_toggle(cfil_info, entry, action_msg->cfa_stats_frequency);
		cfil_rw_unlock_exclusive(&cfil_lck_rw);

		break;

	case CFM_OP_DROP:
		cfil_info_log(LOG_ERR, cfil_info, "CFIL: RECEIVED CFM_OP_DROP");
		CFIL_LOG(LOG_ERR, "CFIL: VERDICT DROP RECEIVED: <so %llx sockID %llu <%llx>> <IN peek:%llu pass:%llu, OUT peek:%llu pass:%llu>",
		    (uint64_t)VM_KERNEL_ADDRPERM(so),
		    cfil_info->cfi_sock_id, cfil_info->cfi_sock_id,
		    action_msg->cfa_in_peek_offset, action_msg->cfa_in_pass_offset,
		    action_msg->cfa_out_peek_offset, action_msg->cfa_out_pass_offset);

		error = cfil_action_drop(so, cfil_info, kcunit);
		cfil_sock_received_verdict(so);
		break;

	default:
		error = EINVAL;
		break;
	}
unlock:
	CFIL_INFO_FREE(cfil_info)
	socket_unlock(so, 1);
done:
	mbuf_freem(m);

	if (error == 0) {
		OSIncrementAtomic(&cfil_stats.cfs_ctl_send_ok);
	} else {
		OSIncrementAtomic(&cfil_stats.cfs_ctl_send_bad);
	}

	return error;
}

static errno_t
cfil_ctl_getopt(kern_ctl_ref kctlref, u_int32_t kcunit, void *unitinfo,
    int opt, void *data, size_t *len)
{
#pragma unused(kctlref, opt)
	struct cfil_info * __single cfil_info = NULL;
	errno_t error = 0;
	struct content_filter *cfc = (struct content_filter *)unitinfo;

	CFIL_LOG(LOG_NOTICE, "");

	if (cfc == NULL) {
		CFIL_LOG(LOG_ERR, "no unitinfo");
		return EINVAL;
	}

	cfil_rw_lock_shared(&cfil_lck_rw);

	if (kcunit > MAX_CONTENT_FILTER) {
		CFIL_LOG(LOG_ERR, "kcunit %u > MAX_CONTENT_FILTER (%d)",
		    kcunit, MAX_CONTENT_FILTER);
		error = EINVAL;
		goto done;
	}
	if (cfc != (void *)content_filters[kcunit - 1]) {
		CFIL_LOG(LOG_ERR, "unitinfo does not match for kcunit %u",
		    kcunit);
		error = EINVAL;
		goto done;
	}
	switch (opt) {
	case CFIL_OPT_NECP_CONTROL_UNIT:
		if (*len < sizeof(uint32_t)) {
			CFIL_LOG(LOG_ERR, "len too small %lu", *len);
			error = EINVAL;
			goto done;
		}
		if (data != NULL) {
			*(uint32_t *)data = cfc->cf_necp_control_unit;
		}
		break;
	case CFIL_OPT_PRESERVE_CONNECTIONS:
		if (*len < sizeof(uint32_t)) {
			CFIL_LOG(LOG_ERR, "CFIL_OPT_PRESERVE_CONNECTIONS len too small %lu", *len);
			error = EINVAL;
			goto done;
		}
		if (data != NULL) {
			*(uint32_t *)data = (cfc->cf_flags & CFF_PRESERVE_CONNECTIONS) ? true : false;
		}
		break;
	case CFIL_OPT_GET_SOCKET_INFO:
		if (*len != sizeof(struct cfil_opt_sock_info)) {
			CFIL_LOG(LOG_ERR, "len does not match %lu", *len);
			error = EINVAL;
			goto done;
		}
		if (data == NULL) {
			CFIL_LOG(LOG_ERR, "data not passed");
			error = EINVAL;
			goto done;
		}

		struct cfil_opt_sock_info *sock_info =
		    (struct cfil_opt_sock_info *) data;

		// Unlock here so that we never hold both cfil_lck_rw and the
		// socket_lock at the same time. Otherwise, this can deadlock
		// because soclose() takes the socket_lock and then exclusive
		// cfil_lck_rw and we require the opposite order.

		// WARNING: Be sure to never use anything protected
		//     by cfil_lck_rw beyond this point.
		// WARNING: Be sure to avoid fallthrough and
		//     goto return_already_unlocked from this branch.
		cfil_rw_unlock_shared(&cfil_lck_rw);

		// Search (TCP+UDP) and lock socket
		struct socket *sock =
		    cfil_socket_from_sock_id(sock_info->cfs_sock_id, false);
		if (sock == NULL) {
			CFIL_LOG(LOG_ERR, "CFIL: GET_SOCKET_INFO failed: bad sock_id %llu",
			    sock_info->cfs_sock_id);
			error = ENOENT;
			goto return_already_unlocked;
		}

		cfil_info = (sock->so_flow_db != NULL) ?
		    soflow_db_get_feature_context(sock->so_flow_db, sock_info->cfs_sock_id) : sock->so_cfil;

		if (cfil_info == NULL) {
			CFIL_LOG(LOG_INFO, "CFIL: GET_SOCKET_INFO failed: so %llx not attached, cannot fetch info",
			    (uint64_t)VM_KERNEL_ADDRPERM(sock));
			error = EINVAL;
			socket_unlock(sock, 1);
			goto return_already_unlocked;
		}

		if (sock->so_proto == NULL || sock->so_proto->pr_domain == NULL) {
			CFIL_LOG(LOG_INFO, "CFIL: GET_SOCKET_INFO failed: so %llx NULL so_proto / pr_domain",
			    (uint64_t)VM_KERNEL_ADDRPERM(sock));
			error = EINVAL;
			socket_unlock(sock, 1);
			goto return_already_unlocked;
		}

		// Fill out family, type, and protocol
		sock_info->cfs_sock_family = SOCK_DOM(sock);
		sock_info->cfs_sock_type = SOCK_TYPE(sock);
		sock_info->cfs_sock_protocol = GET_SO_PROTO(sock);

		// Source and destination addresses
		struct inpcb *inp = sotoinpcb(sock);
		if (inp->inp_vflag & INP_IPV6) {
			struct in6_addr * __single laddr = NULL, * __single faddr = NULL;
			u_int16_t lport = 0, fport = 0;

			cfil_get_flow_address_v6(cfil_info->cfi_hash_entry, inp,
			    &laddr, &faddr, &lport, &fport);
			fill_ip6_sockaddr_4_6(&sock_info->cfs_local, laddr, lport, inp->inp_lifscope);
			fill_ip6_sockaddr_4_6(&sock_info->cfs_remote, faddr, fport, inp->inp_fifscope);
		} else if (inp->inp_vflag & INP_IPV4) {
			struct in_addr laddr = {.s_addr = 0}, faddr = {.s_addr = 0};
			u_int16_t lport = 0, fport = 0;

			cfil_get_flow_address(cfil_info->cfi_hash_entry, inp,
			    &laddr, &faddr, &lport, &fport);
			fill_ip_sockaddr_4_6(&sock_info->cfs_local, laddr, lport);
			fill_ip_sockaddr_4_6(&sock_info->cfs_remote, faddr, fport);
		}

		// Set the pid info
		sock_info->cfs_pid = sock->last_pid;
		memcpy(sock_info->cfs_uuid, sock->last_uuid, sizeof(uuid_t));

		if (sock->so_flags & SOF_DELEGATED) {
			sock_info->cfs_e_pid = sock->e_pid;
			memcpy(sock_info->cfs_e_uuid, sock->e_uuid, sizeof(uuid_t));
		} else {
			sock_info->cfs_e_pid = sock->last_pid;
			memcpy(sock_info->cfs_e_uuid, sock->last_uuid, sizeof(uuid_t));
		}
#if defined(XNU_TARGET_OS_OSX)
		if (!uuid_is_null(sock->so_ruuid)) {
			sock_info->cfs_r_pid = sock->so_rpid;
			memcpy(sock_info->cfs_r_uuid, sock->so_ruuid, sizeof(uuid_t));
		}
#endif
		socket_unlock(sock, 1);

		goto return_already_unlocked;
	default:
		error = ENOPROTOOPT;
		break;
	}
done:
	cfil_rw_unlock_shared(&cfil_lck_rw);

	return error;

return_already_unlocked:

	return error;
}

static errno_t
cfil_ctl_setopt(kern_ctl_ref kctlref, u_int32_t kcunit, void *unitinfo,
    int opt, void *data, size_t len)
{
#pragma unused(kctlref, opt)
	errno_t error = 0;
	struct content_filter *cfc = (struct content_filter *)unitinfo;

	CFIL_LOG(LOG_NOTICE, "");

	if (cfc == NULL) {
		CFIL_LOG(LOG_ERR, "no unitinfo");
		return EINVAL;
	}

	cfil_rw_lock_exclusive(&cfil_lck_rw);

	if (kcunit > MAX_CONTENT_FILTER) {
		CFIL_LOG(LOG_ERR, "kcunit %u > MAX_CONTENT_FILTER (%d)",
		    kcunit, MAX_CONTENT_FILTER);
		error = EINVAL;
		goto done;
	}
	if (cfc != (void *)content_filters[kcunit - 1]) {
		CFIL_LOG(LOG_ERR, "unitinfo does not match for kcunit %u",
		    kcunit);
		error = EINVAL;
		goto done;
	}
	switch (opt) {
	case CFIL_OPT_NECP_CONTROL_UNIT:
		if (len < sizeof(uint32_t)) {
			CFIL_LOG(LOG_ERR, "CFIL_OPT_NECP_CONTROL_UNIT "
			    "len too small %lu", len);
			error = EINVAL;
			goto done;
		}
		if (cfc->cf_necp_control_unit != 0) {
			CFIL_LOG(LOG_ERR, "CFIL_OPT_NECP_CONTROL_UNIT "
			    "already set %u",
			    cfc->cf_necp_control_unit);
			error = EINVAL;
			goto done;
		}
		cfc->cf_necp_control_unit = *(uint32_t *)data;
		break;
	case CFIL_OPT_PRESERVE_CONNECTIONS:
		if (len < sizeof(uint32_t)) {
			CFIL_LOG(LOG_ERR, "CFIL_OPT_PRESERVE_CONNECTIONS "
			    "len too small %lu", len);
			error = EINVAL;
			goto done;
		}
		uint32_t preserve_connections = *((uint32_t *)data);
		CFIL_LOG(LOG_INFO, "CFIL_OPT_PRESERVE_CONNECTIONS got %d (kcunit %d)", preserve_connections, kcunit);
		if (preserve_connections) {
			cfc->cf_flags |= CFF_PRESERVE_CONNECTIONS;
		} else {
			cfc->cf_flags &= ~CFF_PRESERVE_CONNECTIONS;
		}

		cfil_update_behavior_flags();
		break;
	default:
		error = ENOPROTOOPT;
		break;
	}
done:
	cfil_rw_unlock_exclusive(&cfil_lck_rw);

	return error;
}


static void
cfil_ctl_rcvd(kern_ctl_ref kctlref, u_int32_t kcunit, void *unitinfo, int flags)
{
#pragma unused(kctlref, flags)
	struct content_filter *cfc = (struct content_filter *)unitinfo;
	struct socket *so = NULL;
	int error;
	struct cfil_entry *entry;
	struct cfil_info *cfil_info = NULL;

	CFIL_LOG(LOG_INFO, "");

	if (cfc == NULL) {
		CFIL_LOG(LOG_ERR, "no unitinfo");
		OSIncrementAtomic(&cfil_stats.cfs_ctl_rcvd_bad);
		return;
	}

	if (kcunit > MAX_CONTENT_FILTER) {
		CFIL_LOG(LOG_ERR, "kcunit %u > MAX_CONTENT_FILTER (%d)",
		    kcunit, MAX_CONTENT_FILTER);
		OSIncrementAtomic(&cfil_stats.cfs_ctl_rcvd_bad);
		return;
	}
	cfil_rw_lock_shared(&cfil_lck_rw);
	if (cfc != (void *)content_filters[kcunit - 1]) {
		CFIL_LOG(LOG_ERR, "unitinfo does not match for kcunit %u",
		    kcunit);
		OSIncrementAtomic(&cfil_stats.cfs_ctl_rcvd_bad);
		goto done;
	}
	/* Let's assume the flow control is lifted */
	if (cfc->cf_flags & CFF_FLOW_CONTROLLED) {
		if (!cfil_rw_lock_shared_to_exclusive(&cfil_lck_rw)) {
			cfil_rw_lock_exclusive(&cfil_lck_rw);
		}

		cfc->cf_flags &= ~CFF_FLOW_CONTROLLED;

		cfil_rw_lock_exclusive_to_shared(&cfil_lck_rw);
		LCK_RW_ASSERT(&cfil_lck_rw, LCK_RW_ASSERT_SHARED);
	}
	/*
	 * Flow control will be raised again as soon as an entry cannot enqueue
	 * to the kernel control socket
	 */
	while ((cfc->cf_flags & CFF_FLOW_CONTROLLED) == 0) {
		verify_content_filter(cfc);

		cfil_rw_lock_assert_held(&cfil_lck_rw, 0);

		/* Find an entry that is flow controlled */
		TAILQ_FOREACH(entry, &cfc->cf_sock_entries, cfe_link) {
			if (entry->cfe_cfil_info == NULL ||
			    entry->cfe_cfil_info->cfi_so == NULL) {
				continue;
			}
			if ((entry->cfe_flags & CFEF_FLOW_CONTROLLED) == 0) {
				continue;
			}
		}
		if (entry == NULL) {
			break;
		}

		OSIncrementAtomic(&cfil_stats.cfs_ctl_rcvd_flow_lift);

		cfil_info = entry->cfe_cfil_info;
		so = cfil_info->cfi_so;

		if (cfil_info == NULL || os_ref_retain_try(&cfil_info->cfi_ref_count) == false) {
			break;
		}

		cfil_rw_unlock_shared(&cfil_lck_rw);
		socket_lock(so, 1);

		do {
			error = cfil_acquire_sockbuf(so, cfil_info, 1);
			if (error == 0) {
				error = cfil_data_service_ctl_q(so, cfil_info, kcunit, 1);
			}
			cfil_release_sockbuf(so, 1);
			if (error != 0) {
				break;
			}

			error = cfil_acquire_sockbuf(so, cfil_info, 0);
			if (error == 0) {
				error = cfil_data_service_ctl_q(so, cfil_info, kcunit, 0);
			}
			cfil_release_sockbuf(so, 0);
		} while (0);

		CFIL_INFO_FREE(cfil_info);
		socket_lock_assert_owned(so);
		socket_unlock(so, 1);

		cfil_rw_lock_shared(&cfil_lck_rw);
	}
done:
	cfil_rw_unlock_shared(&cfil_lck_rw);
}

struct cflil_tag_container {
	struct m_tag    cfil_m_tag;
	struct cfil_tag cfil_tag;
};

static struct m_tag *
m_tag_kalloc_cfil_udp(u_int32_t id, u_int16_t type, uint16_t len, int wait)
{
	struct cflil_tag_container *tag_container;
	struct m_tag *tag = NULL;

	assert3u(id, ==, KERNEL_MODULE_TAG_ID);
	assert3u(type, ==, KERNEL_TAG_TYPE_CFIL_UDP);
	assert3u(len, ==, sizeof(struct cfil_tag));

	if (len != sizeof(struct cfil_tag)) {
		return NULL;
	}

	tag_container = kalloc_type(struct cflil_tag_container, wait | M_ZERO);
	if (tag_container != NULL) {
		tag =  &tag_container->cfil_m_tag;

		assert3p(tag, ==, tag_container);

		M_TAG_INIT(tag, id, type, len, &tag_container->cfil_tag, NULL);
	}

	return tag;
}

static void
m_tag_kfree_cfil_udp(struct m_tag *tag)
{
	struct cflil_tag_container * __single tag_container = (struct cflil_tag_container *)tag;

	kfree_type(struct cflil_tag_container, tag_container);
}

void
cfil_register_m_tag(void)
{
	errno_t error = 0;

	error = m_register_internal_tag_type(KERNEL_TAG_TYPE_CFIL_UDP, sizeof(struct cfil_tag),
	    m_tag_kalloc_cfil_udp, m_tag_kfree_cfil_udp);

	assert3u(error, ==, 0);
}

void
cfil_init(void)
{
	struct kern_ctl_reg kern_ctl;
	errno_t error = 0;
	unsigned int mbuf_limit = 0;

	CFIL_LOG(LOG_NOTICE, "");

	/*
	 * Compile time verifications
	 */
	_CASSERT(CFIL_MAX_FILTER_COUNT == MAX_CONTENT_FILTER);
	_CASSERT(sizeof(struct cfil_filter_stat) % sizeof(uint32_t) == 0);
	_CASSERT(sizeof(struct cfil_entry_stat) % sizeof(uint32_t) == 0);
	_CASSERT(sizeof(struct cfil_sock_stat) % sizeof(uint32_t) == 0);

	/*
	 * Runtime time verifications
	 */
	VERIFY(IS_P2ALIGNED(&cfil_stats.cfs_ctl_q_in_enqueued,
	    sizeof(uint32_t)));
	VERIFY(IS_P2ALIGNED(&cfil_stats.cfs_ctl_q_out_enqueued,
	    sizeof(uint32_t)));
	VERIFY(IS_P2ALIGNED(&cfil_stats.cfs_ctl_q_in_peeked,
	    sizeof(uint32_t)));
	VERIFY(IS_P2ALIGNED(&cfil_stats.cfs_ctl_q_out_peeked,
	    sizeof(uint32_t)));

	VERIFY(IS_P2ALIGNED(&cfil_stats.cfs_pending_q_in_enqueued,
	    sizeof(uint32_t)));
	VERIFY(IS_P2ALIGNED(&cfil_stats.cfs_pending_q_out_enqueued,
	    sizeof(uint32_t)));

	VERIFY(IS_P2ALIGNED(&cfil_stats.cfs_inject_q_in_enqueued,
	    sizeof(uint32_t)));
	VERIFY(IS_P2ALIGNED(&cfil_stats.cfs_inject_q_out_enqueued,
	    sizeof(uint32_t)));
	VERIFY(IS_P2ALIGNED(&cfil_stats.cfs_inject_q_in_passed,
	    sizeof(uint32_t)));
	VERIFY(IS_P2ALIGNED(&cfil_stats.cfs_inject_q_out_passed,
	    sizeof(uint32_t)));

	/*
	 * Allocate locks
	 */
	TAILQ_INIT(&cfil_sock_head);
	TAILQ_INIT(&cfil_sock_head_stats);

	/*
	 * Register kernel control
	 */
	bzero(&kern_ctl, sizeof(kern_ctl));
	strlcpy(kern_ctl.ctl_name, CONTENT_FILTER_CONTROL_NAME,
	    sizeof(kern_ctl.ctl_name));
	kern_ctl.ctl_flags = CTL_FLAG_PRIVILEGED | CTL_FLAG_REG_EXTENDED;
	kern_ctl.ctl_sendsize = 512 * 1024; /* enough? */
	kern_ctl.ctl_recvsize = 512 * 1024; /* enough? */
	kern_ctl.ctl_connect = cfil_ctl_connect;
	kern_ctl.ctl_disconnect = cfil_ctl_disconnect;
	kern_ctl.ctl_send = cfil_ctl_send;
	kern_ctl.ctl_getopt = cfil_ctl_getopt;
	kern_ctl.ctl_setopt = cfil_ctl_setopt;
	kern_ctl.ctl_rcvd = cfil_ctl_rcvd;
	error = ctl_register(&kern_ctl, &cfil_kctlref);
	if (error != 0) {
		CFIL_LOG(LOG_ERR, "ctl_register failed: %d", error);
		return;
	}

	// Spawn thread for statistics reporting
	if (kernel_thread_start(cfil_stats_report_thread_func, NULL,
	    &cfil_stats_report_thread) != KERN_SUCCESS) {
		panic_plain("%s: Can't create statistics report thread", __func__);
		/* NOTREACHED */
	}
	/* this must not fail */
	VERIFY(cfil_stats_report_thread != NULL);

	// Set UDP per-flow mbuf thresholds to 1/32 of platform max
	mbuf_limit = MAX(UDP_FLOW_GC_MBUF_CNT_MAX, (nmbclusters << MCLSHIFT) >> UDP_FLOW_GC_MBUF_SHIFT);
	cfil_udp_gc_mbuf_num_max = (mbuf_limit >> MCLSHIFT);
	cfil_udp_gc_mbuf_cnt_max = mbuf_limit;

	memset(&global_cfil_stats_report_buffers, 0, sizeof(global_cfil_stats_report_buffers));
}

struct cfil_info *
cfil_info_alloc(struct socket *so, struct soflow_hash_entry *hash_entry)
{
	int kcunit;
	struct cfil_info *cfil_info = NULL;
	struct inpcb *inp = sotoinpcb(so);

	CFIL_LOG(LOG_INFO, "");

	socket_lock_assert_owned(so);

	cfil_info = zalloc_flags(cfil_info_zone, Z_WAITOK | Z_ZERO | Z_NOFAIL);
	os_ref_init(&cfil_info->cfi_ref_count, &cfil_refgrp);

	cfil_queue_init(&cfil_info->cfi_snd.cfi_inject_q);
	cfil_queue_init(&cfil_info->cfi_rcv.cfi_inject_q);

	microuptime(&cfil_info->cfi_timestamp);

	for (kcunit = 1; kcunit <= MAX_CONTENT_FILTER; kcunit++) {
		struct cfil_entry *entry;

		entry = &cfil_info->cfi_entries[kcunit - 1];
		entry->cfe_cfil_info = cfil_info;

		/* Initialize the filter entry */
		entry->cfe_filter = NULL;
		entry->cfe_flags = 0;
		entry->cfe_necp_control_unit = 0;
		entry->cfe_snd.cfe_pass_offset = 0;
		entry->cfe_snd.cfe_peek_offset = 0;
		entry->cfe_snd.cfe_peeked = 0;
		entry->cfe_rcv.cfe_pass_offset = 0;
		entry->cfe_rcv.cfe_peek_offset = 0;
		entry->cfe_rcv.cfe_peeked = 0;
		/*
		 * Timestamp the last action to avoid pre-maturely
		 * triggering garbage collection
		 */
		microuptime(&entry->cfe_last_action);

		cfil_queue_init(&entry->cfe_snd.cfe_pending_q);
		cfil_queue_init(&entry->cfe_rcv.cfe_pending_q);
		cfil_queue_init(&entry->cfe_snd.cfe_ctl_q);
		cfil_queue_init(&entry->cfe_rcv.cfe_ctl_q);
	}

	cfil_rw_lock_exclusive(&cfil_lck_rw);

	/*
	 * Create a cfi_sock_id that's not the socket pointer!
	 */

	if (hash_entry == NULL) {
		// This is the TCP case, cfil_info is tracked per socket
		if (inp->inp_flowhash == 0) {
			inp_calc_flowhash(inp);
			ASSERT(inp->inp_flowhash != 0);
		}

		so->so_cfil = cfil_info;
		cfil_info->cfi_so = so;
		cfil_info->cfi_sock_id =
		    ((so->so_gencnt << 32) | inp->inp_flowhash);
	} else {
		// This is the UDP case, cfil_info is tracked in per-socket hash
		cfil_info->cfi_so = so;
		cfil_info->cfi_hash_entry = hash_entry;
		cfil_info->cfi_sock_id = ((so->so_gencnt << 32) | (hash_entry->soflow_flowhash & 0xffffffff));
	}

	TAILQ_INSERT_TAIL(&cfil_sock_head, cfil_info, cfi_link);
	SLIST_INIT(&cfil_info->cfi_ordered_entries);

	cfil_sock_attached_count++;

	cfil_rw_unlock_exclusive(&cfil_lck_rw);

	if (cfil_info != NULL) {
		OSIncrementAtomic(&cfil_stats.cfs_cfi_alloc_ok);
	} else {
		OSIncrementAtomic(&cfil_stats.cfs_cfi_alloc_fail);
	}

	return cfil_info;
}

int
cfil_info_attach_unit(struct socket *so, uint32_t filter_control_unit, struct cfil_info *cfil_info)
{
	int kcunit;
	int attached = 0;

	CFIL_LOG(LOG_INFO, "");

	socket_lock_assert_owned(so);

	cfil_rw_lock_exclusive(&cfil_lck_rw);

	for (kcunit = 1; kcunit <= MAX_CONTENT_FILTER; kcunit++) {
		struct content_filter *cfc = content_filters[kcunit - 1];
		struct cfil_entry *entry;
		struct cfil_entry *iter_entry;
		struct cfil_entry *iter_prev;

		if (cfc == NULL) {
			continue;
		}
		if (!(cfc->cf_necp_control_unit & filter_control_unit)) {
			continue;
		}

		entry = &cfil_info->cfi_entries[kcunit - 1];

		entry->cfe_filter = cfc;
		entry->cfe_necp_control_unit = cfc->cf_necp_control_unit;
		TAILQ_INSERT_TAIL(&cfc->cf_sock_entries, entry, cfe_link);
		cfc->cf_sock_count++;

		/* Insert the entry into the list ordered by control unit */
		iter_prev = NULL;
		SLIST_FOREACH(iter_entry, &cfil_info->cfi_ordered_entries, cfe_order_link) {
			if (entry->cfe_necp_control_unit < iter_entry->cfe_necp_control_unit) {
				break;
			}
			iter_prev = iter_entry;
		}

		if (iter_prev == NULL) {
			SLIST_INSERT_HEAD(&cfil_info->cfi_ordered_entries, entry, cfe_order_link);
		} else {
			SLIST_INSERT_AFTER(iter_prev, entry, cfe_order_link);
		}

		verify_content_filter(cfc);
		attached = 1;
		entry->cfe_flags |= CFEF_CFIL_ATTACHED;
	}

	cfil_rw_unlock_exclusive(&cfil_lck_rw);

	return attached;
}

static void
cfil_info_free(struct cfil_info *cfil_info)
{
	int kcunit;
	uint64_t in_drain = 0;
	uint64_t out_drained = 0;

	if (cfil_info == NULL) {
		return;
	}

	CFIL_LOG(LOG_INFO, "");

	cfil_rw_lock_exclusive(&cfil_lck_rw);

	if (cfil_info->cfi_debug) {
		cfil_info_log(LOG_ERR, cfil_info, "CFIL: FREEING CFIL_INFO");
	}

	for (kcunit = 1; kcunit <= MAX_CONTENT_FILTER; kcunit++) {
		struct cfil_entry *entry;
		struct content_filter *cfc;

		entry = &cfil_info->cfi_entries[kcunit - 1];

		/* Don't be silly and try to detach twice */
		if (entry->cfe_filter == NULL) {
			continue;
		}

		cfc = content_filters[kcunit - 1];

		VERIFY(cfc == entry->cfe_filter);

		entry->cfe_filter = NULL;
		entry->cfe_necp_control_unit = 0;
		TAILQ_REMOVE(&cfc->cf_sock_entries, entry, cfe_link);
		cfc->cf_sock_count--;

		verify_content_filter(cfc);
	}

	cfil_sock_attached_count--;
	TAILQ_REMOVE(&cfil_sock_head, cfil_info, cfi_link);

	// Turn off stats reporting for cfil_info.
	cfil_info_stats_toggle(cfil_info, NULL, 0);

	out_drained += cfil_queue_drain(&cfil_info->cfi_snd.cfi_inject_q);
	in_drain += cfil_queue_drain(&cfil_info->cfi_rcv.cfi_inject_q);

	for (kcunit = 1; kcunit <= MAX_CONTENT_FILTER; kcunit++) {
		struct cfil_entry *entry;

		entry = &cfil_info->cfi_entries[kcunit - 1];
		out_drained += cfil_queue_drain(&entry->cfe_snd.cfe_pending_q);
		in_drain += cfil_queue_drain(&entry->cfe_rcv.cfe_pending_q);
		out_drained += cfil_queue_drain(&entry->cfe_snd.cfe_ctl_q);
		in_drain += cfil_queue_drain(&entry->cfe_rcv.cfe_ctl_q);
	}
	cfil_rw_unlock_exclusive(&cfil_lck_rw);

	if (out_drained) {
		OSIncrementAtomic(&cfil_stats.cfs_flush_out_free);
	}
	if (in_drain) {
		OSIncrementAtomic(&cfil_stats.cfs_flush_in_free);
	}

	zfree(cfil_info_zone, cfil_info);
}

/*
 * Received a verdict from userspace for a socket.
 * Perform any delayed operation if needed.
 */
static void
cfil_sock_received_verdict(struct socket *so)
{
	if (so == NULL || so->so_cfil == NULL) {
		return;
	}

	so->so_cfil->cfi_flags |= CFIF_INITIAL_VERDICT;

	/*
	 * If socket has already been connected, trigger
	 * soisconnected now.
	 */
	if (so->so_cfil->cfi_flags & CFIF_SOCKET_CONNECTED) {
		so->so_cfil->cfi_flags &= ~CFIF_SOCKET_CONNECTED;
		soisconnected(so);
		return;
	}
}

/*
 * Entry point from Sockets layer
 * The socket is locked.
 *
 * Checks if a connected socket is subject to filter and
 * pending the initial verdict.
 */
boolean_t
cfil_sock_connected_pending_verdict(struct socket *so)
{
	if (so == NULL || so->so_cfil == NULL) {
		return false;
	}

	if (so->so_cfil->cfi_flags & CFIF_INITIAL_VERDICT) {
		return false;
	} else {
		/*
		 * Remember that this protocol is already connected, so
		 * we will trigger soisconnected() upon receipt of
		 * initial verdict later.
		 */
		so->so_cfil->cfi_flags |= CFIF_SOCKET_CONNECTED;
		return true;
	}
}

/*
 * Entry point from Flow Divert
 * The socket is locked.
 *
 * Mark socket as DEAD if all CFIL data has been processed by filter(s).
 * Otherwise, delay the marking until all data has been processed.
 */
boolean_t
cfil_sock_is_dead(struct socket *so)
{
	struct inpcb *inp = NULL;

	if (so == NULL) {
		return false;
	}

	socket_lock_assert_owned(so);

	if ((so->so_flags & SOF_CONTENT_FILTER) != 0) {
		int32_t pending_snd = cfil_sock_data_pending(&so->so_snd);
		int32_t pending_rcv = cfil_sock_data_pending(&so->so_rcv);
		if (pending_snd || pending_rcv) {
			SO_DELAYED_DEAD_SET(so, true);
			return false;
		}
	}

	inp = sotoinpcb(so);
	if (inp != NULL) {
		SO_DELAYED_DEAD_SET(so, false);
		if (SOCK_CHECK_DOM(so, PF_INET6)) {
			in6_pcbdetach(inp);
		} else {
			in_pcbdetach(inp);
		}
		return true;
	}
	return false;
}

/*
 * Entry point from tcp_timer.c
 * The socket is locked.
 *
 * Perform TCP FIN time wait handling if all CFIL data has been processed by filter(s).
 * Otherwise, delay until all data has been processed.
 */
boolean_t
cfil_sock_tcp_add_time_wait(struct socket *so)
{
	struct inpcb *inp = NULL;
	struct tcpcb *tp = NULL;

	// Only handle TCP sockets
	if (so == NULL || !IS_TCP(so)) {
		return false;
	}

	socket_lock_assert_owned(so);

	if ((so->so_flags & SOF_CONTENT_FILTER) != 0) {
		int32_t pending_snd = cfil_sock_data_pending(&so->so_snd);
		int32_t pending_rcv = cfil_sock_data_pending(&so->so_rcv);
		if (pending_snd || pending_rcv) {
			SO_DELAYED_TCP_TIME_WAIT_SET(so, true);
			return false;
		}
	}

	inp = sotoinpcb(so);
	tp = inp ? intotcpcb(inp) : NULL;
	if (tp != NULL) {
		add_to_time_wait_now(tp, 2 * tcp_msl);
		SO_DELAYED_TCP_TIME_WAIT_SET(so, false);
		return true;
	}
	return false;
}

boolean_t
cfil_filter_present(void)
{
	return cfil_active_count > 0;
}

/*
 * Entry point from Sockets layer
 * The socket is locked.
 */
errno_t
cfil_sock_attach(struct socket *so, struct sockaddr *local, struct sockaddr *remote, int dir)
{
	errno_t error = 0;
	uint32_t filter_control_unit;
	int debug = 0;

	socket_lock_assert_owned(so);

	if (so->so_flags1 & SOF1_FLOW_DIVERT_SKIP) {
		/*
		 * This socket has already been evaluated (and ultimately skipped) by
		 * flow divert, so it has also already been through content filter if there
		 * is one.
		 */
		goto done;
	}

	/* Limit ourselves to TCP that are not MPTCP subflows */
	if (SKIP_FILTER_FOR_TCP_SOCKET(so)) {
		goto done;
	}

	debug = DEBUG_FLOW(sotoinpcb(so), so, local, remote);
	if (debug) {
		CFIL_LOG(LOG_ERR, "CFIL: TCP (dir %d) - debug flow with port %d", dir, cfil_log_port);
	}

	filter_control_unit = necp_socket_get_content_filter_control_unit(so);
	if (filter_control_unit == 0) {
		goto done;
	}

	if (filter_control_unit == NECP_FILTER_UNIT_NO_FILTER) {
		goto done;
	}
	if ((filter_control_unit & NECP_MASK_USERSPACE_ONLY) != 0) {
		OSIncrementAtomic(&cfil_stats.cfs_sock_userspace_only);
		goto done;
	}
	if (cfil_active_count == 0) {
		OSIncrementAtomic(&cfil_stats.cfs_sock_attach_in_vain);
		goto done;
	}
	if (so->so_cfil != NULL) {
		OSIncrementAtomic(&cfil_stats.cfs_sock_attach_already);
		CFIL_LOG(LOG_ERR, "already attached");
		goto done;
	} else {
		cfil_info_alloc(so, NULL);
		if (so->so_cfil == NULL) {
			error = ENOMEM;
			OSIncrementAtomic(&cfil_stats.cfs_sock_attach_no_mem);
			goto done;
		}
		so->so_cfil->cfi_dir = dir;
		so->so_cfil->cfi_filter_control_unit = filter_control_unit;
		so->so_cfil->cfi_filter_policy_gencount = necp_socket_get_policy_gencount(so);
		so->so_cfil->cfi_debug = debug;
	}
	if (cfil_info_attach_unit(so, filter_control_unit, so->so_cfil) == 0) {
		CFIL_LOG(LOG_ERR, "cfil_info_attach_unit(%u) failed",
		    filter_control_unit);
		OSIncrementAtomic(&cfil_stats.cfs_sock_attach_failed);
		goto done;
	}
	CFIL_LOG(LOG_INFO, "so %llx filter_control_unit %u sockID %llu <%llx>",
	    (uint64_t)VM_KERNEL_ADDRPERM(so),
	    filter_control_unit, so->so_cfil->cfi_sock_id, so->so_cfil->cfi_sock_id);

	so->so_flags |= SOF_CONTENT_FILTER;
	OSIncrementAtomic(&cfil_stats.cfs_sock_attached);

	/* Hold a reference on the socket */
	so->so_usecount++;

	/*
	 * Save passed addresses for attach event msg (in case resend
	 * is needed.
	 */
	if (remote != NULL && (remote->sa_len <= sizeof(union sockaddr_in_4_6))) {
		SOCKADDR_COPY(remote, SA(&so->so_cfil->cfi_so_attach_faddr), remote->sa_len);
	}
	if (local != NULL && (local->sa_len <= sizeof(union sockaddr_in_4_6))) {
		SOCKADDR_COPY(local, SA(&so->so_cfil->cfi_so_attach_laddr), local->sa_len);
	}

	if (so->so_cfil->cfi_debug) {
		cfil_info_log(LOG_ERR, so->so_cfil, "CFIL: ADDED");
	}

	error = cfil_dispatch_attach_event(so, so->so_cfil, 0, dir);
	/* We can recover from flow control or out of memory errors */
	if (error == ENOBUFS || error == ENOMEM) {
		error = 0;
	} else if (error != 0) {
		goto done;
	}

	CFIL_INFO_VERIFY(so->so_cfil);
done:
	return error;
}

/*
 * Entry point from Sockets layer
 * The socket is locked.
 */
errno_t
cfil_sock_detach(struct socket *so)
{
	if (NEED_DGRAM_FLOW_TRACKING(so)) {
		return 0;
	}

	if (so->so_cfil) {
		if (so->so_flags & SOF_CONTENT_FILTER) {
			so->so_flags &= ~SOF_CONTENT_FILTER;
			VERIFY(so->so_usecount > 0);
			so->so_usecount--;
		}
		CFIL_INFO_FREE(so->so_cfil);
		so->so_cfil = NULL;
		OSIncrementAtomic(&cfil_stats.cfs_sock_detached);
	}
	return 0;
}

/*
 * Fill in the address info of an event message from either
 * the socket or passed in address info.
 */
static void
cfil_fill_event_msg_addresses(struct soflow_hash_entry *entry, struct inpcb *inp,
    union sockaddr_in_4_6 *sin_src, union sockaddr_in_4_6 *sin_dst,
    boolean_t isIPv4, boolean_t outgoing)
{
	if (isIPv4) {
		struct in_addr laddr = {0}, faddr = {0};
		u_int16_t lport = 0, fport = 0;

		cfil_get_flow_address(entry, inp, &laddr, &faddr, &lport, &fport);

		if (outgoing) {
			fill_ip_sockaddr_4_6(sin_src, laddr, lport);
			fill_ip_sockaddr_4_6(sin_dst, faddr, fport);
		} else {
			fill_ip_sockaddr_4_6(sin_src, faddr, fport);
			fill_ip_sockaddr_4_6(sin_dst, laddr, lport);
		}
	} else {
		struct in6_addr * __single laddr = NULL, * __single faddr = NULL;
		u_int16_t lport = 0, fport = 0;
		const u_int32_t lifscope = inp ? inp->inp_lifscope : IFSCOPE_UNKNOWN;
		const u_int32_t fifscope = inp ? inp->inp_fifscope : IFSCOPE_UNKNOWN;

		cfil_get_flow_address_v6(entry, inp, &laddr, &faddr, &lport, &fport);
		if (outgoing) {
			fill_ip6_sockaddr_4_6(sin_src, laddr, lport, lifscope);
			fill_ip6_sockaddr_4_6(sin_dst, faddr, fport, fifscope);
		} else {
			fill_ip6_sockaddr_4_6(sin_src, faddr, fport, fifscope);
			fill_ip6_sockaddr_4_6(sin_dst, laddr, lport, lifscope);
		}
	}
}

static boolean_t
cfil_dispatch_attach_event_sign(cfil_crypto_state_t crypto_state,
    struct cfil_info *cfil_info,
    struct cfil_msg_sock_attached *msg)
{
	struct cfil_crypto_data data = {};
	struct iovec extra_data[1] = { { NULL, 0 } };

	if (crypto_state == NULL || msg == NULL || cfil_info == NULL) {
		return false;
	}

	data.sock_id = msg->cfs_msghdr.cfm_sock_id;
	data.direction = msg->cfs_conn_dir;

	data.pid = msg->cfs_pid;
	data.effective_pid = msg->cfs_e_pid;
	data.responsible_pid = msg->cfs_r_pid;
	uuid_copy(data.uuid, msg->cfs_uuid);
	uuid_copy(data.effective_uuid, msg->cfs_e_uuid);
	uuid_copy(data.responsible_uuid, msg->cfs_r_uuid);
	data.socketProtocol = msg->cfs_sock_protocol;
	if (data.direction == CFS_CONNECTION_DIR_OUT) {
		data.remote.sin6 = msg->cfs_dst.sin6;
		data.local.sin6 = msg->cfs_src.sin6;
	} else {
		data.remote.sin6 = msg->cfs_src.sin6;
		data.local.sin6 = msg->cfs_dst.sin6;
	}

	size_t len = strbuflen(msg->cfs_remote_domain_name, sizeof(msg->cfs_remote_domain_name));
	if (len > 0) {
		extra_data[0].iov_base = msg->cfs_remote_domain_name;
		extra_data[0].iov_len = len;
	}

	// At attach, if local address is already present, no need to re-sign subsequent data messages.
	if (!NULLADDRESS(data.local)) {
		cfil_info->cfi_isSignatureLatest = true;
	}

	msg->cfs_signature_length = sizeof(cfil_crypto_signature);
	if (cfil_crypto_sign_data(crypto_state, &data, extra_data, sizeof(extra_data) / sizeof(extra_data[0]), msg->cfs_signature, &msg->cfs_signature_length) != 0) {
		msg->cfs_signature_length = 0;
		CFIL_LOG(LOG_ERR, "CFIL: Failed to sign attached msg <sockID %llu <%llx>>",
		    msg->cfs_msghdr.cfm_sock_id, msg->cfs_msghdr.cfm_sock_id);
		return false;
	}

	return true;
}

struct cfil_sign_parameters {
	cfil_crypto_state_t csp_state;
	struct cfil_crypto_data *csp_data;
	uint8_t * __indexable csp_signature;
	uint32_t *csp_signature_size;
};

static void
cfil_sign_with_domain_name(char *domain_name __null_terminated, void *ctx)
{
	struct cfil_sign_parameters *parameters = (struct cfil_sign_parameters *)ctx;
	struct iovec extra_data[1] = { { NULL, 0 } };

	if (parameters == NULL) {
		return;
	}

	if (domain_name != NULL) {
		extra_data[0].iov_base = __unsafe_null_terminated_to_indexable(domain_name);
		extra_data[0].iov_len = strlen(domain_name);
	}

	*(parameters->csp_signature_size) = sizeof(cfil_crypto_signature);
	if (cfil_crypto_sign_data(parameters->csp_state, parameters->csp_data,
	    extra_data, sizeof(extra_data) / sizeof(extra_data[0]),
	    parameters->csp_signature, parameters->csp_signature_size) != 0) {
		*(parameters->csp_signature_size) = 0;
	}
}

static boolean_t
cfil_dispatch_data_event_sign(cfil_crypto_state_t crypto_state,
    struct socket *so, struct cfil_info *cfil_info,
    struct cfil_msg_data_event *msg)
{
	struct cfil_crypto_data data = {};

	if (crypto_state == NULL || msg == NULL ||
	    so == NULL || cfil_info == NULL) {
		return false;
	}

	data.sock_id = cfil_info->cfi_sock_id;
	data.direction = cfil_info->cfi_dir;
	data.pid = so->last_pid;
	memcpy(data.uuid, so->last_uuid, sizeof(uuid_t));
	if (so->so_flags & SOF_DELEGATED) {
		data.effective_pid = so->e_pid;
		memcpy(data.effective_uuid, so->e_uuid, sizeof(uuid_t));
	} else {
		data.effective_pid = so->last_pid;
		memcpy(data.effective_uuid, so->last_uuid, sizeof(uuid_t));
	}
#if defined(XNU_TARGET_OS_OSX)
	if (!uuid_is_null(so->so_ruuid)) {
		data.responsible_pid = so->so_rpid;
		memcpy(data.responsible_uuid, so->so_ruuid, sizeof(uuid_t));
	}
#endif
	data.socketProtocol = GET_SO_PROTO(so);

	if (data.direction == CFS_CONNECTION_DIR_OUT) {
		data.remote.sin6 = msg->cfc_dst.sin6;
		data.local.sin6 = msg->cfc_src.sin6;
	} else {
		data.remote.sin6 = msg->cfc_src.sin6;
		data.local.sin6 = msg->cfc_dst.sin6;
	}

	// At first data, local address may show up for the first time, update address cache and
	// no need to re-sign subsequent data messages anymore.
	if (!NULLADDRESS(data.local)) {
		memcpy(&cfil_info->cfi_so_attach_laddr, &data.local, data.local.sa.sa_len);
		cfil_info->cfi_isSignatureLatest = true;
	}

	struct cfil_sign_parameters parameters = {
		.csp_state = crypto_state,
		.csp_data = &data,
		.csp_signature = msg->cfd_signature,
		.csp_signature_size = &msg->cfd_signature_length,
	};
	necp_with_inp_domain_name(so, &parameters, cfil_sign_with_domain_name);

	if (msg->cfd_signature_length == 0) {
		CFIL_LOG(LOG_ERR, "CFIL: Failed to sign data msg <sockID %llu <%llx>>",
		    msg->cfd_msghdr.cfm_sock_id, msg->cfd_msghdr.cfm_sock_id);
		return false;
	}

	return true;
}

static boolean_t
cfil_dispatch_closed_event_sign(cfil_crypto_state_t crypto_state,
    struct socket *so, struct cfil_info *cfil_info,
    struct cfil_msg_sock_closed *msg)
{
	struct cfil_crypto_data data = {};
	struct soflow_hash_entry hash_entry = {};
	struct soflow_hash_entry *hash_entry_ptr = NULL;
	struct inpcb *inp = (struct inpcb *)so->so_pcb;

	if (crypto_state == NULL || msg == NULL ||
	    so == NULL || inp == NULL || cfil_info == NULL) {
		return false;
	}

	data.sock_id = cfil_info->cfi_sock_id;
	data.direction = cfil_info->cfi_dir;

	data.pid = so->last_pid;
	memcpy(data.uuid, so->last_uuid, sizeof(uuid_t));
	if (so->so_flags & SOF_DELEGATED) {
		data.effective_pid = so->e_pid;
		memcpy(data.effective_uuid, so->e_uuid, sizeof(uuid_t));
	} else {
		data.effective_pid = so->last_pid;
		memcpy(data.effective_uuid, so->last_uuid, sizeof(uuid_t));
	}
#if defined(XNU_TARGET_OS_OSX)
	if (!uuid_is_null(so->so_ruuid)) {
		data.responsible_pid = so->so_rpid;
		memcpy(data.responsible_uuid, so->so_ruuid, sizeof(uuid_t));
	}
#endif
	data.socketProtocol = GET_SO_PROTO(so);

	/*
	 * Fill in address info:
	 * For UDP, use the cfil_info hash entry directly.
	 * For TCP, compose an hash entry with the saved addresses.
	 */
	if (cfil_info->cfi_hash_entry != NULL) {
		hash_entry_ptr = cfil_info->cfi_hash_entry;
	} else if (cfil_info->cfi_so_attach_faddr.sa.sa_len > 0 ||
	    cfil_info->cfi_so_attach_laddr.sa.sa_len > 0) {
		soflow_fill_hash_entry_from_address(&hash_entry, TRUE, SA(&cfil_info->cfi_so_attach_laddr), FALSE);
		soflow_fill_hash_entry_from_address(&hash_entry, FALSE, SA(&cfil_info->cfi_so_attach_faddr), FALSE);
		hash_entry_ptr = &hash_entry;
	}
	if (hash_entry_ptr != NULL) {
		boolean_t outgoing = (cfil_info->cfi_dir == CFS_CONNECTION_DIR_OUT);
		union sockaddr_in_4_6 *src = outgoing ? &data.local : &data.remote;
		union sockaddr_in_4_6 *dst = outgoing ? &data.remote : &data.local;
		cfil_fill_event_msg_addresses(hash_entry_ptr, inp, src, dst, !IS_INP_V6(inp), outgoing);
	}

	data.byte_count_in = cfil_info->cfi_byte_inbound_count;
	data.byte_count_out = cfil_info->cfi_byte_outbound_count;

	struct cfil_sign_parameters parameters = {
		.csp_state = crypto_state,
		.csp_data = &data,
		.csp_signature = msg->cfc_signature,
		.csp_signature_size = &msg->cfc_signature_length
	};
	necp_with_inp_domain_name(so, &parameters, cfil_sign_with_domain_name);

	if (msg->cfc_signature_length == 0) {
		CFIL_LOG(LOG_ERR, "CFIL: Failed to sign closed msg <sockID %llu <%llx>>",
		    msg->cfc_msghdr.cfm_sock_id, msg->cfc_msghdr.cfm_sock_id);
		return false;
	}

	return true;
}

static void
cfil_populate_attached_msg_domain_name(char *domain_name __null_terminated, void *ctx)
{
	struct cfil_msg_sock_attached *msg_attached = (struct cfil_msg_sock_attached *)ctx;

	if (msg_attached == NULL) {
		return;
	}

	if (domain_name != NULL) {
		strlcpy(msg_attached->cfs_remote_domain_name, domain_name, sizeof(msg_attached->cfs_remote_domain_name));
	}
}

static bool
cfil_copy_audit_token(pid_t pid, audit_token_t *buffer)
{
	bool success = false;
	proc_t p = proc_find(pid);
	if (p != PROC_NULL) {
		task_t __single t = proc_task(p);
		if (t != TASK_NULL) {
			audit_token_t audit_token = {};
			mach_msg_type_number_t count = TASK_AUDIT_TOKEN_COUNT;
			if (task_info(t, TASK_AUDIT_TOKEN, (task_info_t)&audit_token, &count) == KERN_SUCCESS) {
				memcpy(buffer, &audit_token, sizeof(audit_token_t));
				success = true;
			}
		}
		proc_rele(p);
	}
	return success;
}

static int
cfil_dispatch_attach_event(struct socket *so, struct cfil_info *cfil_info,
    uint32_t kcunit, int conn_dir)
{
	errno_t error = 0;
	struct cfil_entry *entry = NULL;
	struct cfil_msg_sock_attached * __single msg_attached;
	struct content_filter *cfc = NULL;
	struct inpcb *inp = (struct inpcb *)so->so_pcb;
	struct soflow_hash_entry *hash_entry_ptr = NULL;
	struct soflow_hash_entry hash_entry;

	memset(&hash_entry, 0, sizeof(struct soflow_hash_entry));

	socket_lock_assert_owned(so);

	cfil_rw_lock_shared(&cfil_lck_rw);

	if (so->so_proto == NULL || so->so_proto->pr_domain == NULL) {
		error = EINVAL;
		goto done;
	}

	if (kcunit == 0) {
		entry = SLIST_FIRST(&cfil_info->cfi_ordered_entries);
	} else {
		entry = &cfil_info->cfi_entries[kcunit - 1];
	}

	if (entry == NULL) {
		goto done;
	}

	cfc = entry->cfe_filter;
	if (cfc == NULL) {
		goto done;
	}

	if ((entry->cfe_flags & CFEF_SENT_SOCK_ATTACHED)) {
		goto done;
	}

	if (kcunit == 0) {
		kcunit = CFI_ENTRY_KCUNIT(cfil_info, entry);
	}

	CFIL_LOG(LOG_INFO, "so %llx filter_control_unit %u kcunit %u",
	    (uint64_t)VM_KERNEL_ADDRPERM(so), entry->cfe_necp_control_unit, kcunit);

	/* Would be wasteful to try when flow controlled */
	if (cfc->cf_flags & CFF_FLOW_CONTROLLED) {
		error = ENOBUFS;
		goto done;
	}

	msg_attached = kalloc_data(sizeof(struct cfil_msg_sock_attached), Z_WAITOK);
	if (msg_attached == NULL) {
		error = ENOMEM;
		goto done;
	}

	bzero(msg_attached, sizeof(struct cfil_msg_sock_attached));
	msg_attached->cfs_msghdr.cfm_len = sizeof(struct cfil_msg_sock_attached);
	msg_attached->cfs_msghdr.cfm_version = CFM_VERSION_CURRENT;
	msg_attached->cfs_msghdr.cfm_type = CFM_TYPE_EVENT;
	msg_attached->cfs_msghdr.cfm_op = CFM_OP_SOCKET_ATTACHED;
	msg_attached->cfs_msghdr.cfm_sock_id = entry->cfe_cfil_info->cfi_sock_id;

	msg_attached->cfs_sock_family = SOCK_DOM(so);
	msg_attached->cfs_sock_type = SOCK_TYPE(so);
	msg_attached->cfs_sock_protocol = GET_SO_PROTO(so);
	msg_attached->cfs_pid = so->last_pid;
	memcpy(msg_attached->cfs_uuid, so->last_uuid, sizeof(uuid_t));
	if (so->so_flags & SOF_DELEGATED) {
		msg_attached->cfs_e_pid = so->e_pid;
		memcpy(msg_attached->cfs_e_uuid, so->e_uuid, sizeof(uuid_t));
	} else {
		msg_attached->cfs_e_pid = so->last_pid;
		memcpy(msg_attached->cfs_e_uuid, so->last_uuid, sizeof(uuid_t));
	}
#if defined(XNU_TARGET_OS_OSX)
	if (!uuid_is_null(so->so_ruuid)) {
		msg_attached->cfs_r_pid = so->so_rpid;
		memcpy(msg_attached->cfs_r_uuid, so->so_ruuid, sizeof(uuid_t));
	}
#endif
	/*
	 * Fill in address info:
	 * For UDP, use the cfil_info hash entry directly.
	 * For TCP, compose an hash entry with the saved addresses.
	 */
	if (cfil_info->cfi_hash_entry != NULL) {
		hash_entry_ptr = cfil_info->cfi_hash_entry;
	} else if (cfil_info->cfi_so_attach_faddr.sa.sa_len > 0 ||
	    cfil_info->cfi_so_attach_laddr.sa.sa_len > 0) {
		soflow_fill_hash_entry_from_address(&hash_entry, TRUE, SA(&cfil_info->cfi_so_attach_laddr), FALSE);
		soflow_fill_hash_entry_from_address(&hash_entry, FALSE, SA(&cfil_info->cfi_so_attach_faddr), FALSE);
		hash_entry_ptr = &hash_entry;
	}
	if (hash_entry_ptr != NULL) {
		cfil_fill_event_msg_addresses(hash_entry_ptr, inp,
		    &msg_attached->cfs_src, &msg_attached->cfs_dst,
		    !IS_INP_V6(inp), conn_dir == CFS_CONNECTION_DIR_OUT);
	}
	msg_attached->cfs_conn_dir = conn_dir;

	if (msg_attached->cfs_e_pid != 0) {
		if (!cfil_copy_audit_token(msg_attached->cfs_e_pid, (audit_token_t *)&msg_attached->cfs_audit_token)) {
			CFIL_LOG(LOG_ERR, "CFIL: Failed to get effective audit token for <sockID %llu <%llx>> ",
			    entry->cfe_cfil_info->cfi_sock_id, entry->cfe_cfil_info->cfi_sock_id);
		}
	}

	if (msg_attached->cfs_pid != 0) {
		if (msg_attached->cfs_pid == msg_attached->cfs_e_pid) {
			memcpy(&msg_attached->cfs_real_audit_token, &msg_attached->cfs_audit_token, sizeof(msg_attached->cfs_real_audit_token));
		} else if (!cfil_copy_audit_token(msg_attached->cfs_pid, (audit_token_t *)&msg_attached->cfs_real_audit_token)) {
			CFIL_LOG(LOG_ERR, "CFIL: Failed to get real audit token for <sockID %llu <%llx>> ",
			    entry->cfe_cfil_info->cfi_sock_id, entry->cfe_cfil_info->cfi_sock_id);
		}
	}

	necp_with_inp_domain_name(so, msg_attached, cfil_populate_attached_msg_domain_name);

	if (cfil_info->cfi_debug) {
		cfil_info_log(LOG_ERR, cfil_info, "CFIL: SENDING ATTACH UP");
	}

	cfil_dispatch_attach_event_sign(entry->cfe_filter->cf_crypto_state, cfil_info, msg_attached);

	error = ctl_enqueuedata(entry->cfe_filter->cf_kcref,
	    entry->cfe_filter->cf_kcunit,
	    msg_attached,
	    sizeof(struct cfil_msg_sock_attached),
	    CTL_DATA_EOR);

	kfree_data(msg_attached, sizeof(struct cfil_msg_sock_attached));

	if (error != 0) {
		CFIL_LOG(LOG_ERR, "ctl_enqueuedata() failed: %d", error);
		goto done;
	}
	microuptime(&entry->cfe_last_event);
	cfil_info->cfi_first_event.tv_sec = entry->cfe_last_event.tv_sec;
	cfil_info->cfi_first_event.tv_usec = entry->cfe_last_event.tv_usec;

	entry->cfe_flags |= CFEF_SENT_SOCK_ATTACHED;
	OSIncrementAtomic(&cfil_stats.cfs_attach_event_ok);
done:

	/* We can recover from flow control */
	if (error == ENOBUFS) {
		entry->cfe_flags |= CFEF_FLOW_CONTROLLED;
		OSIncrementAtomic(&cfil_stats.cfs_attach_event_flow_control);

		if (!cfil_rw_lock_shared_to_exclusive(&cfil_lck_rw)) {
			cfil_rw_lock_exclusive(&cfil_lck_rw);
		}

		cfc->cf_flags |= CFF_FLOW_CONTROLLED;

		cfil_rw_unlock_exclusive(&cfil_lck_rw);
	} else {
		if (error != 0) {
			OSIncrementAtomic(&cfil_stats.cfs_attach_event_fail);
		}

		cfil_rw_unlock_shared(&cfil_lck_rw);
	}
	return error;
}

static int
cfil_dispatch_disconnect_event(struct socket *so, struct cfil_info *cfil_info, uint32_t kcunit, int outgoing)
{
	errno_t error = 0;
	struct mbuf *msg = NULL;
	struct cfil_entry *entry;
	struct cfe_buf *entrybuf;
	struct cfil_msg_hdr msg_disconnected;
	struct content_filter *cfc;

	socket_lock_assert_owned(so);

	cfil_rw_lock_shared(&cfil_lck_rw);

	entry = &cfil_info->cfi_entries[kcunit - 1];
	if (outgoing) {
		entrybuf = &entry->cfe_snd;
	} else {
		entrybuf = &entry->cfe_rcv;
	}

	cfc = entry->cfe_filter;
	if (cfc == NULL) {
		goto done;
	}

	// Mark if this flow qualifies for immediate close.
	SET_NO_CLOSE_WAIT(sotoinpcb(so), cfil_info);

	CFIL_LOG(LOG_INFO, "so %llx kcunit %u outgoing %d",
	    (uint64_t)VM_KERNEL_ADDRPERM(so), kcunit, outgoing);

	/*
	 * Send the disconnection event once
	 */
	if ((outgoing && (entry->cfe_flags & CFEF_SENT_DISCONNECT_OUT)) ||
	    (!outgoing && (entry->cfe_flags & CFEF_SENT_DISCONNECT_IN))) {
		CFIL_LOG(LOG_INFO, "so %llx disconnect already sent",
		    (uint64_t)VM_KERNEL_ADDRPERM(so));
		goto done;
	}

	/*
	 * We're not disconnected as long as some data is waiting
	 * to be delivered to the filter
	 */
	if (outgoing && cfil_queue_empty(&entrybuf->cfe_ctl_q) == 0) {
		CFIL_LOG(LOG_INFO, "so %llx control queue not empty",
		    (uint64_t)VM_KERNEL_ADDRPERM(so));
		error = EBUSY;
		goto done;
	}
	/* Would be wasteful to try when flow controlled */
	if (cfc->cf_flags & CFF_FLOW_CONTROLLED) {
		error = ENOBUFS;
		goto done;
	}

	if (cfil_info->cfi_debug) {
		const char * __null_terminated out = "CFIL: OUT - SENDING DISCONNECT UP";
		const char * __null_terminated in = "CFIL: IN - SENDING DISCONNECT UP";
		cfil_info_log(LOG_ERR, cfil_info, outgoing ? out : in);
	}

	bzero(&msg_disconnected, sizeof(struct cfil_msg_hdr));
	msg_disconnected.cfm_len = sizeof(struct cfil_msg_hdr);
	msg_disconnected.cfm_version = CFM_VERSION_CURRENT;
	msg_disconnected.cfm_type = CFM_TYPE_EVENT;
	msg_disconnected.cfm_op = outgoing ? CFM_OP_DISCONNECT_OUT :
	    CFM_OP_DISCONNECT_IN;
	msg_disconnected.cfm_sock_id = entry->cfe_cfil_info->cfi_sock_id;
	error = ctl_enqueuedata(entry->cfe_filter->cf_kcref,
	    entry->cfe_filter->cf_kcunit,
	    &msg_disconnected,
	    sizeof(struct cfil_msg_hdr),
	    CTL_DATA_EOR);
	if (error != 0) {
		CFIL_LOG(LOG_ERR, "ctl_enqueuembuf() failed: %d", error);
		mbuf_freem(msg);
		goto done;
	}
	microuptime(&entry->cfe_last_event);
	CFI_ADD_TIME_LOG(cfil_info, &entry->cfe_last_event, &cfil_info->cfi_first_event, msg_disconnected.cfm_op);

	/* Remember we have sent the disconnection message */
	if (outgoing) {
		entry->cfe_flags |= CFEF_SENT_DISCONNECT_OUT;
		OSIncrementAtomic(&cfil_stats.cfs_disconnect_out_event_ok);
	} else {
		entry->cfe_flags |= CFEF_SENT_DISCONNECT_IN;
		OSIncrementAtomic(&cfil_stats.cfs_disconnect_in_event_ok);
	}
done:
	if (error == ENOBUFS) {
		entry->cfe_flags |= CFEF_FLOW_CONTROLLED;
		OSIncrementAtomic(
			&cfil_stats.cfs_disconnect_event_flow_control);

		if (!cfil_rw_lock_shared_to_exclusive(&cfil_lck_rw)) {
			cfil_rw_lock_exclusive(&cfil_lck_rw);
		}

		cfc->cf_flags |= CFF_FLOW_CONTROLLED;

		cfil_rw_unlock_exclusive(&cfil_lck_rw);
	} else {
		if (error != 0) {
			OSIncrementAtomic(
				&cfil_stats.cfs_disconnect_event_fail);
		}

		cfil_rw_unlock_shared(&cfil_lck_rw);
	}
	return error;
}

int
cfil_dispatch_closed_event(struct socket *so, struct cfil_info *cfil_info, int kcunit)
{
	struct cfil_entry *entry;
	struct cfil_msg_sock_closed msg_closed;
	errno_t error = 0;
	struct content_filter *cfc;
	struct inpcb *inp = NULL;

	socket_lock_assert_owned(so);

	cfil_rw_lock_shared(&cfil_lck_rw);

	entry = &cfil_info->cfi_entries[kcunit - 1];
	cfc = entry->cfe_filter;
	if (cfc == NULL) {
		goto done;
	}

	CFIL_LOG(LOG_INFO, "so %llx kcunit %d",
	    (uint64_t)VM_KERNEL_ADDRPERM(so), kcunit);

	/* Would be wasteful to try when flow controlled */
	if (cfc->cf_flags & CFF_FLOW_CONTROLLED) {
		error = ENOBUFS;
		goto done;
	}
	/*
	 * Send a single closed message per filter
	 */
	if ((entry->cfe_flags & CFEF_SENT_SOCK_CLOSED) != 0) {
		goto done;
	}
	if ((entry->cfe_flags & CFEF_SENT_SOCK_ATTACHED) == 0) {
		goto done;
	}

	microuptime(&entry->cfe_last_event);
	CFI_ADD_TIME_LOG(cfil_info, &entry->cfe_last_event, &cfil_info->cfi_first_event, CFM_OP_SOCKET_CLOSED);

	bzero(&msg_closed, sizeof(struct cfil_msg_sock_closed));
	msg_closed.cfc_msghdr.cfm_len = sizeof(struct cfil_msg_sock_closed);
	msg_closed.cfc_msghdr.cfm_version = CFM_VERSION_CURRENT;
	msg_closed.cfc_msghdr.cfm_type = CFM_TYPE_EVENT;
	msg_closed.cfc_msghdr.cfm_op = CFM_OP_SOCKET_CLOSED;
	msg_closed.cfc_msghdr.cfm_sock_id = entry->cfe_cfil_info->cfi_sock_id;
	msg_closed.cfc_first_event.tv_sec = cfil_info->cfi_first_event.tv_sec;
	msg_closed.cfc_first_event.tv_usec = cfil_info->cfi_first_event.tv_usec;
	memcpy(msg_closed.cfc_op_time, cfil_info->cfi_op_time, sizeof(uint32_t) * CFI_MAX_TIME_LOG_ENTRY);
	memcpy(msg_closed.cfc_op_list, cfil_info->cfi_op_list, sizeof(unsigned char) * CFI_MAX_TIME_LOG_ENTRY);
	msg_closed.cfc_op_list_ctr = cfil_info->cfi_op_list_ctr;
	msg_closed.cfc_byte_inbound_count = cfil_info->cfi_byte_inbound_count;
	msg_closed.cfc_byte_outbound_count = cfil_info->cfi_byte_outbound_count;

	if (entry->cfe_laddr_sent == false) {
		/* cache it if necessary */
		if (cfil_info->cfi_so_attach_laddr.sa.sa_len == 0) {
			inp = cfil_info->cfi_so ? sotoinpcb(cfil_info->cfi_so) : NULL;
			if (inp != NULL) {
				boolean_t outgoing = (cfil_info->cfi_dir == CFS_CONNECTION_DIR_OUT);
				union sockaddr_in_4_6 *src = outgoing ? &cfil_info->cfi_so_attach_laddr : NULL;
				union sockaddr_in_4_6 *dst = outgoing ? NULL : &cfil_info->cfi_so_attach_laddr;
				cfil_fill_event_msg_addresses(cfil_info->cfi_hash_entry, inp,
				    src, dst, !IS_INP_V6(inp), outgoing);
			}
		}

		if (cfil_info->cfi_so_attach_laddr.sa.sa_len != 0) {
			msg_closed.cfc_laddr.sin6 = cfil_info->cfi_so_attach_laddr.sin6;
			entry->cfe_laddr_sent = true;
		}
	}

	cfil_dispatch_closed_event_sign(entry->cfe_filter->cf_crypto_state, so, cfil_info, &msg_closed);

	if (cfil_info->cfi_debug) {
		cfil_info_log(LOG_ERR, cfil_info, "CFIL: SENDING CLOSED UP");
	}

	/* for debugging
	 *  if (msg_closed.cfc_op_list_ctr > CFI_MAX_TIME_LOG_ENTRY) {
	 *       msg_closed.cfc_op_list_ctr  = CFI_MAX_TIME_LOG_ENTRY;       // just in case
	 *  }
	 *  for (unsigned int i = 0; i < msg_closed.cfc_op_list_ctr ; i++) {
	 *       CFIL_LOG(LOG_ERR, "MD: socket %llu event %2u, time + %u msec", msg_closed.cfc_msghdr.cfm_sock_id, (unsigned short)msg_closed.cfc_op_list[i], msg_closed.cfc_op_time[i]);
	 *  }
	 */

	error = ctl_enqueuedata(entry->cfe_filter->cf_kcref,
	    entry->cfe_filter->cf_kcunit,
	    &msg_closed,
	    sizeof(struct cfil_msg_sock_closed),
	    CTL_DATA_EOR);
	if (error != 0) {
		CFIL_LOG(LOG_ERR, "ctl_enqueuedata() failed: %d",
		    error);
		goto done;
	}

	entry->cfe_flags |= CFEF_SENT_SOCK_CLOSED;
	OSIncrementAtomic(&cfil_stats.cfs_closed_event_ok);
done:
	/* We can recover from flow control */
	if (error == ENOBUFS) {
		entry->cfe_flags |= CFEF_FLOW_CONTROLLED;
		OSIncrementAtomic(&cfil_stats.cfs_closed_event_flow_control);

		if (!cfil_rw_lock_shared_to_exclusive(&cfil_lck_rw)) {
			cfil_rw_lock_exclusive(&cfil_lck_rw);
		}

		cfc->cf_flags |= CFF_FLOW_CONTROLLED;

		cfil_rw_unlock_exclusive(&cfil_lck_rw);
	} else {
		if (error != 0) {
			OSIncrementAtomic(&cfil_stats.cfs_closed_event_fail);
		}

		cfil_rw_unlock_shared(&cfil_lck_rw);
	}

	return error;
}

static void
fill_ip6_sockaddr_4_6(union sockaddr_in_4_6 *sin46,
    struct in6_addr *ip6, u_int16_t port, uint32_t ifscope)
{
	if (sin46 == NULL) {
		return;
	}

	struct sockaddr_in6 *sin6 = &sin46->sin6;

	sin6->sin6_family = AF_INET6;
	sin6->sin6_len = sizeof(*sin6);
	sin6->sin6_port = port;
	sin6->sin6_addr = *ip6;
	if (IN6_IS_SCOPE_EMBED(&sin6->sin6_addr)) {
		sin6->sin6_scope_id = ifscope;
		if (in6_embedded_scope) {
			in6_verify_ifscope(&sin6->sin6_addr, sin6->sin6_scope_id);
			if (sin6->sin6_addr.s6_addr16[1] != 0) {
				sin6->sin6_scope_id = ntohs(sin6->sin6_addr.s6_addr16[1]);
				sin6->sin6_addr.s6_addr16[1] = 0;
			}
		}
	}
}

static void
fill_ip_sockaddr_4_6(union sockaddr_in_4_6 *sin46,
    struct in_addr ip, u_int16_t port)
{
	if (sin46 == NULL) {
		return;
	}

	struct sockaddr_in *sin = &sin46->sin;

	sin->sin_family = AF_INET;
	sin->sin_len = sizeof(*sin);
	sin->sin_port = port;
	sin->sin_addr.s_addr = ip.s_addr;
}

static void
cfil_get_flow_address_v6(struct soflow_hash_entry *entry, struct inpcb *inp,
    struct in6_addr **laddr, struct in6_addr **faddr,
    u_int16_t *lport, u_int16_t *fport)
{
	if (entry != NULL) {
		*laddr = &entry->soflow_laddr.addr6;
		*faddr = &entry->soflow_faddr.addr6;
		*lport = entry->soflow_lport;
		*fport = entry->soflow_fport;
	} else {
		*laddr = &inp->in6p_laddr;
		*faddr = &inp->in6p_faddr;
		*lport = inp->inp_lport;
		*fport = inp->inp_fport;
	}
}

static void
cfil_get_flow_address(struct soflow_hash_entry *entry, struct inpcb *inp,
    struct in_addr *laddr, struct in_addr *faddr,
    u_int16_t *lport, u_int16_t *fport)
{
	if (entry != NULL) {
		*laddr = entry->soflow_laddr.addr46.ia46_addr4;
		*faddr = entry->soflow_faddr.addr46.ia46_addr4;
		*lport = entry->soflow_lport;
		*fport = entry->soflow_fport;
	} else {
		*laddr = inp->inp_laddr;
		*faddr = inp->inp_faddr;
		*lport = inp->inp_lport;
		*fport = inp->inp_fport;
	}
}

static int
cfil_dispatch_data_event(struct socket *so, struct cfil_info *cfil_info, uint32_t kcunit, int outgoing,
    struct mbuf *data, unsigned int copyoffset, unsigned int copylen)
{
	errno_t error = 0;
	struct mbuf *copy = NULL;
	struct mbuf * __single msg = NULL;
	unsigned int one = 1;
	struct cfil_msg_data_event *data_req;
	struct inpcb *inp = (struct inpcb *)so->so_pcb;
	struct cfil_entry *entry;
	struct cfe_buf *entrybuf;
	struct content_filter *cfc;
	struct timeval tv;
	int inp_flags = 0;

	cfil_rw_lock_shared(&cfil_lck_rw);

	entry = &cfil_info->cfi_entries[kcunit - 1];
	if (outgoing) {
		entrybuf = &entry->cfe_snd;
	} else {
		entrybuf = &entry->cfe_rcv;
	}

	cfc = entry->cfe_filter;
	if (cfc == NULL) {
		goto done;
	}

	data = cfil_data_start(data);
	if (data == NULL) {
		CFIL_LOG(LOG_ERR, "No data start");
		goto done;
	}

	CFIL_LOG(LOG_INFO, "so %llx kcunit %u outgoing %d",
	    (uint64_t)VM_KERNEL_ADDRPERM(so), kcunit, outgoing);

	socket_lock_assert_owned(so);

	/* Would be wasteful to try */
	if (cfc->cf_flags & CFF_FLOW_CONTROLLED) {
		error = ENOBUFS;
		goto done;
	}

	/* Make a copy of the data to pass to kernel control socket */
	copy = m_copym_mode(data, copyoffset, copylen, M_DONTWAIT, NULL, NULL,
	    M_COPYM_NOOP_HDR);
	if (copy == NULL) {
		CFIL_LOG(LOG_ERR, "m_copym_mode() failed");
		error = ENOMEM;
		goto done;
	}

	/* We need an mbuf packet for the message header */
	const size_t hdrsize = sizeof(struct cfil_msg_data_event);
	error = mbuf_allocpacket(MBUF_DONTWAIT, hdrsize, &one, &msg);
	if (error != 0) {
		CFIL_LOG(LOG_ERR, "mbuf_allocpacket() failed");
		m_freem(copy);
		/*
		 * ENOBUFS is to indicate flow control
		 */
		error = ENOMEM;
		goto done;
	}
	mbuf_setlen(msg, hdrsize);
	mbuf_pkthdr_setlen(msg, hdrsize + copylen);
	msg->m_next = copy;
	data_req = mtod(msg, struct cfil_msg_data_event *);
	bzero(data_req, hdrsize);
	data_req->cfd_msghdr.cfm_len = (uint32_t)hdrsize + copylen;
	data_req->cfd_msghdr.cfm_version = 1;
	data_req->cfd_msghdr.cfm_type = CFM_TYPE_EVENT;
	data_req->cfd_msghdr.cfm_op =
	    outgoing ? CFM_OP_DATA_OUT : CFM_OP_DATA_IN;
	data_req->cfd_msghdr.cfm_sock_id =
	    entry->cfe_cfil_info->cfi_sock_id;
	data_req->cfd_start_offset = entrybuf->cfe_peeked;
	data_req->cfd_end_offset = entrybuf->cfe_peeked + copylen;
	// The last_pid or e_pid is set here because a socket could have been
	// accepted by launchd and a new process spawned (with a new pid).
	// So the last pid associated with the socket is appended to the data event.
	// for a provider that is peeking bytes.
	if (so->so_flags & SOF_DELEGATED) {
		data_req->cfd_delegated_pid = so->e_pid;
	} else {
		data_req->cfd_delegated_pid = so->last_pid;
	}
	if (data_req->cfd_delegated_pid != 0) {
		if (!cfil_copy_audit_token(data_req->cfd_delegated_pid, (audit_token_t *)&data_req->cfd_delegated_audit_token)) {
			CFIL_LOG(LOG_ERR, "CFIL: Failed to get audit token for <sockID %llu <%llx>> ",
			    entry->cfe_cfil_info->cfi_sock_id, entry->cfe_cfil_info->cfi_sock_id);
		}
	}

	data_req->cfd_flags = 0;
	if (OPTIONAL_IP_HEADER(so)) {
		/*
		 * For non-UDP/TCP traffic, indicate to filters if optional
		 * IP header is present:
		 *      outgoing - indicate according to INP_HDRINCL flag
		 *      incoming - For IPv4 only, stripping of IP header is
		 *                 optional.  But for CFIL, we delay stripping
		 *                 at rip_input.  So CFIL always expects IP
		 *                 frames. IP header will be stripped according
		 *                 to INP_STRIPHDR flag later at reinjection.
		 */
		if ((!outgoing && !IS_INP_V6(inp)) ||
		    (outgoing && cfil_dgram_peek_socket_state(data, &inp_flags) && (inp_flags & INP_HDRINCL))) {
			data_req->cfd_flags |= CFD_DATA_FLAG_IP_HEADER;
		}
	}

	/*
	 * Copy address/port into event msg.
	 * For non connected sockets need to copy addresses from passed
	 * parameters
	 */
	cfil_fill_event_msg_addresses(cfil_info->cfi_hash_entry, inp,
	    &data_req->cfc_src, &data_req->cfc_dst,
	    !IS_INP_V6(inp), outgoing);

	if (cfil_info->cfi_debug && cfil_log_data) {
		cfil_info_log(LOG_ERR, cfil_info, "CFIL: SENDING DATA UP");
	}

	if (cfil_info->cfi_isSignatureLatest == false) {
		cfil_dispatch_data_event_sign(entry->cfe_filter->cf_crypto_state, so, cfil_info, data_req);
	}

	microuptime(&tv);
	CFI_ADD_TIME_LOG(cfil_info, &tv, &cfil_info->cfi_first_event, data_req->cfd_msghdr.cfm_op);

	/* Pass the message to the content filter */
	error = ctl_enqueuembuf(entry->cfe_filter->cf_kcref,
	    entry->cfe_filter->cf_kcunit,
	    msg, CTL_DATA_EOR);
	if (error != 0) {
		CFIL_LOG(LOG_ERR, "ctl_enqueuembuf() failed: %d", error);
		mbuf_freem(msg);
		goto done;
	}
	entry->cfe_flags &= ~CFEF_FLOW_CONTROLLED;
	OSIncrementAtomic(&cfil_stats.cfs_data_event_ok);

	if (cfil_info->cfi_debug && cfil_log_data) {
		CFIL_LOG(LOG_ERR, "CFIL: VERDICT ACTION: so %llx sockID %llu <%llx> outgoing %d: mbuf %llx copyoffset %u copylen %u (%s)",
		    (uint64_t)VM_KERNEL_ADDRPERM(so), cfil_info->cfi_sock_id, cfil_info->cfi_sock_id, outgoing, (uint64_t)VM_KERNEL_ADDRPERM(data), copyoffset, copylen,
		    data_req->cfd_flags & CFD_DATA_FLAG_IP_HEADER ? "IP HDR" : "NO IP HDR");
	}

done:
	if (error == ENOBUFS) {
		entry->cfe_flags |= CFEF_FLOW_CONTROLLED;
		OSIncrementAtomic(
			&cfil_stats.cfs_data_event_flow_control);

		if (!cfil_rw_lock_shared_to_exclusive(&cfil_lck_rw)) {
			cfil_rw_lock_exclusive(&cfil_lck_rw);
		}

		cfc->cf_flags |= CFF_FLOW_CONTROLLED;

		cfil_rw_unlock_exclusive(&cfil_lck_rw);
	} else {
		if (error != 0) {
			OSIncrementAtomic(&cfil_stats.cfs_data_event_fail);
		}

		cfil_rw_unlock_shared(&cfil_lck_rw);
	}
	return error;
}

/*
 * Process the queue of data waiting to be delivered to content filter
 */
static int
cfil_data_service_ctl_q(struct socket *so, struct cfil_info *cfil_info, uint32_t kcunit, int outgoing)
{
	errno_t error = 0;
	struct mbuf *data, *tmp = NULL;
	unsigned int datalen = 0, copylen = 0, copyoffset = 0;
	struct cfil_entry *entry;
	struct cfe_buf *entrybuf;
	uint64_t currentoffset = 0;

	if (cfil_info == NULL) {
		return 0;
	}

	CFIL_LOG(LOG_INFO, "so %llx kcunit %u outgoing %d",
	    (uint64_t)VM_KERNEL_ADDRPERM(so), kcunit, outgoing);

	socket_lock_assert_owned(so);

	entry = &cfil_info->cfi_entries[kcunit - 1];
	if (outgoing) {
		entrybuf = &entry->cfe_snd;
	} else {
		entrybuf = &entry->cfe_rcv;
	}

	/* Send attached message if not yet done */
	if ((entry->cfe_flags & CFEF_SENT_SOCK_ATTACHED) == 0) {
		error = cfil_dispatch_attach_event(so, cfil_info, CFI_ENTRY_KCUNIT(cfil_info, entry),
		    cfil_info->cfi_dir);
		if (error != 0) {
			/* We can recover from flow control */
			if (error == ENOBUFS || error == ENOMEM) {
				error = 0;
			}
			goto done;
		}
	} else if ((entry->cfe_flags & CFEF_DATA_START) == 0) {
		OSIncrementAtomic(&cfil_stats.cfs_ctl_q_not_started);
		goto done;
	}

	if (cfil_info->cfi_debug && cfil_log_data) {
		CFIL_LOG(LOG_ERR, "CFIL: SERVICE CTL-Q: pass_offset %llu peeked %llu peek_offset %llu",
		    entrybuf->cfe_pass_offset,
		    entrybuf->cfe_peeked,
		    entrybuf->cfe_peek_offset);
	}

	/* Move all data that can pass */
	while ((data = cfil_queue_first(&entrybuf->cfe_ctl_q)) != NULL &&
	    entrybuf->cfe_ctl_q.q_start < entrybuf->cfe_pass_offset) {
		datalen = cfil_data_length(data, NULL, NULL);
		tmp = data;

		if (entrybuf->cfe_ctl_q.q_start + datalen <=
		    entrybuf->cfe_pass_offset) {
			/*
			 * The first mbuf can fully pass
			 */
			copylen = datalen;
		} else {
			/*
			 * The first mbuf can partially pass
			 */
			copylen = (unsigned int)(entrybuf->cfe_pass_offset - entrybuf->cfe_ctl_q.q_start);
		}
		VERIFY(copylen <= datalen);

		if (cfil_info->cfi_debug && cfil_log_data) {
			CFIL_LOG(LOG_ERR,
			    "CFIL: SERVICE CTL-Q PASSING: %llx first %llu peeked %llu pass %llu peek %llu"
			    "datalen %u copylen %u",
			    (uint64_t)VM_KERNEL_ADDRPERM(tmp),
			    entrybuf->cfe_ctl_q.q_start,
			    entrybuf->cfe_peeked,
			    entrybuf->cfe_pass_offset,
			    entrybuf->cfe_peek_offset,
			    datalen, copylen);
		}

		/*
		 * Data that passes has been peeked at explicitly or
		 * implicitly
		 */
		if (entrybuf->cfe_ctl_q.q_start + copylen >
		    entrybuf->cfe_peeked) {
			entrybuf->cfe_peeked =
			    entrybuf->cfe_ctl_q.q_start + copylen;
		}
		/*
		 * Stop on partial pass
		 */
		if (copylen < datalen) {
			break;
		}

		/* All good, move full data from ctl queue to pending queue */
		cfil_queue_remove(&entrybuf->cfe_ctl_q, data, datalen);

		cfil_queue_enqueue(&entrybuf->cfe_pending_q, data, datalen);
		if (outgoing) {
			OSAddAtomic64(datalen,
			    &cfil_stats.cfs_pending_q_out_enqueued);
		} else {
			OSAddAtomic64(datalen,
			    &cfil_stats.cfs_pending_q_in_enqueued);
		}
	}
	CFIL_INFO_VERIFY(cfil_info);
	if (tmp != NULL) {
		CFIL_LOG(LOG_DEBUG,
		    "%llx first %llu peeked %llu pass %llu peek %llu"
		    "datalen %u copylen %u",
		    (uint64_t)VM_KERNEL_ADDRPERM(tmp),
		    entrybuf->cfe_ctl_q.q_start,
		    entrybuf->cfe_peeked,
		    entrybuf->cfe_pass_offset,
		    entrybuf->cfe_peek_offset,
		    datalen, copylen);
	}
	tmp = NULL;

	/* Now deal with remaining data the filter wants to peek at */
	for (data = cfil_queue_first(&entrybuf->cfe_ctl_q),
	    currentoffset = entrybuf->cfe_ctl_q.q_start;
	    data != NULL && currentoffset < entrybuf->cfe_peek_offset;
	    data = cfil_queue_next(&entrybuf->cfe_ctl_q, data),
	    currentoffset += datalen) {
		datalen = cfil_data_length(data, NULL, NULL);
		tmp = data;

		/* We've already peeked at this mbuf */
		if (currentoffset + datalen <= entrybuf->cfe_peeked) {
			continue;
		}
		/*
		 * The data in the first mbuf may have been
		 * partially peeked at
		 */
		copyoffset = (unsigned int)(entrybuf->cfe_peeked - currentoffset);
		VERIFY(copyoffset < datalen);
		copylen = datalen - copyoffset;
		VERIFY(copylen <= datalen);
		/*
		 * Do not copy more than needed
		 */
		if (currentoffset + copyoffset + copylen >
		    entrybuf->cfe_peek_offset) {
			copylen = (unsigned int)(entrybuf->cfe_peek_offset -
			    (currentoffset + copyoffset));
		}

		if (cfil_info->cfi_debug && cfil_log_data) {
			CFIL_LOG(LOG_ERR,
			    "CFIL: SERVICE CTL-Q PEEKING: %llx current %llu peeked %llu pass %llu peek %llu "
			    "datalen %u copylen %u copyoffset %u",
			    (uint64_t)VM_KERNEL_ADDRPERM(tmp),
			    currentoffset,
			    entrybuf->cfe_peeked,
			    entrybuf->cfe_pass_offset,
			    entrybuf->cfe_peek_offset,
			    datalen, copylen, copyoffset);
		}

		/*
		 * Stop if there is nothing more to peek at
		 */
		if (copylen == 0) {
			break;
		}
		/*
		 * Let the filter get a peek at this span of data
		 */
		error = cfil_dispatch_data_event(so, cfil_info, kcunit,
		    outgoing, data, copyoffset, copylen);
		if (error != 0) {
			/* On error, leave data in ctl_q */
			break;
		}
		entrybuf->cfe_peeked += copylen;
		if (outgoing) {
			OSAddAtomic64(copylen,
			    &cfil_stats.cfs_ctl_q_out_peeked);
		} else {
			OSAddAtomic64(copylen,
			    &cfil_stats.cfs_ctl_q_in_peeked);
		}

		/* Stop when data could not be fully peeked at */
		if (copylen + copyoffset < datalen) {
			break;
		}
	}
	CFIL_INFO_VERIFY(cfil_info);
	if (tmp != NULL) {
		CFIL_LOG(LOG_DEBUG,
		    "%llx first %llu peeked %llu pass %llu peek %llu"
		    "datalen %u copylen %u copyoffset %u",
		    (uint64_t)VM_KERNEL_ADDRPERM(tmp),
		    currentoffset,
		    entrybuf->cfe_peeked,
		    entrybuf->cfe_pass_offset,
		    entrybuf->cfe_peek_offset,
		    datalen, copylen, copyoffset);
	}

	/*
	 * Process data that has passed the filter
	 */
	error = cfil_service_pending_queue(so, cfil_info, kcunit, outgoing);
	if (error != 0) {
		if (error != EJUSTRETURN) {
			CFIL_LOG(LOG_ERR, "cfil_service_pending_queue() error %d", error);
		}
		goto done;
	}

	/*
	 * Dispatch disconnect events that could not be sent
	 */
	if (cfil_info == NULL) {
		goto done;
	} else if (outgoing) {
		if ((cfil_info->cfi_flags & CFIF_SHUT_WR) &&
		    !(entry->cfe_flags & CFEF_SENT_DISCONNECT_OUT)) {
			cfil_dispatch_disconnect_event(so, cfil_info, kcunit, 1);
		}
	} else {
		if ((cfil_info->cfi_flags & CFIF_SHUT_RD) &&
		    !(entry->cfe_flags & CFEF_SENT_DISCONNECT_IN)) {
			cfil_dispatch_disconnect_event(so, cfil_info, kcunit, 0);
		}
	}

done:
	CFIL_LOG(LOG_DEBUG,
	    "first %llu peeked %llu pass %llu peek %llu",
	    entrybuf->cfe_ctl_q.q_start,
	    entrybuf->cfe_peeked,
	    entrybuf->cfe_pass_offset,
	    entrybuf->cfe_peek_offset);

	CFIL_INFO_VERIFY(cfil_info);
	return error;
}

/*
 * cfil_data_filter()
 *
 * Process data for a content filter installed on a socket
 */
int
cfil_data_filter(struct socket *so, struct cfil_info *cfil_info, uint32_t kcunit, int outgoing,
    struct mbuf *data, uint32_t datalen)
{
	errno_t error = 0;
	struct cfil_entry *entry;
	struct cfe_buf *entrybuf;

	CFIL_LOG(LOG_INFO, "so %llx kcunit %u outgoing %d",
	    (uint64_t)VM_KERNEL_ADDRPERM(so), kcunit, outgoing);

	socket_lock_assert_owned(so);

	entry = &cfil_info->cfi_entries[kcunit - 1];
	if (outgoing) {
		entrybuf = &entry->cfe_snd;
	} else {
		entrybuf = &entry->cfe_rcv;
	}

	/* Are we attached to the filter? */
	if (entry->cfe_filter == NULL) {
		error = 0;
		goto done;
	}

	/* Dispatch to filters */
	cfil_queue_enqueue(&entrybuf->cfe_ctl_q, data, datalen);
	if (outgoing) {
		OSAddAtomic64(datalen,
		    &cfil_stats.cfs_ctl_q_out_enqueued);
	} else {
		OSAddAtomic64(datalen,
		    &cfil_stats.cfs_ctl_q_in_enqueued);
	}

	error = cfil_data_service_ctl_q(so, cfil_info, kcunit, outgoing);
	if (error != 0) {
		if (error != EJUSTRETURN) {
			CFIL_LOG(LOG_ERR, "cfil_data_service_ctl_q() error %d", error);
		}
	}
	/*
	 * We have to return EJUSTRETURN in all cases to avoid double free
	 * by socket layer
	 */
	error = EJUSTRETURN;
done:
	CFIL_INFO_VERIFY(cfil_info);

	CFIL_LOG(LOG_INFO, "return %d", error);
	return error;
}

static void
cfil_strip_ip_header(struct cfil_info *cfil_info, mbuf_t data, struct socket *so)
{
	struct ip *ip = NULL;
	unsigned int hlen = 0;
	mbuf_t data_start = NULL;
	struct inpcb *inp = so ? sotoinpcb(so) : NULL;

	if (inp && (inp->inp_flags & INP_STRIPHDR)) {
		data_start = cfil_data_start(data);
		if (data_start != NULL && (data_start->m_flags & M_PKTHDR)) {
			ip = mtod(data_start, struct ip *);
			hlen = IP_VHL_HL(ip->ip_vhl) << 2;

			if (cfil_info->cfi_debug && cfil_log_data) {
				CFIL_LOG(LOG_ERR, "CFIL: IPHDR STRIPPING: <so %llx>: <hlen %d m_len %d>",
				    (uint64_t)VM_KERNEL_ADDRPERM(so),
				    hlen, data_start->m_len);
			}
			VERIFY(hlen <= data_start->m_len);
			data_start->m_len -= hlen;
			data_start->m_pkthdr.len -= hlen;
			data_start->m_data += hlen;
		}
	}
}

/*
 * cfil_service_inject_queue() re-inject data that passed the
 * content filters
 */
static int
cfil_service_inject_queue(struct socket *so, struct cfil_info *cfil_info, int outgoing)
{
	mbuf_t data;
	unsigned int datalen;
	int mbcnt = 0;
	int mbnum = 0;
	errno_t error = 0;
	struct cfi_buf *cfi_buf;
	struct cfil_queue *inject_q;
	int need_rwakeup = 0;
	int count = 0;

	if (cfil_info == NULL) {
		return 0;
	}

	socket_lock_assert_owned(so);

	if (so->so_state & SS_DEFUNCT) {
		return 0;
	}

	if (outgoing) {
		cfi_buf = &cfil_info->cfi_snd;
		cfil_info->cfi_flags &= ~CFIF_RETRY_INJECT_OUT;
	} else {
		cfi_buf = &cfil_info->cfi_rcv;
		cfil_info->cfi_flags &= ~CFIF_RETRY_INJECT_IN;
	}
	inject_q = &cfi_buf->cfi_inject_q;

	if (cfil_queue_empty(inject_q)) {
		return 0;
	}

	if (cfil_info->cfi_debug && cfil_log_data) {
		CFIL_LOG(LOG_ERR, "CFIL: SERVICE INJECT-Q: <so %llx> outgoing %d queue len %llu",
		    (uint64_t)VM_KERNEL_ADDRPERM(so), outgoing, cfil_queue_len(inject_q));
	}

	while ((data = cfil_queue_first(inject_q)) != NULL) {
		datalen = cfil_data_length(data, &mbcnt, &mbnum);

		if (cfil_info->cfi_debug && cfil_log_data) {
			CFIL_LOG(LOG_ERR, "CFIL: SERVICE INJECT-Q: <so %llx> data %llx datalen %u (mbcnt %u)",
			    (uint64_t)VM_KERNEL_ADDRPERM(so), (uint64_t)VM_KERNEL_ADDRPERM(data), datalen, mbcnt);
		}

		/* Remove data from queue and adjust stats */
		cfil_queue_remove(inject_q, data, datalen);
		cfi_buf->cfi_pending_first += datalen;
		cfi_buf->cfi_pending_mbcnt -= mbcnt;
		cfi_buf->cfi_pending_mbnum -= mbnum;
		cfil_info_buf_verify(cfi_buf);

		if (outgoing) {
			error = sosend_reinject(so, NULL, data, NULL, 0);
			if (error != 0) {
				cfil_info_log(LOG_ERR, cfil_info, "CFIL: Error: sosend_reinject() failed");
				CFIL_LOG(LOG_ERR, "CFIL: sosend() failed %d", error);
				break;
			}
			// At least one injection succeeded, need to wake up pending threads.
			need_rwakeup = 1;
		} else {
			data->m_flags |= M_SKIPCFIL;

			/*
			 * NOTE: We currently only support TCP, UDP, ICMP,
			 * ICMPv6 and RAWIP.  For MPTCP and message TCP we'll
			 * need to call the appropriate sbappendxxx()
			 * of fix sock_inject_data_in()
			 */
			if (NEED_DGRAM_FLOW_TRACKING(so)) {
				if (OPTIONAL_IP_HEADER(so)) {
					cfil_strip_ip_header(cfil_info, data, so);
				}

				if (sbappendchain(&so->so_rcv, data)) {
					need_rwakeup = 1;
				}
			} else {
				if (sbappendstream(&so->so_rcv, data)) {
					need_rwakeup = 1;
				}
			}
		}

		if (outgoing) {
			OSAddAtomic64(datalen,
			    &cfil_stats.cfs_inject_q_out_passed);
		} else {
			OSAddAtomic64(datalen,
			    &cfil_stats.cfs_inject_q_in_passed);
		}

		count++;
	}

	if (cfil_info->cfi_debug && cfil_log_data) {
		CFIL_LOG(LOG_ERR, "CFIL: SERVICE INJECT-Q: <so %llx> injected %d",
		    (uint64_t)VM_KERNEL_ADDRPERM(so), count);
	}

	/* A single wakeup is for several packets is more efficient */
	if (need_rwakeup) {
		if (outgoing == TRUE) {
			sowwakeup(so);
		} else {
			sorwakeup(so);
		}
	}

	if (error != 0 && cfil_info) {
		if (error == ENOBUFS) {
			OSIncrementAtomic(&cfil_stats.cfs_inject_q_nobufs);
		}
		if (error == ENOMEM) {
			OSIncrementAtomic(&cfil_stats.cfs_inject_q_nomem);
		}

		if (outgoing) {
			cfil_info->cfi_flags |= CFIF_RETRY_INJECT_OUT;
			OSIncrementAtomic(&cfil_stats.cfs_inject_q_out_fail);
		} else {
			cfil_info->cfi_flags |= CFIF_RETRY_INJECT_IN;
			OSIncrementAtomic(&cfil_stats.cfs_inject_q_in_fail);
		}
	}

	/*
	 * Notify
	 */
	if (cfil_info && (cfil_info->cfi_flags & CFIF_SHUT_WR)) {
		cfil_sock_notify_shutdown(so, SHUT_WR);
		if (cfil_sock_data_pending(&so->so_snd) == 0) {
			soshutdownlock_final(so, SHUT_WR);
		}
	}
	if (cfil_info && (cfil_info->cfi_flags & CFIF_CLOSE_WAIT)) {
		if (cfil_filters_attached(so) == 0) {
			CFIL_LOG(LOG_INFO, "so %llx waking",
			    (uint64_t)VM_KERNEL_ADDRPERM(so));
			wakeup((caddr_t)cfil_info);
		}
	}

	if (SO_DELAYED_DEAD_GET(so)) {
		// Check to see if all data processed for this socket, if so mark it DEAD now.
		const bool is_dead = cfil_sock_is_dead(so);
		if (is_dead && cfil_info->cfi_debug) {
			cfil_info_log(LOG_ERR, cfil_info, "CFIL: Marked previoulsy delayed socket as DEAD");
		}
	}
	if (SO_DELAYED_TCP_TIME_WAIT_GET(so)) {
		// Check to see if all data processed for this socket, if so handle the TCP time wait now
		const bool is_added = cfil_sock_tcp_add_time_wait(so);
		if (is_added && cfil_info->cfi_debug) {
			cfil_info_log(LOG_ERR, cfil_info, "CFIL: Handled previously delayed socket for TCP time wait");
		}
	}

	CFIL_INFO_VERIFY(cfil_info);

	return error;
}

static int
cfil_service_pending_queue(struct socket *so, struct cfil_info *cfil_info, uint32_t kcunit, int outgoing)
{
	uint64_t passlen, curlen;
	mbuf_t data;
	unsigned int datalen;
	errno_t error = 0;
	struct cfil_entry *entry;
	struct cfe_buf *entrybuf;
	struct cfil_queue *pending_q;
	struct cfil_entry *iter_entry = NULL;

	CFIL_LOG(LOG_INFO, "so %llx kcunit %u outgoing %d",
	    (uint64_t)VM_KERNEL_ADDRPERM(so), kcunit, outgoing);

	socket_lock_assert_owned(so);

	entry = &cfil_info->cfi_entries[kcunit - 1];
	if (outgoing) {
		entrybuf = &entry->cfe_snd;
	} else {
		entrybuf = &entry->cfe_rcv;
	}

	pending_q = &entrybuf->cfe_pending_q;

	passlen = entrybuf->cfe_pass_offset - pending_q->q_start;

	if (cfil_queue_empty(pending_q)) {
		for (iter_entry = SLIST_NEXT(entry, cfe_order_link);
		    iter_entry != NULL;
		    iter_entry = SLIST_NEXT(iter_entry, cfe_order_link)) {
			error = cfil_data_service_ctl_q(so, cfil_info, CFI_ENTRY_KCUNIT(cfil_info, iter_entry), outgoing);
			/* 0 means passed so we can continue */
			if (error != 0) {
				break;
			}
		}
		goto done;
	}

	/*
	 * Locate the chunks of data that we can pass to the next filter
	 * A data chunk must be on mbuf boundaries
	 */
	curlen = 0;
	while ((data = cfil_queue_first(pending_q)) != NULL) {
		datalen = cfil_data_length(data, NULL, NULL);

		if (cfil_info->cfi_debug && cfil_log_data) {
			CFIL_LOG(LOG_ERR,
			    "CFIL: SERVICE PENDING-Q: data %llx datalen %u passlen %llu curlen %llu",
			    (uint64_t)VM_KERNEL_ADDRPERM(data), datalen,
			    passlen, curlen);
		}

		if (curlen + datalen > passlen) {
			break;
		}

		cfil_queue_remove(pending_q, data, datalen);

		curlen += datalen;

		for (iter_entry = SLIST_NEXT(entry, cfe_order_link);
		    iter_entry != NULL;
		    iter_entry = SLIST_NEXT(iter_entry, cfe_order_link)) {
			error = cfil_data_filter(so, cfil_info, CFI_ENTRY_KCUNIT(cfil_info, iter_entry), outgoing,
			    data, datalen);
			/* 0 means passed so we can continue */
			if (error != 0) {
				break;
			}
		}
		/* When data has passed all filters, re-inject */
		if (error == 0) {
			if (outgoing) {
				cfil_queue_enqueue(
					&cfil_info->cfi_snd.cfi_inject_q,
					data, datalen);
				OSAddAtomic64(datalen,
				    &cfil_stats.cfs_inject_q_out_enqueued);
			} else {
				cfil_queue_enqueue(
					&cfil_info->cfi_rcv.cfi_inject_q,
					data, datalen);
				OSAddAtomic64(datalen,
				    &cfil_stats.cfs_inject_q_in_enqueued);
			}
		}
	}

done:
	CFIL_INFO_VERIFY(cfil_info);

	return error;
}

int
cfil_update_data_offsets(struct socket *so, struct cfil_info *cfil_info, uint32_t kcunit, int outgoing,
    uint64_t pass_offset, uint64_t peek_offset)
{
	errno_t error = 0;
	struct cfil_entry *entry = NULL;
	struct cfe_buf *entrybuf;
	int updated = 0;

	CFIL_LOG(LOG_INFO, "pass %llu peek %llu", pass_offset, peek_offset);

	socket_lock_assert_owned(so);

	if (cfil_info == NULL) {
		CFIL_LOG(LOG_ERR, "so %llx cfil detached",
		    (uint64_t)VM_KERNEL_ADDRPERM(so));
		error = 0;
		goto done;
	} else if (cfil_info->cfi_flags & CFIF_DROP) {
		CFIL_LOG(LOG_ERR, "so %llx drop set",
		    (uint64_t)VM_KERNEL_ADDRPERM(so));
		error = EPIPE;
		goto done;
	}

	entry = &cfil_info->cfi_entries[kcunit - 1];
	if (outgoing) {
		entrybuf = &entry->cfe_snd;
	} else {
		entrybuf = &entry->cfe_rcv;
	}

	/* Record updated offsets for this content filter */
	if (pass_offset > entrybuf->cfe_pass_offset) {
		entrybuf->cfe_pass_offset = pass_offset;

		if (entrybuf->cfe_peek_offset < entrybuf->cfe_pass_offset) {
			entrybuf->cfe_peek_offset = entrybuf->cfe_pass_offset;
		}
		updated = 1;
	} else {
		CFIL_LOG(LOG_INFO, "pass_offset %llu <= cfe_pass_offset %llu",
		    pass_offset, entrybuf->cfe_pass_offset);
	}
	/* Filter does not want or need to see data that's allowed to pass */
	if (peek_offset > entrybuf->cfe_pass_offset &&
	    peek_offset > entrybuf->cfe_peek_offset) {
		entrybuf->cfe_peek_offset = peek_offset;
		updated = 1;
	}
	/* Nothing to do */
	if (updated == 0) {
		goto done;
	}

	/* Move data held in control queue to pending queue if needed */
	error = cfil_data_service_ctl_q(so, cfil_info, kcunit, outgoing);
	if (error != 0) {
		if (error != EJUSTRETURN) {
			CFIL_LOG(LOG_ERR, "cfil_data_service_ctl_q() error %d", error);
		}
		goto done;
	}
	error = EJUSTRETURN;

done:
	/*
	 * The filter is effectively detached when pass all from both sides
	 * or when the socket is closed and no more data is waiting
	 * to be delivered to the filter
	 */
	if (entry != NULL &&
	    ((entry->cfe_snd.cfe_pass_offset == CFM_MAX_OFFSET &&
	    entry->cfe_rcv.cfe_pass_offset == CFM_MAX_OFFSET) ||
	    ((cfil_info->cfi_flags & CFIF_CLOSE_WAIT) &&
	    cfil_queue_empty(&entry->cfe_snd.cfe_ctl_q) &&
	    cfil_queue_empty(&entry->cfe_rcv.cfe_ctl_q)))) {
		entry->cfe_flags |= CFEF_CFIL_DETACHED;

		if (cfil_info->cfi_debug) {
			const char * __null_terminated out = "CFIL: OUT - PASSED ALL - DETACH";
			const char * __null_terminated in = "CFIL: IN - PASSED ALL - DETACH";
			cfil_info_log(LOG_ERR, cfil_info, outgoing ? out : in);
		}

		CFIL_LOG(LOG_INFO, "so %llx detached %u",
		    (uint64_t)VM_KERNEL_ADDRPERM(so), kcunit);
		if ((cfil_info->cfi_flags & CFIF_CLOSE_WAIT) &&
		    cfil_filters_attached(so) == 0) {
			if (cfil_info->cfi_debug) {
				cfil_info_log(LOG_ERR, cfil_info, "CFIL: WAKING");
			}
			CFIL_LOG(LOG_INFO, "so %llx waking",
			    (uint64_t)VM_KERNEL_ADDRPERM(so));
			wakeup((caddr_t)cfil_info);
		}
	}
	CFIL_INFO_VERIFY(cfil_info);
	CFIL_LOG(LOG_INFO, "return %d", error);
	return error;
}

/*
 * Update pass offset for socket when no data is pending
 */
static int
cfil_set_socket_pass_offset(struct socket *so, struct cfil_info *cfil_info, int outgoing)
{
	struct cfi_buf *cfi_buf;
	struct cfil_entry *entry;
	struct cfe_buf *entrybuf;
	uint32_t kcunit;
	uint64_t pass_offset = 0;
	boolean_t first = true;

	if (cfil_info == NULL) {
		return 0;
	}

	if (cfil_info->cfi_debug && cfil_log_data) {
		CFIL_LOG(LOG_ERR, "so %llx outgoing %d",
		    (uint64_t)VM_KERNEL_ADDRPERM(so), outgoing);
	}

	socket_lock_assert_owned(so);

	if (outgoing) {
		cfi_buf = &cfil_info->cfi_snd;
	} else {
		cfi_buf = &cfil_info->cfi_rcv;
	}

	if (cfil_info->cfi_debug && cfil_log_data) {
		CFIL_LOG(LOG_ERR, "CFIL: <so %llx, sockID %llu <%llx>> outgoing %d cfi_pending_first %llu cfi_pending_last %llu",
		    (uint64_t)VM_KERNEL_ADDRPERM(so), cfil_info->cfi_sock_id, cfil_info->cfi_sock_id, outgoing,
		    cfi_buf->cfi_pending_first, cfi_buf->cfi_pending_last);
	}

	if (cfi_buf->cfi_pending_last - cfi_buf->cfi_pending_first == 0) {
		for (kcunit = 1; kcunit <= MAX_CONTENT_FILTER; kcunit++) {
			entry = &cfil_info->cfi_entries[kcunit - 1];

			/* Are we attached to a filter? */
			if (entry->cfe_filter == NULL) {
				continue;
			}

			if (outgoing) {
				entrybuf = &entry->cfe_snd;
			} else {
				entrybuf = &entry->cfe_rcv;
			}

			// Keep track of the smallest pass_offset among filters.
			if (first == true ||
			    entrybuf->cfe_pass_offset < pass_offset) {
				pass_offset = entrybuf->cfe_pass_offset;
				first = false;
			}
		}
		cfi_buf->cfi_pass_offset = pass_offset;
	}

	if (cfil_info->cfi_debug && cfil_log_data) {
		CFIL_LOG(LOG_ERR, "CFIL: <so %llx, sockID %llu <%llx>>, cfi_pass_offset %llu",
		    (uint64_t)VM_KERNEL_ADDRPERM(so), cfil_info->cfi_sock_id, cfil_info->cfi_sock_id, cfi_buf->cfi_pass_offset);
	}

	return 0;
}

int
cfil_action_data_pass(struct socket *so, struct cfil_info *cfil_info, uint32_t kcunit, int outgoing,
    uint64_t pass_offset, uint64_t peek_offset)
{
	errno_t error = 0;

	CFIL_LOG(LOG_INFO, "");

	socket_lock_assert_owned(so);

	error = cfil_acquire_sockbuf(so, cfil_info, outgoing);
	if (error != 0) {
		CFIL_LOG(LOG_INFO, "so %llx %s dropped",
		    (uint64_t)VM_KERNEL_ADDRPERM(so),
		    outgoing ? "out" : "in");
		goto release;
	}

	error = cfil_update_data_offsets(so, cfil_info, kcunit, outgoing,
	    pass_offset, peek_offset);

	cfil_service_inject_queue(so, cfil_info, outgoing);

	cfil_set_socket_pass_offset(so, cfil_info, outgoing);
release:
	CFIL_INFO_VERIFY(cfil_info);
	cfil_release_sockbuf(so, outgoing);

	return error;
}


static void
cfil_flush_queues(struct socket *so, struct cfil_info *cfil_info)
{
	struct cfil_entry *entry;
	int kcunit;
	uint64_t drained;

	if ((so->so_flags & SOF_CONTENT_FILTER) == 0 || cfil_info == NULL) {
		goto done;
	}

	socket_lock_assert_owned(so);

	/*
	 * Flush the output queues and ignore errors as long as
	 * we are attached
	 */
	(void) cfil_acquire_sockbuf(so, cfil_info, 1);
	if (cfil_info != NULL) {
		drained = 0;
		for (kcunit = 1; kcunit <= MAX_CONTENT_FILTER; kcunit++) {
			entry = &cfil_info->cfi_entries[kcunit - 1];

			drained += cfil_queue_drain(&entry->cfe_snd.cfe_ctl_q);
			drained += cfil_queue_drain(&entry->cfe_snd.cfe_pending_q);
		}
		drained += cfil_queue_drain(&cfil_info->cfi_snd.cfi_inject_q);

		if (drained) {
			if (cfil_info->cfi_flags & CFIF_DROP) {
				OSIncrementAtomic(
					&cfil_stats.cfs_flush_out_drop);
			} else {
				OSIncrementAtomic(
					&cfil_stats.cfs_flush_out_close);
			}
		}
	}
	cfil_release_sockbuf(so, 1);

	/*
	 * Flush the input queues
	 */
	(void) cfil_acquire_sockbuf(so, cfil_info, 0);
	if (cfil_info != NULL) {
		drained = 0;
		for (kcunit = 1; kcunit <= MAX_CONTENT_FILTER; kcunit++) {
			entry = &cfil_info->cfi_entries[kcunit - 1];

			drained += cfil_queue_drain(
				&entry->cfe_rcv.cfe_ctl_q);
			drained += cfil_queue_drain(
				&entry->cfe_rcv.cfe_pending_q);
		}
		drained += cfil_queue_drain(&cfil_info->cfi_rcv.cfi_inject_q);

		if (drained) {
			if (cfil_info->cfi_flags & CFIF_DROP) {
				OSIncrementAtomic(
					&cfil_stats.cfs_flush_in_drop);
			} else {
				OSIncrementAtomic(
					&cfil_stats.cfs_flush_in_close);
			}
		}
	}
	cfil_release_sockbuf(so, 0);
done:
	CFIL_INFO_VERIFY(cfil_info);
}

int
cfil_action_drop(struct socket *so, struct cfil_info *cfil_info, uint32_t kcunit)
{
	errno_t error = 0;
	struct cfil_entry *entry;
	struct proc *p;

	if ((so->so_flags & SOF_CONTENT_FILTER) == 0 || cfil_info == NULL) {
		goto done;
	}

	socket_lock_assert_owned(so);

	entry = &cfil_info->cfi_entries[kcunit - 1];

	/* Are we attached to the filter? */
	if (entry->cfe_filter == NULL) {
		goto done;
	}

	cfil_info->cfi_flags |= CFIF_DROP;

	p = current_proc();

	/*
	 * Force the socket to be marked defunct
	 * (forcing fixed along with rdar://19391339)
	 */
	if (so->so_flow_db == NULL) {
		error = sosetdefunct(p, so,
		    SHUTDOWN_SOCKET_LEVEL_CONTENT_FILTER | SHUTDOWN_SOCKET_LEVEL_DISCONNECT_ALL,
		    FALSE);

		/* Flush the socket buffer and disconnect */
		if (error == 0) {
			error = sodefunct(p, so,
			    SHUTDOWN_SOCKET_LEVEL_CONTENT_FILTER | SHUTDOWN_SOCKET_LEVEL_DISCONNECT_ALL);
		}
	}

	/* The filter is done, mark as detached */
	entry->cfe_flags |= CFEF_CFIL_DETACHED;

	if (cfil_info->cfi_debug) {
		cfil_info_log(LOG_ERR, cfil_info, "CFIL: DROP - DETACH");
	}

	CFIL_LOG(LOG_INFO, "so %llx detached %u",
	    (uint64_t)VM_KERNEL_ADDRPERM(so), kcunit);

	/* Pending data needs to go */
	cfil_flush_queues(so, cfil_info);

	if (cfil_info && (cfil_info->cfi_flags & CFIF_CLOSE_WAIT)) {
		if (cfil_filters_attached(so) == 0) {
			CFIL_LOG(LOG_INFO, "so %llx waking",
			    (uint64_t)VM_KERNEL_ADDRPERM(so));
			wakeup((caddr_t)cfil_info);
		}
	}
done:
	return error;
}

int
cfil_action_bless_client(uint32_t kcunit, struct cfil_msg_hdr *msghdr)
{
	errno_t error = 0;
	struct cfil_info * __single cfil_info = NULL;

	bool cfil_attached = false;
	struct cfil_msg_bless_client *blessmsg = (struct cfil_msg_bless_client *)msghdr;

	// Search and lock socket
	struct socket *so = cfil_socket_from_client_uuid(blessmsg->cfb_client_uuid, &cfil_attached);
	if (so == NULL) {
		error = ENOENT;
	} else {
		// The client gets a pass automatically
		cfil_info = (so->so_flow_db != NULL) ?
		    soflow_db_get_feature_context(so->so_flow_db, msghdr->cfm_sock_id) : so->so_cfil;

		if (cfil_attached) {
			if (cfil_info != NULL && cfil_info->cfi_debug) {
				cfil_info_log(LOG_ERR, cfil_info, "CFIL: VERDICT RECEIVED: BLESS");
			}
			cfil_sock_received_verdict(so);
			(void)cfil_action_data_pass(so, cfil_info, kcunit, 1, CFM_MAX_OFFSET, CFM_MAX_OFFSET);
			(void)cfil_action_data_pass(so, cfil_info, kcunit, 0, CFM_MAX_OFFSET, CFM_MAX_OFFSET);
		} else {
			so->so_flags1 |= SOF1_CONTENT_FILTER_SKIP;
		}
		socket_unlock(so, 1);
	}

	return error;
}

int
cfil_action_set_crypto_key(uint32_t kcunit, struct cfil_msg_hdr *msghdr)
{
	struct content_filter *cfc = NULL;
	cfil_crypto_state_t crypto_state = NULL;
	struct cfil_msg_set_crypto_key *keymsg = (struct cfil_msg_set_crypto_key *)msghdr;

	CFIL_LOG(LOG_NOTICE, "");

	if (kcunit > MAX_CONTENT_FILTER) {
		CFIL_LOG(LOG_ERR, "kcunit %u > MAX_CONTENT_FILTER (%d)",
		    kcunit, MAX_CONTENT_FILTER);
		return EINVAL;
	}
	crypto_state = cfil_crypto_init_client((uint8_t *)keymsg->crypto_key);
	if (crypto_state == NULL) {
		CFIL_LOG(LOG_ERR, "failed to initialize crypto state for unit %u)",
		    kcunit);
		return EINVAL;
	}

	cfil_rw_lock_exclusive(&cfil_lck_rw);

	cfc = content_filters[kcunit - 1];
	if (cfc->cf_kcunit != kcunit) {
		CFIL_LOG(LOG_ERR, "bad unit info %u)",
		    kcunit);
		cfil_rw_unlock_exclusive(&cfil_lck_rw);
		cfil_crypto_cleanup_state(crypto_state);
		return EINVAL;
	}
	if (cfc->cf_crypto_state != NULL) {
		cfil_crypto_cleanup_state(cfc->cf_crypto_state);
		cfc->cf_crypto_state = NULL;
	}
	cfc->cf_crypto_state = crypto_state;

	cfil_rw_unlock_exclusive(&cfil_lck_rw);
	return 0;
}

static int
cfil_update_entry_offsets(struct socket *so, struct cfil_info *cfil_info, int outgoing, unsigned int datalen)
{
	struct cfil_entry *entry;
	struct cfe_buf *entrybuf;
	uint32_t kcunit;

	CFIL_LOG(LOG_INFO, "so %llx outgoing %d datalen %u",
	    (uint64_t)VM_KERNEL_ADDRPERM(so), outgoing, datalen);

	for (kcunit = 1; kcunit <= MAX_CONTENT_FILTER; kcunit++) {
		entry = &cfil_info->cfi_entries[kcunit - 1];

		/* Are we attached to the filter? */
		if (entry->cfe_filter == NULL) {
			continue;
		}

		if (outgoing) {
			entrybuf = &entry->cfe_snd;
		} else {
			entrybuf = &entry->cfe_rcv;
		}

		entrybuf->cfe_ctl_q.q_start += datalen;
		if (entrybuf->cfe_pass_offset < entrybuf->cfe_ctl_q.q_start) {
			entrybuf->cfe_pass_offset = entrybuf->cfe_ctl_q.q_start;
		}
		entrybuf->cfe_peeked = entrybuf->cfe_ctl_q.q_start;
		if (entrybuf->cfe_peek_offset < entrybuf->cfe_pass_offset) {
			entrybuf->cfe_peek_offset = entrybuf->cfe_pass_offset;
		}

		entrybuf->cfe_ctl_q.q_end += datalen;

		entrybuf->cfe_pending_q.q_start += datalen;
		entrybuf->cfe_pending_q.q_end += datalen;
	}
	CFIL_INFO_VERIFY(cfil_info);
	return 0;
}

int
cfil_data_common(struct socket *so, struct cfil_info *cfil_info, int outgoing, struct sockaddr *to,
    struct mbuf *data, struct mbuf *control, uint32_t flags)
{
#pragma unused(to, control, flags)
	errno_t error = 0;
	unsigned int datalen;
	int mbcnt = 0;
	int mbnum = 0;
	int kcunit;
	struct cfi_buf *cfi_buf;
	struct mbuf *chain = NULL;

	if (cfil_info == NULL) {
		CFIL_LOG(LOG_ERR, "so %llx cfil detached",
		    (uint64_t)VM_KERNEL_ADDRPERM(so));
		error = 0;
		goto done;
	} else if (cfil_info->cfi_flags & CFIF_DROP) {
		CFIL_LOG(LOG_ERR, "so %llx drop set",
		    (uint64_t)VM_KERNEL_ADDRPERM(so));
		error = EPIPE;
		goto done;
	}

	datalen = cfil_data_length(data, &mbcnt, &mbnum);

	if (datalen == 0) {
		error = 0;
		goto done;
	}

	if (outgoing) {
		cfi_buf = &cfil_info->cfi_snd;
		cfil_info->cfi_byte_outbound_count += datalen;
	} else {
		cfi_buf = &cfil_info->cfi_rcv;
		cfil_info->cfi_byte_inbound_count += datalen;
	}

	cfi_buf->cfi_pending_last += datalen;
	cfi_buf->cfi_pending_mbcnt += mbcnt;
	cfi_buf->cfi_pending_mbnum += mbnum;

	if (NEED_DGRAM_FLOW_TRACKING(so)) {
		if (cfi_buf->cfi_pending_mbnum > cfil_udp_gc_mbuf_num_max ||
		    cfi_buf->cfi_pending_mbcnt > cfil_udp_gc_mbuf_cnt_max) {
			cfi_buf->cfi_tail_drop_cnt++;
			cfi_buf->cfi_pending_mbcnt -= mbcnt;
			cfi_buf->cfi_pending_mbnum -= mbnum;
			return EPIPE;
		}
	}

	cfil_info_buf_verify(cfi_buf);

	if (cfil_info->cfi_debug && cfil_log_data) {
		CFIL_LOG(LOG_ERR, "CFIL: QUEUEING DATA: <so %llx> %s: data %llx len %u flags 0x%x nextpkt %llx - cfi_pending_last %llu cfi_pending_mbcnt %u   cfi_pass_offset %llu",
		    (uint64_t)VM_KERNEL_ADDRPERM(so),
		    outgoing ? "OUT" : "IN",
		    (uint64_t)VM_KERNEL_ADDRPERM(data), datalen, data->m_flags,
		    (uint64_t)VM_KERNEL_ADDRPERM(data->m_nextpkt),
		    cfi_buf->cfi_pending_last,
		    cfi_buf->cfi_pending_mbcnt,
		    cfi_buf->cfi_pass_offset);
	}

	/* Fast path when below pass offset */
	if (cfi_buf->cfi_pending_last <= cfi_buf->cfi_pass_offset) {
		cfil_update_entry_offsets(so, cfil_info, outgoing, datalen);
		if (cfil_info->cfi_debug && cfil_log_data) {
			CFIL_LOG(LOG_ERR, "CFIL: QUEUEING DATA: <so %llx> %s: FAST PATH",
			    (uint64_t)VM_KERNEL_ADDRPERM(so),
			    outgoing ? "OUT" : "IN");
		}
		// For incoming packets, see if we need to strip off ip header
		if (!outgoing && NEED_DGRAM_FLOW_TRACKING(so) && OPTIONAL_IP_HEADER(so)) {
			cfil_strip_ip_header(cfil_info, data, so);
		}
	} else {
		struct cfil_entry *iter_entry;
		SLIST_FOREACH(iter_entry, &cfil_info->cfi_ordered_entries, cfe_order_link) {
			// Is cfil attached to this filter?
			kcunit = CFI_ENTRY_KCUNIT(cfil_info, iter_entry);
			if (IS_ENTRY_ATTACHED(cfil_info, kcunit)) {
				if (NEED_DGRAM_FLOW_TRACKING(so) && chain == NULL) {
					/* Datagrams only:
					 * Chain addr (incoming only TDB), control (optional) and data into one chain.
					 * This full chain will be reinjected into socket after recieving verdict.
					 */
					(void) cfil_dgram_save_socket_state(cfil_info, data);
					chain = sbconcat_mbufs(NULL, outgoing ? NULL : to, data, control);
					if (chain == NULL) {
						return ENOBUFS;
					}
					data = chain;
				}
				error = cfil_data_filter(so, cfil_info, kcunit, outgoing, data,
				    datalen);
			}
			/* 0 means passed so continue with next filter */
			if (error != 0) {
				break;
			}
		}
	}

	/* Move cursor if no filter claimed the data */
	if (error == 0) {
		cfi_buf->cfi_pending_first += datalen;
		cfi_buf->cfi_pending_mbcnt -= mbcnt;
		cfi_buf->cfi_pending_mbnum -= mbnum;
		cfil_info_buf_verify(cfi_buf);
	}
done:
	CFIL_INFO_VERIFY(cfil_info);

	return error;
}

/*
 * Callback from socket layer sosendxxx()
 */
int
cfil_sock_data_out(struct socket *so, struct sockaddr  *to,
    struct mbuf *data, struct mbuf *control, uint32_t flags, struct soflow_hash_entry *flow_entry)
{
	int error = 0;
	int new_filter_control_unit = 0;

	if (NEED_DGRAM_FLOW_TRACKING(so)) {
		return cfil_sock_udp_handle_data(TRUE, so, NULL, to, data, control, flags, flow_entry);
	}

	if ((so->so_flags & SOF_CONTENT_FILTER) == 0 || so->so_cfil == NULL) {
		/* Drop pre-existing TCP sockets if filter is enabled now */
		if (!DO_PRESERVE_CONNECTIONS && cfil_active_count > 0 && !SKIP_FILTER_FOR_TCP_SOCKET(so)) {
			new_filter_control_unit = necp_socket_get_content_filter_control_unit(so);
			if (new_filter_control_unit > 0) {
				cfil_log_printf(so, NULL, "CFIL: outbound TCP data dropped for pre-existing un-filtered flow");
				return EPIPE;
			}
		}
		return 0;
	}

	/* Drop pre-existing TCP sockets when filter state changed */
	new_filter_control_unit = necp_socket_get_content_filter_control_unit(so);
	if (new_filter_control_unit > 0 && new_filter_control_unit != so->so_cfil->cfi_filter_control_unit && !SKIP_FILTER_FOR_TCP_SOCKET(so)) {
		if (DO_PRESERVE_CONNECTIONS || (so->so_cfil->cfi_filter_policy_gencount == necp_socket_get_policy_gencount(so))) {
			// CFIL state has changed, but preserve the flow intentionally or if this is not a result of NECP policy change
			so->so_cfil->cfi_filter_control_unit = new_filter_control_unit;
		} else {
			cfil_log_printf(so, so->so_cfil, "CFIL: outbound TCP data dropped (0x%x -> 0x%x)",
			    so->so_cfil->cfi_filter_control_unit, new_filter_control_unit);
			return EPIPE;
		}
	}

	/*
	 * Pass initial data for TFO.
	 */
	if (IS_INITIAL_TFO_DATA(so)) {
		return 0;
	}

	socket_lock_assert_owned(so);

	if (so->so_cfil->cfi_flags & CFIF_DROP) {
		CFIL_LOG(LOG_ERR, "so %llx drop set",
		    (uint64_t)VM_KERNEL_ADDRPERM(so));
		return EPIPE;
	}
	if (control != NULL) {
		CFIL_LOG(LOG_ERR, "so %llx control",
		    (uint64_t)VM_KERNEL_ADDRPERM(so));
		OSIncrementAtomic(&cfil_stats.cfs_data_out_control);
	}
	if ((flags & MSG_OOB)) {
		CFIL_LOG(LOG_ERR, "so %llx MSG_OOB",
		    (uint64_t)VM_KERNEL_ADDRPERM(so));
		OSIncrementAtomic(&cfil_stats.cfs_data_out_oob);
	}
	/*
	 * Abort if socket is defunct.
	 */
	if (so->so_flags & SOF_DEFUNCT) {
		return EPIPE;
	}
	if ((so->so_snd.sb_flags & SB_LOCK) == 0) {
		panic("so %p SB_LOCK not set", so);
	}

	if (so->so_snd.sb_cfil_thread != NULL) {
		panic("%s sb_cfil_thread %p not NULL", __func__,
		    so->so_snd.sb_cfil_thread);
	}

	error = cfil_data_common(so, so->so_cfil, 1, to, data, control, flags);

	return error;
}

/*
 * Callback from socket layer sbappendxxx()
 */
int
cfil_sock_data_in(struct socket *so, struct sockaddr *from,
    struct mbuf *data, struct mbuf *control, uint32_t flags, struct soflow_hash_entry *flow_entry)
{
	int error = 0;
	int new_filter_control_unit = 0;

	if (NEED_DGRAM_FLOW_TRACKING(so)) {
		return cfil_sock_udp_handle_data(FALSE, so, NULL, from, data, control, flags, flow_entry);
	}

	if ((so->so_flags & SOF_CONTENT_FILTER) == 0 || so->so_cfil == NULL) {
		/* Drop pre-existing TCP sockets if filter is enabled now */
		if (!DO_PRESERVE_CONNECTIONS && cfil_active_count > 0 && !SKIP_FILTER_FOR_TCP_SOCKET(so)) {
			new_filter_control_unit = necp_socket_get_content_filter_control_unit(so);
			if (new_filter_control_unit > 0) {
				cfil_log_printf(so, NULL, "CFIL: inbound TCP data dropped for pre-existing un-filtered flow");
				return EPIPE;
			}
		}
		return 0;
	}

	/* Drop pre-existing TCP sockets when filter state changed */
	new_filter_control_unit = necp_socket_get_content_filter_control_unit(so);
	if (new_filter_control_unit > 0 && new_filter_control_unit != so->so_cfil->cfi_filter_control_unit && !SKIP_FILTER_FOR_TCP_SOCKET(so)) {
		if (DO_PRESERVE_CONNECTIONS || (so->so_cfil->cfi_filter_policy_gencount == necp_socket_get_policy_gencount(so))) {
			// CFIL state has changed, but preserve the flow intentionally or if this is not a result of NECP policy change
			so->so_cfil->cfi_filter_control_unit = new_filter_control_unit;
		} else {
			cfil_log_printf(so, so->so_cfil, "CFIL: inbound TCP data dropped (0x%x -> 0x%x)",
			    so->so_cfil->cfi_filter_control_unit, new_filter_control_unit);
			return EPIPE;
		}
	}

	/*
	 * Pass initial data for TFO.
	 */
	if (IS_INITIAL_TFO_DATA(so)) {
		return 0;
	}

	socket_lock_assert_owned(so);

	if (so->so_cfil->cfi_flags & CFIF_DROP) {
		CFIL_LOG(LOG_ERR, "so %llx drop set",
		    (uint64_t)VM_KERNEL_ADDRPERM(so));
		return EPIPE;
	}
	if (control != NULL) {
		CFIL_LOG(LOG_ERR, "so %llx control",
		    (uint64_t)VM_KERNEL_ADDRPERM(so));
		OSIncrementAtomic(&cfil_stats.cfs_data_in_control);
	}
	if (data->m_type == MT_OOBDATA) {
		CFIL_LOG(LOG_ERR, "so %llx MSG_OOB",
		    (uint64_t)VM_KERNEL_ADDRPERM(so));
		OSIncrementAtomic(&cfil_stats.cfs_data_in_oob);
	}
	error = cfil_data_common(so, so->so_cfil, 0, from, data, control, flags);

	return error;
}

/*
 * Callback from socket layer soshutdownxxx()
 *
 * We may delay the shutdown write if there's outgoing data in process.
 *
 * There is no point in delaying the shutdown read because the process
 * indicated that it does not want to read anymore data.
 */
int
cfil_sock_shutdown(struct socket *so, int *how)
{
	int error = 0;

	if (NEED_DGRAM_FLOW_TRACKING(so)) {
		return cfil_sock_udp_shutdown(so, how);
	}

	if ((so->so_flags & SOF_CONTENT_FILTER) == 0 || so->so_cfil == NULL) {
		goto done;
	}

	socket_lock_assert_owned(so);

	CFIL_LOG(LOG_INFO, "so %llx how %d",
	    (uint64_t)VM_KERNEL_ADDRPERM(so), *how);

	/*
	 * Check the state of the socket before the content filter
	 */
	if (*how != SHUT_WR && (so->so_state & SS_CANTRCVMORE) != 0) {
		/* read already shut down */
		error = ENOTCONN;
		goto done;
	}
	if (*how != SHUT_RD && (so->so_state & SS_CANTSENDMORE) != 0) {
		/* write already shut down */
		error = ENOTCONN;
		goto done;
	}

	if ((so->so_cfil->cfi_flags & CFIF_DROP) != 0) {
		CFIL_LOG(LOG_ERR, "so %llx drop set",
		    (uint64_t)VM_KERNEL_ADDRPERM(so));
		goto done;
	}

	/*
	 * shutdown read: SHUT_RD or SHUT_RDWR
	 */
	if (*how != SHUT_WR) {
		if (so->so_cfil->cfi_flags & CFIF_SHUT_RD) {
			error = ENOTCONN;
			goto done;
		}
		so->so_cfil->cfi_flags |= CFIF_SHUT_RD;
		cfil_sock_notify_shutdown(so, SHUT_RD);
	}
	/*
	 * shutdown write: SHUT_WR or SHUT_RDWR
	 */
	if (*how != SHUT_RD) {
		if (so->so_cfil->cfi_flags & CFIF_SHUT_WR) {
			error = ENOTCONN;
			goto done;
		}
		so->so_cfil->cfi_flags |= CFIF_SHUT_WR;
		cfil_sock_notify_shutdown(so, SHUT_WR);
		/*
		 * When outgoing data is pending, we delay the shutdown at the
		 * protocol level until the content filters give the final
		 * verdict on the pending data.
		 */
		if (cfil_sock_data_pending(&so->so_snd) != 0) {
			/*
			 * When shutting down the read and write sides at once
			 * we can proceed to the final shutdown of the read
			 * side. Otherwise, we just return.
			 */
			if (*how == SHUT_WR) {
				error = EJUSTRETURN;
			} else if (*how == SHUT_RDWR) {
				*how = SHUT_RD;
			}
		}
	}
done:
	return error;
}

/*
 * This is called when the socket is closed and there is no more
 * opportunity for filtering
 */
void
cfil_sock_is_closed(struct socket *so)
{
	errno_t error = 0;
	int kcunit;

	if (NEED_DGRAM_FLOW_TRACKING(so)) {
		cfil_sock_udp_is_closed(so);
		return;
	}

	if ((so->so_flags & SOF_CONTENT_FILTER) == 0 || so->so_cfil == NULL) {
		return;
	}

	CFIL_LOG(LOG_INFO, "so %llx", (uint64_t)VM_KERNEL_ADDRPERM(so));

	socket_lock_assert_owned(so);

	for (kcunit = 1; kcunit <= MAX_CONTENT_FILTER; kcunit++) {
		/* Let the filters know of the closing */
		error = cfil_dispatch_closed_event(so, so->so_cfil, kcunit);
	}

	/* Last chance to push passed data out */
	error = cfil_acquire_sockbuf(so, so->so_cfil, 1);
	if (error == 0) {
		cfil_service_inject_queue(so, so->so_cfil, 1);
	}
	cfil_release_sockbuf(so, 1);

	if (so->so_cfil != NULL) {
		so->so_cfil->cfi_flags |= CFIF_SOCK_CLOSED;
	}

	/* Pending data needs to go */
	cfil_flush_queues(so, so->so_cfil);

	CFIL_INFO_VERIFY(so->so_cfil);
}

/*
 * This is called when the socket is disconnected so let the filters
 * know about the disconnection and that no more data will come
 *
 * The how parameter has the same values as soshutown()
 */
void
cfil_sock_notify_shutdown(struct socket *so, int how)
{
	errno_t error = 0;
	int kcunit;

	if (NEED_DGRAM_FLOW_TRACKING(so)) {
		cfil_sock_udp_notify_shutdown(so, how, 0, 0);
		return;
	}

	if ((so->so_flags & SOF_CONTENT_FILTER) == 0 || so->so_cfil == NULL) {
		return;
	}

	CFIL_LOG(LOG_INFO, "so %llx how %d",
	    (uint64_t)VM_KERNEL_ADDRPERM(so), how);

	socket_lock_assert_owned(so);

	for (kcunit = 1; kcunit <= MAX_CONTENT_FILTER; kcunit++) {
		/* Disconnect incoming side */
		if (how != SHUT_WR) {
			error = cfil_dispatch_disconnect_event(so, so->so_cfil, kcunit, 0);
		}
		/* Disconnect outgoing side */
		if (how != SHUT_RD) {
			error = cfil_dispatch_disconnect_event(so, so->so_cfil, kcunit, 1);
		}
	}
}

static int
cfil_filters_attached(struct socket *so)
{
	struct cfil_entry *entry;
	uint32_t kcunit;
	int attached = 0;

	if (NEED_DGRAM_FLOW_TRACKING(so)) {
		return cfil_filters_udp_attached(so, FALSE);
	}

	if ((so->so_flags & SOF_CONTENT_FILTER) == 0 || so->so_cfil == NULL) {
		return 0;
	}

	socket_lock_assert_owned(so);

	for (kcunit = 1; kcunit <= MAX_CONTENT_FILTER; kcunit++) {
		entry = &so->so_cfil->cfi_entries[kcunit - 1];

		/* Are we attached to the filter? */
		if (entry->cfe_filter == NULL) {
			continue;
		}
		if ((entry->cfe_flags & CFEF_SENT_SOCK_ATTACHED) == 0) {
			continue;
		}
		if ((entry->cfe_flags & CFEF_CFIL_DETACHED) != 0) {
			continue;
		}
		attached = 1;
		break;
	}

	return attached;
}

/*
 * This is called when the socket is closed and we are waiting for
 * the filters to gives the final pass or drop
 */
void
cfil_sock_close_wait(struct socket *so)
{
	lck_mtx_t *mutex_held;
	struct timespec ts;
	int error;

	if (NEED_DGRAM_FLOW_TRACKING(so)) {
		cfil_sock_udp_close_wait(so);
		return;
	}

	if ((so->so_flags & SOF_CONTENT_FILTER) == 0 || so->so_cfil == NULL) {
		return;
	}

	// This flow does not need to wait for close ack from user-space
	if (IS_NO_CLOSE_WAIT(so->so_cfil)) {
		if (so->so_cfil->cfi_debug) {
			cfil_info_log(LOG_ERR, so->so_cfil, "CFIL: SKIP CLOSE WAIT");
		}
		return;
	}

	CFIL_LOG(LOG_INFO, "so %llx", (uint64_t)VM_KERNEL_ADDRPERM(so));

	if (so->so_proto->pr_getlock != NULL) {
		mutex_held = (*so->so_proto->pr_getlock)(so, PR_F_WILLUNLOCK);
	} else {
		mutex_held = so->so_proto->pr_domain->dom_mtx;
	}
	LCK_MTX_ASSERT(mutex_held, LCK_MTX_ASSERT_OWNED);

	while (cfil_filters_attached(so)) {
		/*
		 * Notify the filters we are going away so they can detach
		 */
		cfil_sock_notify_shutdown(so, SHUT_RDWR);

		/*
		 * Make sure we need to wait after the filter are notified
		 * of the disconnection
		 */
		if (cfil_filters_attached(so) == 0) {
			break;
		}

		CFIL_LOG(LOG_INFO, "so %llx waiting",
		    (uint64_t)VM_KERNEL_ADDRPERM(so));

		ts.tv_sec = cfil_close_wait_timeout / 1000;
		ts.tv_nsec = (cfil_close_wait_timeout % 1000) *
		    NSEC_PER_USEC * 1000;

		OSIncrementAtomic(&cfil_stats.cfs_close_wait);
		so->so_cfil->cfi_flags |= CFIF_CLOSE_WAIT;
		error = msleep((caddr_t)so->so_cfil, mutex_held,
		    PSOCK | PCATCH, "cfil_sock_close_wait", &ts);

		// Woke up from sleep, validate if cfil_info is still valid
		if (so->so_cfil == NULL) {
			// cfil_info is not valid, do not continue
			return;
		}

		so->so_cfil->cfi_flags &= ~CFIF_CLOSE_WAIT;

		CFIL_LOG(LOG_NOTICE, "so %llx timed out %d",
		    (uint64_t)VM_KERNEL_ADDRPERM(so), (error != 0));

		/*
		 * Force close in case of timeout
		 */
		if (error != 0) {
			OSIncrementAtomic(&cfil_stats.cfs_close_wait_timeout);
			break;
		}
	}
}

/*
 * Returns the size of the data held by the content filter by using
 */
int32_t
cfil_sock_data_pending(struct sockbuf *sb)
{
	struct socket *so = sb->sb_so;
	uint64_t pending = 0;

	if (NEED_DGRAM_FLOW_TRACKING(so)) {
		return cfil_sock_udp_data_pending(sb, FALSE);
	}

	if ((so->so_flags & SOF_CONTENT_FILTER) != 0 && so->so_cfil != NULL) {
		struct cfi_buf *cfi_buf;

		socket_lock_assert_owned(so);

		if ((sb->sb_flags & SB_RECV) == 0) {
			cfi_buf = &so->so_cfil->cfi_snd;
		} else {
			cfi_buf = &so->so_cfil->cfi_rcv;
		}

		pending = cfi_buf->cfi_pending_last -
		    cfi_buf->cfi_pending_first;

		/*
		 * If we are limited by the "chars of mbufs used" roughly
		 * adjust so we won't overcommit
		 */
		if (pending > (uint64_t)cfi_buf->cfi_pending_mbcnt) {
			pending = cfi_buf->cfi_pending_mbcnt;
		}
	}

	VERIFY(pending < INT32_MAX);

	return (int32_t)(pending);
}

/*
 * Return the socket buffer space used by data being held by content filters
 * so processes won't clog the socket buffer
 */
int32_t
cfil_sock_data_space(struct sockbuf *sb)
{
	struct socket *so = sb->sb_so;
	uint64_t pending = 0;

	if (NEED_DGRAM_FLOW_TRACKING(so)) {
		return cfil_sock_udp_data_pending(sb, TRUE);
	}

	if ((so->so_flags & SOF_CONTENT_FILTER) != 0 && so->so_cfil != NULL &&
	    so->so_snd.sb_cfil_thread != current_thread()) {
		struct cfi_buf *cfi_buf;

		socket_lock_assert_owned(so);

		if ((sb->sb_flags & SB_RECV) == 0) {
			cfi_buf = &so->so_cfil->cfi_snd;
		} else {
			cfi_buf = &so->so_cfil->cfi_rcv;
		}

		pending = cfi_buf->cfi_pending_last -
		    cfi_buf->cfi_pending_first;

		/*
		 * If we are limited by the "chars of mbufs used" roughly
		 * adjust so we won't overcommit
		 */
		if ((uint64_t)cfi_buf->cfi_pending_mbcnt > pending) {
			pending = cfi_buf->cfi_pending_mbcnt;
		}

		VERIFY(pending < INT32_MAX);
	}

	return (int32_t)(pending);
}

/*
 * A callback from the socket and protocol layer when data becomes
 * available in the socket buffer to give a chance for the content filter
 * to re-inject data that was held back
 */
void
cfil_sock_buf_update(struct sockbuf *sb)
{
	int outgoing;
	int error;
	struct socket *so = sb->sb_so;

	if (NEED_DGRAM_FLOW_TRACKING(so)) {
		cfil_sock_udp_buf_update(sb);
		return;
	}

	if ((so->so_flags & SOF_CONTENT_FILTER) == 0 || so->so_cfil == NULL) {
		return;
	}

	if (!cfil_sbtrim) {
		return;
	}

	socket_lock_assert_owned(so);

	if ((sb->sb_flags & SB_RECV) == 0) {
		if ((so->so_cfil->cfi_flags & CFIF_RETRY_INJECT_OUT) == 0) {
			return;
		}
		outgoing = 1;
		OSIncrementAtomic(&cfil_stats.cfs_inject_q_out_retry);
	} else {
		if ((so->so_cfil->cfi_flags & CFIF_RETRY_INJECT_IN) == 0) {
			return;
		}
		outgoing = 0;
		OSIncrementAtomic(&cfil_stats.cfs_inject_q_in_retry);
	}

	CFIL_LOG(LOG_NOTICE, "so %llx outgoing %d",
	    (uint64_t)VM_KERNEL_ADDRPERM(so), outgoing);

	error = cfil_acquire_sockbuf(so, so->so_cfil, outgoing);
	if (error == 0) {
		cfil_service_inject_queue(so, so->so_cfil, outgoing);
	}
	cfil_release_sockbuf(so, outgoing);
}

int
sysctl_cfil_filter_list(struct sysctl_oid *oidp, void *arg1, int arg2,
    struct sysctl_req *req)
{
#pragma unused(oidp, arg1, arg2)
	int error = 0;
	size_t len = 0;
	u_int32_t i;

	/* Read only  */
	if (req->newptr != USER_ADDR_NULL) {
		return EPERM;
	}

	cfil_rw_lock_shared(&cfil_lck_rw);

	for (i = 0; i < MAX_CONTENT_FILTER; i++) {
		struct cfil_filter_stat filter_stat;
		struct content_filter *cfc = content_filters[i];

		if (cfc == NULL) {
			continue;
		}

		/* If just asking for the size */
		if (req->oldptr == USER_ADDR_NULL) {
			len += sizeof(struct cfil_filter_stat);
			continue;
		}

		bzero(&filter_stat, sizeof(struct cfil_filter_stat));
		filter_stat.cfs_len = sizeof(struct cfil_filter_stat);
		filter_stat.cfs_filter_id = cfc->cf_kcunit;
		filter_stat.cfs_flags = cfc->cf_flags;
		filter_stat.cfs_sock_count = cfc->cf_sock_count;
		filter_stat.cfs_necp_control_unit = cfc->cf_necp_control_unit;

		error = SYSCTL_OUT(req, &filter_stat,
		    sizeof(struct cfil_filter_stat));
		if (error != 0) {
			break;
		}
	}
	/* If just asking for the size */
	if (req->oldptr == USER_ADDR_NULL) {
		req->oldidx = len;
	}

	cfil_rw_unlock_shared(&cfil_lck_rw);

	if (cfil_log_level >= LOG_DEBUG) {
		if (req->oldptr != USER_ADDR_NULL) {
			for (i = 1; i <= MAX_CONTENT_FILTER; i++) {
				cfil_filter_show(i);
			}
		}
	}

	return error;
}

static int
sysctl_cfil_sock_list(struct sysctl_oid *oidp, void *arg1, int arg2,
    struct sysctl_req *req)
{
#pragma unused(oidp, arg1, arg2)
	int error = 0;
	u_int32_t i;
	struct cfil_info *cfi;

	/* Read only  */
	if (req->newptr != USER_ADDR_NULL) {
		return EPERM;
	}

	cfil_rw_lock_shared(&cfil_lck_rw);

	/*
	 * If just asking for the size,
	 */
	if (req->oldptr == USER_ADDR_NULL) {
		req->oldidx = cfil_sock_attached_count *
		    sizeof(struct cfil_sock_stat);
		/* Bump the length in case new sockets gets attached */
		req->oldidx += req->oldidx >> 3;
		goto done;
	}

	TAILQ_FOREACH(cfi, &cfil_sock_head, cfi_link) {
		struct cfil_entry *entry;
		struct cfil_sock_stat stat;
		struct socket *so = cfi->cfi_so;

		bzero(&stat, sizeof(struct cfil_sock_stat));
		stat.cfs_len = sizeof(struct cfil_sock_stat);
		stat.cfs_sock_id = cfi->cfi_sock_id;
		stat.cfs_flags = cfi->cfi_flags;

		if (so != NULL && so->so_proto != NULL && so->so_proto->pr_domain != NULL) {
			stat.cfs_pid = so->last_pid;
			memcpy(stat.cfs_uuid, so->last_uuid,
			    sizeof(uuid_t));
			if (so->so_flags & SOF_DELEGATED) {
				stat.cfs_e_pid = so->e_pid;
				memcpy(stat.cfs_e_uuid, so->e_uuid,
				    sizeof(uuid_t));
			} else {
				stat.cfs_e_pid = so->last_pid;
				memcpy(stat.cfs_e_uuid, so->last_uuid,
				    sizeof(uuid_t));
			}

			stat.cfs_sock_family = SOCK_DOM(so);
			stat.cfs_sock_type = SOCK_TYPE(so);
			stat.cfs_sock_protocol = GET_SO_PROTO(so);
		}

		stat.cfs_snd.cbs_pending_first =
		    cfi->cfi_snd.cfi_pending_first;
		stat.cfs_snd.cbs_pending_last =
		    cfi->cfi_snd.cfi_pending_last;
		stat.cfs_snd.cbs_inject_q_len =
		    cfil_queue_len(&cfi->cfi_snd.cfi_inject_q);
		stat.cfs_snd.cbs_pass_offset =
		    cfi->cfi_snd.cfi_pass_offset;

		stat.cfs_rcv.cbs_pending_first =
		    cfi->cfi_rcv.cfi_pending_first;
		stat.cfs_rcv.cbs_pending_last =
		    cfi->cfi_rcv.cfi_pending_last;
		stat.cfs_rcv.cbs_inject_q_len =
		    cfil_queue_len(&cfi->cfi_rcv.cfi_inject_q);
		stat.cfs_rcv.cbs_pass_offset =
		    cfi->cfi_rcv.cfi_pass_offset;

		for (i = 0; i < MAX_CONTENT_FILTER; i++) {
			struct cfil_entry_stat *estat;
			struct cfe_buf *ebuf;
			struct cfe_buf_stat *sbuf;

			entry = &cfi->cfi_entries[i];

			estat = &stat.ces_entries[i];

			estat->ces_len = sizeof(struct cfil_entry_stat);
			estat->ces_filter_id = entry->cfe_filter ?
			    entry->cfe_filter->cf_kcunit : 0;
			estat->ces_flags = entry->cfe_flags;
			estat->ces_necp_control_unit =
			    entry->cfe_necp_control_unit;

			estat->ces_last_event.tv_sec =
			    (int64_t)entry->cfe_last_event.tv_sec;
			estat->ces_last_event.tv_usec =
			    (int64_t)entry->cfe_last_event.tv_usec;

			estat->ces_last_action.tv_sec =
			    (int64_t)entry->cfe_last_action.tv_sec;
			estat->ces_last_action.tv_usec =
			    (int64_t)entry->cfe_last_action.tv_usec;

			ebuf = &entry->cfe_snd;
			sbuf = &estat->ces_snd;
			sbuf->cbs_pending_first =
			    cfil_queue_offset_first(&ebuf->cfe_pending_q);
			sbuf->cbs_pending_last =
			    cfil_queue_offset_last(&ebuf->cfe_pending_q);
			sbuf->cbs_ctl_first =
			    cfil_queue_offset_first(&ebuf->cfe_ctl_q);
			sbuf->cbs_ctl_last =
			    cfil_queue_offset_last(&ebuf->cfe_ctl_q);
			sbuf->cbs_pass_offset =  ebuf->cfe_pass_offset;
			sbuf->cbs_peek_offset =  ebuf->cfe_peek_offset;
			sbuf->cbs_peeked =  ebuf->cfe_peeked;

			ebuf = &entry->cfe_rcv;
			sbuf = &estat->ces_rcv;
			sbuf->cbs_pending_first =
			    cfil_queue_offset_first(&ebuf->cfe_pending_q);
			sbuf->cbs_pending_last =
			    cfil_queue_offset_last(&ebuf->cfe_pending_q);
			sbuf->cbs_ctl_first =
			    cfil_queue_offset_first(&ebuf->cfe_ctl_q);
			sbuf->cbs_ctl_last =
			    cfil_queue_offset_last(&ebuf->cfe_ctl_q);
			sbuf->cbs_pass_offset =  ebuf->cfe_pass_offset;
			sbuf->cbs_peek_offset =  ebuf->cfe_peek_offset;
			sbuf->cbs_peeked =  ebuf->cfe_peeked;
		}
		error = SYSCTL_OUT(req, &stat,
		    sizeof(struct cfil_sock_stat));
		if (error != 0) {
			break;
		}
	}
done:
	cfil_rw_unlock_shared(&cfil_lck_rw);

	if (cfil_log_level >= LOG_DEBUG) {
		if (req->oldptr != USER_ADDR_NULL) {
			cfil_info_show();
		}
	}

	return error;
}

/*
 * UDP Socket Support
 */
static void
cfil_hash_entry_log(int level, struct socket *so, struct soflow_hash_entry *entry, uint64_t sockId, const char* msg)
{
	char local[MAX_IPv6_STR_LEN + 6] = {};
	char remote[MAX_IPv6_STR_LEN + 6] = {};
	const void  *addr;
	proc_t sock_proc = NULL;
	pid_t sock_pid = 0;
	struct timeval current_ts = {};
	struct timeval diff_time = {};

	// No sock or not UDP, no-op
	if (so == NULL || entry == NULL) {
		return;
	}

	local[0] = remote[0] = 0x0;

	switch (entry->soflow_family) {
	case AF_INET6:
		addr = &entry->soflow_laddr.addr6;
		inet_ntop(AF_INET6, addr, local, sizeof(local));
		addr = &entry->soflow_faddr.addr6;
		inet_ntop(AF_INET6, addr, remote, sizeof(local));
		break;
	case AF_INET:
		addr = &entry->soflow_laddr.addr46.ia46_addr4.s_addr;
		inet_ntop(AF_INET, addr, local, sizeof(local));
		addr = &entry->soflow_faddr.addr46.ia46_addr4.s_addr;
		inet_ntop(AF_INET, addr, remote, sizeof(local));
		break;
	default:
		return;
	}

	sock_pid = SOCKET_PID(so);
	sock_proc = proc_find(sock_pid);

	microuptime(&current_ts);
	timersub(&current_ts, &entry->soflow_timestamp, &diff_time);

	CFIL_LOG(level, "<%s>: [%d %s] <%s(%d) %s so %llx %llu %llu age %ld> lport %d fport %d laddr %s faddr %s hash %X",
	    msg, sock_pid, (sock_proc != NULL ? proc_best_name(sock_proc) : ""),
	    IS_UDP(so) ? "UDP" : "proto", GET_SO_PROTO(so), (entry->soflow_outgoing ? "out" : "in"),
	    (uint64_t)VM_KERNEL_ADDRPERM(so), sockId, entry->soflow_feat_ctxt_id, diff_time.tv_sec,
	    ntohs(entry->soflow_lport), ntohs(entry->soflow_fport), local, remote,
	    entry->soflow_flowhash);

	if (sock_proc != NULL) {
		proc_rele(sock_proc);
	}
}

static void
cfil_inp_log(int level, struct socket *so, const char* msg)
{
	struct inpcb *inp = NULL;
	struct sockaddr_in *sin = NULL;
	struct sockaddr_in6 *sin6 = NULL;
	char local[MAX_IPv6_STR_LEN + 6];
	char remote[MAX_IPv6_STR_LEN + 6];
	ushort lport = 0;
	ushort fport = 0;
	const void  *addr;
	proc_t sock_proc = NULL;
	pid_t sock_pid = 0;

	if (so == NULL) {
		return;
	}

	inp = sotoinpcb(so);
	if (inp == NULL) {
		return;
	}

	local[0] = remote[0] = 0x0;

	if (inp->inp_vflag & INP_IPV6) {
		addr = &inp->in6p_laddr.s6_addr32;
		inet_ntop(AF_INET6, addr, local, sizeof(local));
		addr = &inp->in6p_faddr.s6_addr32;
		inet_ntop(AF_INET6, addr, remote, sizeof(remote));
	} else {
		addr = &inp->inp_laddr.s_addr;
		inet_ntop(AF_INET, addr, local, sizeof(local));
		addr = &inp->inp_faddr.s_addr;
		inet_ntop(AF_INET, addr, remote, sizeof(remote));
	}
	lport = inp->inp_lport;
	fport = inp->inp_fport;

	if (so->so_cfil && so->so_cfil->cfi_so_attach_faddr.sa.sa_len > 0) {
		if (so->so_cfil->cfi_so_attach_faddr.sa.sa_family == AF_INET6) {
			sin6 = SIN6(&so->so_cfil->cfi_so_attach_faddr.sa);
			addr = &sin6->sin6_addr;
			inet_ntop(AF_INET6, addr, remote, sizeof(remote));
			fport = sin6->sin6_port;
		} else if (so->so_cfil->cfi_so_attach_faddr.sa.sa_family == AF_INET) {
			sin = SIN(&so->so_cfil->cfi_so_attach_faddr.sa);
			addr = &sin->sin_addr.s_addr;
			inet_ntop(AF_INET, addr, remote, sizeof(remote));
			fport = sin->sin_port;
		}
	}
	if (so->so_cfil && so->so_cfil->cfi_so_attach_laddr.sa.sa_len > 0) {
		if (so->so_cfil->cfi_so_attach_laddr.sa.sa_family == AF_INET6) {
			sin6 = SIN6(&so->so_cfil->cfi_so_attach_laddr.sa);
			addr = &sin6->sin6_addr;
			inet_ntop(AF_INET6, addr, local, sizeof(remote));
			fport = sin6->sin6_port;
		} else if (so->so_cfil->cfi_so_attach_laddr.sa.sa_family == AF_INET) {
			sin = SIN(&so->so_cfil->cfi_so_attach_laddr.sa);
			addr = &sin->sin_addr.s_addr;
			inet_ntop(AF_INET, addr, local, sizeof(remote));
			fport = sin->sin_port;
		}
	}

	sock_pid = SOCKET_PID(so);
	sock_proc = proc_find(sock_pid);

	if (so->so_cfil != NULL) {
		struct timeval current_ts = {};
		struct timeval diff_time = {};

		microuptime(&current_ts);
		timersub(&current_ts, &(so->so_cfil->cfi_timestamp), &diff_time);

		CFIL_LOG(level, "<%s>: [%d %s] <%s %s so %llx flags 0x%x 0x%x %llu age %ld> lport %d fport %d laddr %s faddr %s",
		    msg, sock_pid, (sock_proc != NULL ? proc_best_name(sock_proc) : ""), IS_UDP(so) ? "UDP" : "TCP", (so->so_cfil->cfi_dir == CFS_CONNECTION_DIR_IN ? "in" : "out"),
		    (uint64_t)VM_KERNEL_ADDRPERM(so), inp->inp_flags, inp->inp_socket->so_flags, so->so_cfil->cfi_sock_id, diff_time.tv_sec,
		    ntohs(lport), ntohs(fport), local, remote);
	} else {
		CFIL_LOG(level, "<%s>: [%d %s] <%s %s so %llx - flags 0x%x 0x%x> lport %d fport %d laddr %s faddr %s",
		    msg, sock_pid, (sock_proc != NULL ? proc_best_name(sock_proc) : ""), IS_UDP(so) ? "UDP" : "TCP", (so->so_flags1 & SOF1_INBOUND) ? "in" : "out",
		    (uint64_t)VM_KERNEL_ADDRPERM(so), inp->inp_flags, inp->inp_socket->so_flags,
		    ntohs(lport), ntohs(fport), local, remote);
	}

	if (sock_proc != NULL) {
		proc_rele(sock_proc);
	}
}

static void
cfil_info_log(int level, struct cfil_info *cfil_info, const char* msg)
{
	if (cfil_info == NULL) {
		return;
	}

	if (cfil_info->cfi_hash_entry != NULL) {
		cfil_hash_entry_log(level, cfil_info->cfi_so, cfil_info->cfi_hash_entry, cfil_info->cfi_sock_id, msg);
	} else {
		cfil_inp_log(level, cfil_info->cfi_so, msg);
	}
}

void
cfil_log_printf(struct socket *so, struct cfil_info *info, const char *format, ...)
{
	va_list args;
	char buffer[256] = {};

	va_start(args, format);
	vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);

	if (info != NULL) {
		cfil_info_log(LOG_ERR, info, __unsafe_null_terminated_from_indexable(buffer));
	} else if (so != NULL) {
		cfil_inp_log(LOG_ERR, so, __unsafe_null_terminated_from_indexable(buffer));
	}
}

static void
cfil_sock_udp_unlink_flow(struct socket *so, struct soflow_hash_entry *hash_entry, struct cfil_info *cfil_info)
{
	if (so == NULL || hash_entry == NULL || cfil_info == NULL) {
		return;
	}

	if (so->so_flags & SOF_CONTENT_FILTER) {
		VERIFY(so->so_usecount > 0);
		so->so_usecount--;
	}

	// Hold exclusive lock before clearing cfil_info hash entry link
	cfil_rw_lock_exclusive(&cfil_lck_rw);

	cfil_info->cfi_hash_entry = NULL;

	if (cfil_info->cfi_debug) {
		CFIL_LOG(LOG_ERR, "CFIL <%s>: <so %llx> - use count %d",
		    IS_UDP(so) ? "UDP" : "TCP", (uint64_t)VM_KERNEL_ADDRPERM(so), so->so_usecount);
	}

	cfil_rw_unlock_exclusive(&cfil_lck_rw);
}

bool
check_port(struct sockaddr *addr, u_short port)
{
	struct sockaddr_in *sin = NULL;
	struct sockaddr_in6 *sin6 = NULL;

	if (addr == NULL || port == 0) {
		return FALSE;
	}

	switch (addr->sa_family) {
	case AF_INET:
		sin = SIN(addr);
		if (sin->sin_len < sizeof(*sin)) {
			return FALSE;
		}
		if (port == ntohs(sin->sin_port)) {
			return TRUE;
		}
		break;
	case AF_INET6:
		sin6 = SIN6(addr);
		if (sin6->sin6_len < sizeof(*sin6)) {
			return FALSE;
		}
		if (port == ntohs(sin6->sin6_port)) {
			return TRUE;
		}
		break;
	default:
		break;
	}
	return FALSE;
}

cfil_sock_id_t
cfil_sock_id_from_datagram_socket(struct socket *so, struct sockaddr *local, struct sockaddr *remote)
{
	socket_lock_assert_owned(so);

	if (so->so_flow_db == NULL) {
		return CFIL_SOCK_ID_NONE;
	}
	return (cfil_sock_id_t)soflow_db_get_feature_context_id(so->so_flow_db, local, remote);
}

static struct cfil_info *
cfil_sock_udp_get_info(struct socket *so, uint32_t filter_control_unit, bool outgoing, struct soflow_hash_entry *hash_entry,
    struct sockaddr *local, struct sockaddr *remote)
{
	int new_filter_control_unit = 0;
	struct cfil_info *cfil_info = NULL;

	errno_t error = 0;
	socket_lock_assert_owned(so);

	if (hash_entry == NULL || hash_entry->soflow_db == NULL) {
		return NULL;
	}

	if (hash_entry->soflow_feat_ctxt != NULL && hash_entry->soflow_feat_ctxt_id != 0) {
		/* Drop pre-existing UDP flow if filter state changed */
		cfil_info = (struct cfil_info *) hash_entry->soflow_feat_ctxt;
		new_filter_control_unit = filter_control_unit;
		if (new_filter_control_unit > 0 && new_filter_control_unit != cfil_info->cfi_filter_control_unit) {
			if (cfil_info->cfi_filter_policy_gencount == hash_entry->soflow_policies_gencount) {
				CFIL_LOG(LOG_NOTICE, "CFIL: UDP(%s) <so %llx> - filter state changed to 0x%x, but policies did not. Keeping existing filter state 0x%x",
				    outgoing ? "OUT" : "IN", (uint64_t)VM_KERNEL_ADDRPERM(so), new_filter_control_unit, cfil_info->cfi_filter_control_unit);
			} else if (DO_PRESERVE_CONNECTIONS) {
				CFIL_LOG(LOG_NOTICE, "CFIL: UDP(%s) <so %llx> - filter state changed from 0x%x to 0x%x, but existing connections are to be preserved",
				    outgoing ? "OUT" : "IN", (uint64_t)VM_KERNEL_ADDRPERM(so), cfil_info->cfi_filter_control_unit, new_filter_control_unit);
				// CFIL state has changed, but preserve the flow intentionally because preserve existing connections is in effect.
				cfil_info->cfi_filter_control_unit = new_filter_control_unit;
			} else {
				cfil_log_printf(so, cfil_info, "CFIL: %sbound UDP data dropped (0x%x -> 0x%x)",
				    (outgoing ? "out" : "in"), cfil_info->cfi_filter_control_unit, new_filter_control_unit);
				return NULL;
			}
		}
		return cfil_info;
	}

	cfil_info = cfil_info_alloc(so, hash_entry);
	if (cfil_info == NULL) {
		CFIL_LOG(LOG_ERR, "CFIL: <so %llx> UDP failed to alloc cfil_info", (uint64_t)VM_KERNEL_ADDRPERM(so));
		OSIncrementAtomic(&cfil_stats.cfs_sock_attach_no_mem);
		return NULL;
	}
	cfil_info->cfi_filter_control_unit = filter_control_unit;
	cfil_info->cfi_filter_policy_gencount = hash_entry->soflow_policies_gencount;
	cfil_info->cfi_dir = outgoing ? CFS_CONNECTION_DIR_OUT : CFS_CONNECTION_DIR_IN;
	cfil_info->cfi_debug = DEBUG_FLOW(sotoinpcb(so), so, local, remote);
	if (cfil_info->cfi_debug) {
		CFIL_LOG(LOG_ERR, "CFIL: <so %llx> UDP (outgoing %d) - debug flow with port %d", (uint64_t)VM_KERNEL_ADDRPERM(so), outgoing, cfil_log_port);
		CFIL_LOG(LOG_ERR, "CFIL: <so %llx> UDP so_gencnt %llx entry flowhash %x cfil %p sockID %llu <%llx>",
		    (uint64_t)VM_KERNEL_ADDRPERM(so), so->so_gencnt, hash_entry->soflow_flowhash, cfil_info, cfil_info->cfi_sock_id, cfil_info->cfi_sock_id);
	}

	if (cfil_info_attach_unit(so, filter_control_unit, cfil_info) == 0) {
		CFIL_INFO_FREE(cfil_info);
		CFIL_LOG(LOG_ERR, "CFIL: <so %llx> UDP cfil_info_attach_unit(%u) failed",
		    (uint64_t)VM_KERNEL_ADDRPERM(so), filter_control_unit);
		OSIncrementAtomic(&cfil_stats.cfs_sock_attach_failed);
		return NULL;
	}

	if (cfil_info->cfi_debug) {
		CFIL_LOG(LOG_ERR, "CFIL: UDP <so %llx> filter_control_unit %u sockID %llu <%llx> attached",
		    (uint64_t)VM_KERNEL_ADDRPERM(so),
		    filter_control_unit, cfil_info->cfi_sock_id, cfil_info->cfi_sock_id);
	}

	so->so_flags |= SOF_CONTENT_FILTER;
	OSIncrementAtomic(&cfil_stats.cfs_sock_attached);

	/* Hold a reference on the socket for each flow */
	so->so_usecount++;

	/* link cfil_info to flow */
	hash_entry->soflow_feat_ctxt = cfil_info;
	hash_entry->soflow_feat_ctxt_id = cfil_info->cfi_sock_id;

	if (cfil_info->cfi_debug) {
		cfil_info_log(LOG_ERR, cfil_info, "CFIL: ADDED");
	}

	error = cfil_dispatch_attach_event(so, cfil_info, 0,
	    outgoing ? CFS_CONNECTION_DIR_OUT : CFS_CONNECTION_DIR_IN);
	/* We can recover from flow control or out of memory errors */
	if (error != 0 && error != ENOBUFS && error != ENOMEM) {
		CFIL_LOG(LOG_ERR, "CFIL: UDP <so %llx> cfil_dispatch_attach_event failed <error %d>",
		    (uint64_t)VM_KERNEL_ADDRPERM(so), error);
		return NULL;
	}

	CFIL_INFO_VERIFY(cfil_info);
	return cfil_info;
}

errno_t
cfil_sock_udp_handle_data(bool outgoing, struct socket *so,
    struct sockaddr *local, struct sockaddr *remote,
    struct mbuf *data, struct mbuf *control, uint32_t flags,
    struct soflow_hash_entry *hash_entry)
{
#pragma unused(outgoing, so, local, remote, data, control, flags)
	errno_t error = 0;
	uint32_t filter_control_unit = 0;
	struct cfil_info *cfil_info = NULL;

	socket_lock_assert_owned(so);

	if (cfil_active_count == 0) {
		CFIL_LOG(LOG_DEBUG, "CFIL: UDP no active filter");
		OSIncrementAtomic(&cfil_stats.cfs_sock_attach_in_vain);
		return error;
	}

	// Socket has been blessed
	if ((so->so_flags1 & SOF1_CONTENT_FILTER_SKIP) != 0) {
		return error;
	}

	if (hash_entry == NULL) {
		CFIL_LOG(LOG_ERR, "CFIL: <so %llx> NULL soflow_hash_entry", (uint64_t)VM_KERNEL_ADDRPERM(so));
		return EPIPE;
	}

	if (hash_entry->soflow_db == NULL) {
		CFIL_LOG(LOG_ERR, "CFIL: <so %llx> NULL soflow_hash_entry db", (uint64_t)VM_KERNEL_ADDRPERM(so));
		return EPIPE;
	}

	filter_control_unit = hash_entry->soflow_filter_control_unit;
	if (filter_control_unit == 0) {
		CFIL_LOG(LOG_DEBUG, "CFIL: UDP failed to get control unit");
		return error;
	}

	if (filter_control_unit == NECP_FILTER_UNIT_NO_FILTER) {
		return error;
	}

	if ((filter_control_unit & NECP_MASK_USERSPACE_ONLY) != 0) {
		CFIL_LOG(LOG_DEBUG, "CFIL: UDP user space only");
		OSIncrementAtomic(&cfil_stats.cfs_sock_userspace_only);
		return error;
	}

	cfil_info = cfil_sock_udp_get_info(so, filter_control_unit, outgoing, hash_entry, local, remote);
	if (cfil_info == NULL) {
		return EPIPE;
	}
	// Update last used timestamp, this is for flow Idle TO

	if (cfil_info->cfi_debug) {
		cfil_info_log(LOG_ERR, cfil_info, "CFIL: Got flow");
	}

	if (cfil_info->cfi_flags & CFIF_DROP) {
		if (cfil_info->cfi_debug) {
			cfil_info_log(LOG_ERR, cfil_info, "CFIL: UDP DROP");
		}
		return EPIPE;
	}
	if (control != NULL) {
		OSIncrementAtomic(&cfil_stats.cfs_data_in_control);
	}
	if (data->m_type == MT_OOBDATA) {
		CFIL_LOG(LOG_ERR, "so %llx MSG_OOB",
		    (uint64_t)VM_KERNEL_ADDRPERM(so));
		OSIncrementAtomic(&cfil_stats.cfs_data_in_oob);
	}

	error = cfil_data_common(so, cfil_info, outgoing, remote, data, control, flags);

	return error;
}

struct cfil_udp_attached_context {
	bool need_wait;
	lck_mtx_t *mutex_held;
	int attached;
};

static bool
cfil_filters_udp_attached_per_flow(struct socket *so,
    struct soflow_hash_entry *hash_entry,
    void *context)
{
	struct cfil_udp_attached_context *apply_context = NULL;
	struct cfil_info * __single cfil_info = NULL;
	struct cfil_entry *entry = NULL;
	uint64_t sock_flow_id = 0;
	struct timespec ts;
	errno_t error = 0;
	int kcunit;

	if (hash_entry->soflow_feat_ctxt == NULL || context == NULL) {
		return true;
	}

	cfil_info = hash_entry->soflow_feat_ctxt;
	apply_context = (struct cfil_udp_attached_context *)context;

	for (kcunit = 1; kcunit <= MAX_CONTENT_FILTER; kcunit++) {
		entry = &cfil_info->cfi_entries[kcunit - 1];

		/* Are we attached to the filter? */
		if (entry->cfe_filter == NULL) {
			continue;
		}

		if ((entry->cfe_flags & CFEF_SENT_SOCK_ATTACHED) == 0) {
			continue;
		}
		if ((entry->cfe_flags & CFEF_CFIL_DETACHED) != 0) {
			continue;
		}

		if (apply_context->need_wait == TRUE) {
			if (cfil_info->cfi_debug) {
				cfil_info_log(LOG_ERR, cfil_info, "CFIL: UDP PER-FLOW WAIT FOR FLOW TO FINISH");
			}

			ts.tv_sec = cfil_close_wait_timeout / 1000;
			ts.tv_nsec = (cfil_close_wait_timeout % 1000) * NSEC_PER_USEC * 1000;

			OSIncrementAtomic(&cfil_stats.cfs_close_wait);
			cfil_info->cfi_flags |= CFIF_CLOSE_WAIT;
			sock_flow_id = cfil_info->cfi_sock_id;

			error = msleep((caddr_t)cfil_info, apply_context->mutex_held,
			    PSOCK | PCATCH, "cfil_filters_udp_attached_per_flow", &ts);

			// Woke up from sleep, validate if cfil_info is still valid
			if (so->so_flow_db == NULL ||
			    (cfil_info != soflow_db_get_feature_context(so->so_flow_db, sock_flow_id))) {
				// cfil_info is not valid, do not continue
				return false;
			}

			cfil_info->cfi_flags &= ~CFIF_CLOSE_WAIT;

			if (cfil_info->cfi_debug) {
				cfil_info_log(LOG_ERR, cfil_info, "CFIL: UDP PER-FLOW WAIT FOR FLOW DONE");
			}

			/*
			 * Force close in case of timeout
			 */
			if (error != 0) {
				OSIncrementAtomic(&cfil_stats.cfs_close_wait_timeout);

				if (cfil_info->cfi_debug) {
					cfil_info_log(LOG_ERR, cfil_info, "CFIL: UDP PER-FLOW WAIT FOR FLOW TIMED OUT, FORCE DETACH");
				}

				entry->cfe_flags |= CFEF_CFIL_DETACHED;
				return false;
			}
		}
		apply_context->attached = 1;
		return false;
	}
	return true;
}

/*
 * Go through all UDP flows for specified socket and returns TRUE if
 * any flow is still attached.  If need_wait is TRUE, wait on first
 * attached flow.
 */
static int
cfil_filters_udp_attached(struct socket *so, bool need_wait)
{
	struct cfil_udp_attached_context apply_context = { 0 };
	lck_mtx_t *mutex_held;

	socket_lock_assert_owned(so);

	if ((so->so_flags & SOF_CONTENT_FILTER) != 0 && so->so_flow_db != NULL) {
		if (so->so_proto->pr_getlock != NULL) {
			mutex_held = (*so->so_proto->pr_getlock)(so, PR_F_WILLUNLOCK);
		} else {
			mutex_held = so->so_proto->pr_domain->dom_mtx;
		}
		LCK_MTX_ASSERT(mutex_held, LCK_MTX_ASSERT_OWNED);

		apply_context.need_wait = need_wait;
		apply_context.mutex_held = mutex_held;
		soflow_db_apply(so->so_flow_db, cfil_filters_udp_attached_per_flow, (void *)&apply_context);
	}

	return apply_context.attached;
}

struct cfil_udp_data_pending_context {
	struct sockbuf *sb;
	uint64_t total_pending;
};

static bool
cfil_sock_udp_data_pending_per_flow(struct socket *so,
    struct soflow_hash_entry *hash_entry,
    void *context)
{
#pragma unused(so)
	struct cfil_udp_data_pending_context *apply_context = NULL;
	struct cfil_info * __single cfil_info = NULL;
	struct cfi_buf *cfi_buf;

	uint64_t pending = 0;

	if (hash_entry->soflow_feat_ctxt == NULL || context == NULL) {
		return true;
	}

	cfil_info = hash_entry->soflow_feat_ctxt;
	apply_context = (struct cfil_udp_data_pending_context *)context;

	if (apply_context->sb == NULL) {
		return true;
	}

	if ((apply_context->sb->sb_flags & SB_RECV) == 0) {
		cfi_buf = &cfil_info->cfi_snd;
	} else {
		cfi_buf = &cfil_info->cfi_rcv;
	}

	pending = cfi_buf->cfi_pending_last - cfi_buf->cfi_pending_first;
	/*
	 * If we are limited by the "chars of mbufs used" roughly
	 * adjust so we won't overcommit
	 */
	if ((uint64_t)cfi_buf->cfi_pending_mbcnt > pending) {
		pending = cfi_buf->cfi_pending_mbcnt;
	}

	apply_context->total_pending += pending;
	return true;
}

int32_t
cfil_sock_udp_data_pending(struct sockbuf *sb, bool check_thread)
{
	struct cfil_udp_data_pending_context apply_context = { 0 };
	struct socket *so = sb->sb_so;

	socket_lock_assert_owned(so);

	if ((so->so_flags & SOF_CONTENT_FILTER) != 0 && so->so_flow_db != NULL &&
	    (check_thread == FALSE || so->so_snd.sb_cfil_thread != current_thread())) {
		apply_context.sb = sb;
		soflow_db_apply(so->so_flow_db, cfil_sock_udp_data_pending_per_flow, (void *)&apply_context);

		VERIFY(apply_context.total_pending < INT32_MAX);
	}

	return (int32_t)(apply_context.total_pending);
}

struct cfil_udp_notify_shutdown_context {
	int how;
	int drop_flag;
	int shut_flag;
	int done_count;
};

static bool
cfil_sock_udp_notify_shutdown_per_flow(struct socket *so,
    struct soflow_hash_entry *hash_entry,
    void *context)
{
	struct cfil_udp_notify_shutdown_context *apply_context = NULL;
	struct cfil_info * __single cfil_info = NULL;
	errno_t error = 0;
	int kcunit;

	if (hash_entry->soflow_feat_ctxt == NULL || context == NULL) {
		return true;
	}

	cfil_info = hash_entry->soflow_feat_ctxt;
	apply_context = (struct cfil_udp_notify_shutdown_context *)context;

	// This flow is marked as DROP
	if (cfil_info->cfi_flags & apply_context->drop_flag) {
		apply_context->done_count++;
		return true;
	}

	// This flow has been shut already, skip
	if (cfil_info->cfi_flags & apply_context->shut_flag) {
		return true;
	}
	// Mark flow as shut
	cfil_info->cfi_flags |= apply_context->shut_flag;
	apply_context->done_count++;

	for (kcunit = 1; kcunit <= MAX_CONTENT_FILTER; kcunit++) {
		/* Disconnect incoming side */
		if (apply_context->how != SHUT_WR) {
			error = cfil_dispatch_disconnect_event(so, cfil_info, kcunit, 0);
		}
		/* Disconnect outgoing side */
		if (apply_context->how != SHUT_RD) {
			error = cfil_dispatch_disconnect_event(so, cfil_info, kcunit, 1);
		}
	}

	if (cfil_info->cfi_debug) {
		cfil_info_log(LOG_ERR, cfil_info, "CFIL: UDP PER-FLOW NOTIFY_SHUTDOWN");
	}

	return true;
}

int
cfil_sock_udp_notify_shutdown(struct socket *so, int how, int drop_flag, int shut_flag)
{
	struct cfil_udp_notify_shutdown_context apply_context = { 0 };
	errno_t error = 0;

	socket_lock_assert_owned(so);

	if ((so->so_flags & SOF_CONTENT_FILTER) != 0 && so->so_flow_db != NULL) {
		apply_context.how = how;
		apply_context.drop_flag = drop_flag;
		apply_context.shut_flag = shut_flag;

		soflow_db_apply(so->so_flow_db, cfil_sock_udp_notify_shutdown_per_flow, (void *)&apply_context);
	}

	if (apply_context.done_count == 0) {
		error = ENOTCONN;
	}
	return error;
}

int
cfil_sock_udp_shutdown(struct socket *so, int *how)
{
	int error = 0;

	if ((so->so_flags & SOF_CONTENT_FILTER) == 0 || (so->so_flow_db == NULL)) {
		goto done;
	}

	socket_lock_assert_owned(so);

	CFIL_LOG(LOG_INFO, "so %llx how %d",
	    (uint64_t)VM_KERNEL_ADDRPERM(so), *how);

	/*
	 * Check the state of the socket before the content filter
	 */
	if (*how != SHUT_WR && (so->so_state & SS_CANTRCVMORE) != 0) {
		/* read already shut down */
		error = ENOTCONN;
		goto done;
	}
	if (*how != SHUT_RD && (so->so_state & SS_CANTSENDMORE) != 0) {
		/* write already shut down */
		error = ENOTCONN;
		goto done;
	}

	/*
	 * shutdown read: SHUT_RD or SHUT_RDWR
	 */
	if (*how != SHUT_WR) {
		error = cfil_sock_udp_notify_shutdown(so, SHUT_RD, CFIF_DROP, CFIF_SHUT_RD);
		if (error != 0) {
			goto done;
		}
	}
	/*
	 * shutdown write: SHUT_WR or SHUT_RDWR
	 */
	if (*how != SHUT_RD) {
		error = cfil_sock_udp_notify_shutdown(so, SHUT_WR, CFIF_DROP, CFIF_SHUT_WR);
		if (error != 0) {
			goto done;
		}

		/*
		 * When outgoing data is pending, we delay the shutdown at the
		 * protocol level until the content filters give the final
		 * verdict on the pending data.
		 */
		if (cfil_sock_data_pending(&so->so_snd) != 0) {
			/*
			 * When shutting down the read and write sides at once
			 * we can proceed to the final shutdown of the read
			 * side. Otherwise, we just return.
			 */
			if (*how == SHUT_WR) {
				error = EJUSTRETURN;
			} else if (*how == SHUT_RDWR) {
				*how = SHUT_RD;
			}
		}
	}
done:
	return error;
}

void
cfil_sock_udp_close_wait(struct socket *so)
{
	socket_lock_assert_owned(so);

	while (cfil_filters_udp_attached(so, FALSE)) {
		/*
		 * Notify the filters we are going away so they can detach
		 */
		cfil_sock_udp_notify_shutdown(so, SHUT_RDWR, 0, 0);

		/*
		 * Make sure we need to wait after the filter are notified
		 * of the disconnection
		 */
		if (cfil_filters_udp_attached(so, TRUE) == 0) {
			break;
		}
	}
}

static bool
cfil_sock_udp_is_closed_per_flow(struct socket *so,
    struct soflow_hash_entry *hash_entry,
    void *context)
{
#pragma unused(context)
	struct cfil_info * __single cfil_info = NULL;
	errno_t error = 0;
	int kcunit;

	if (hash_entry->soflow_feat_ctxt == NULL) {
		return true;
	}

	cfil_info = hash_entry->soflow_feat_ctxt;

	for (kcunit = 1; kcunit <= MAX_CONTENT_FILTER; kcunit++) {
		/* Let the filters know of the closing */
		error = cfil_dispatch_closed_event(so, cfil_info, kcunit);
	}

	/* Last chance to push passed data out */
	error = cfil_acquire_sockbuf(so, cfil_info, 1);
	if (error == 0) {
		cfil_service_inject_queue(so, cfil_info, 1);
	}
	cfil_release_sockbuf(so, 1);

	cfil_info->cfi_flags |= CFIF_SOCK_CLOSED;

	/* Pending data needs to go */
	cfil_flush_queues(so, cfil_info);

	CFIL_INFO_VERIFY(cfil_info);

	if (cfil_info->cfi_debug) {
		cfil_info_log(LOG_ERR, cfil_info, "CFIL: UDP PER-FLOW IS_CLOSED");
	}

	return true;
}

void
cfil_sock_udp_is_closed(struct socket *so)
{
	socket_lock_assert_owned(so);

	if ((so->so_flags & SOF_CONTENT_FILTER) != 0 && so->so_flow_db != NULL) {
		soflow_db_apply(so->so_flow_db, cfil_sock_udp_is_closed_per_flow, NULL);
	}
}

static bool
cfil_sock_udp_buf_update_per_flow(struct socket *so,
    struct soflow_hash_entry *hash_entry,
    void *context)
{
	struct cfil_info * __single cfil_info = NULL;
	struct sockbuf *sb = NULL;
	errno_t error = 0;
	int outgoing;

	if (hash_entry->soflow_feat_ctxt == NULL || context == NULL) {
		return true;
	}

	cfil_info = hash_entry->soflow_feat_ctxt;
	sb = (struct sockbuf *) context;

	if ((sb->sb_flags & SB_RECV) == 0) {
		if ((cfil_info->cfi_flags & CFIF_RETRY_INJECT_OUT) == 0) {
			return true;
		}
		outgoing = 1;
		OSIncrementAtomic(&cfil_stats.cfs_inject_q_out_retry);
	} else {
		if ((cfil_info->cfi_flags & CFIF_RETRY_INJECT_IN) == 0) {
			return true;
		}
		outgoing = 0;
		OSIncrementAtomic(&cfil_stats.cfs_inject_q_in_retry);
	}

	CFIL_LOG(LOG_NOTICE, "so %llx outgoing %d",
	    (uint64_t)VM_KERNEL_ADDRPERM(so), outgoing);

	error = cfil_acquire_sockbuf(so, cfil_info, outgoing);
	if (error == 0) {
		cfil_service_inject_queue(so, cfil_info, outgoing);
	}
	cfil_release_sockbuf(so, outgoing);
	return true;
}

void
cfil_sock_udp_buf_update(struct sockbuf *sb)
{
	struct socket *so = sb->sb_so;

	socket_lock_assert_owned(so);

	if ((so->so_flags & SOF_CONTENT_FILTER) != 0 && so->so_flow_db != NULL) {
		if (!cfil_sbtrim) {
			return;
		}
		soflow_db_apply(so->so_flow_db, cfil_sock_udp_buf_update_per_flow, (void *)sb);
	}
}

void
cfil_filter_show(u_int32_t kcunit)
{
	struct content_filter *cfc = NULL;
	struct cfil_entry *entry;
	int count = 0;

	if (kcunit > MAX_CONTENT_FILTER) {
		return;
	}

	cfil_rw_lock_shared(&cfil_lck_rw);

	if (content_filters[kcunit - 1] == NULL) {
		cfil_rw_unlock_shared(&cfil_lck_rw);
		return;
	}
	cfc = content_filters[kcunit - 1];

	CFIL_LOG(LOG_DEBUG, "CFIL: FILTER SHOW: Filter <unit %d, entry count %d> flags <%lx>:",
	    kcunit, cfc->cf_sock_count, (unsigned long)cfc->cf_flags);
	if (cfc->cf_flags & CFF_DETACHING) {
		CFIL_LOG(LOG_DEBUG, "CFIL: FILTER SHOW:-DETACHING");
	}
	if (cfc->cf_flags & CFF_ACTIVE) {
		CFIL_LOG(LOG_DEBUG, "CFIL: FILTER SHOW:-ACTIVE");
	}
	if (cfc->cf_flags & CFF_FLOW_CONTROLLED) {
		CFIL_LOG(LOG_DEBUG, "CFIL: FILTER SHOW:-FLOW CONTROLLED");
	}

	TAILQ_FOREACH(entry, &cfc->cf_sock_entries, cfe_link) {
		if (entry->cfe_cfil_info && entry->cfe_cfil_info->cfi_so) {
			struct cfil_info *cfil_info = entry->cfe_cfil_info;

			count++;

			if (entry->cfe_flags & CFEF_CFIL_DETACHED) {
				cfil_info_log(LOG_DEBUG, cfil_info, "CFIL: FILTER SHOW:-DETACHED");
			} else {
				cfil_info_log(LOG_DEBUG, cfil_info, "CFIL: FILTER SHOW:-ATTACHED");
			}
		}
	}

	CFIL_LOG(LOG_DEBUG, "CFIL: FILTER SHOW:Filter - total entries shown: %d", count);

	cfil_rw_unlock_shared(&cfil_lck_rw);
}

void
cfil_info_show(void)
{
	struct cfil_info *cfil_info;
	int count = 0;

	cfil_rw_lock_shared(&cfil_lck_rw);

	CFIL_LOG(LOG_DEBUG, "CFIL: INFO SHOW:count %d", cfil_sock_attached_count);

	TAILQ_FOREACH(cfil_info, &cfil_sock_head, cfi_link) {
		count++;

		cfil_info_log(LOG_DEBUG, cfil_info, "CFIL: INFO SHOW");

		if (cfil_info->cfi_flags & CFIF_DROP) {
			CFIL_LOG(LOG_DEBUG, "CFIL: INFO FLAG - DROP");
		}
		if (cfil_info->cfi_flags & CFIF_CLOSE_WAIT) {
			CFIL_LOG(LOG_DEBUG, "CFIL: INFO FLAG - CLOSE_WAIT");
		}
		if (cfil_info->cfi_flags & CFIF_SOCK_CLOSED) {
			CFIL_LOG(LOG_DEBUG, "CFIL: INFO FLAG - SOCK_CLOSED");
		}
		if (cfil_info->cfi_flags & CFIF_RETRY_INJECT_IN) {
			CFIL_LOG(LOG_DEBUG, "CFIL: INFO FLAG - RETRY_INJECT_IN");
		}
		if (cfil_info->cfi_flags & CFIF_RETRY_INJECT_OUT) {
			CFIL_LOG(LOG_DEBUG, "CFIL: INFO FLAG - RETRY_INJECT_OUT");
		}
		if (cfil_info->cfi_flags & CFIF_SHUT_WR) {
			CFIL_LOG(LOG_DEBUG, "CFIL: INFO FLAG - SHUT_WR");
		}
		if (cfil_info->cfi_flags & CFIF_SHUT_RD) {
			CFIL_LOG(LOG_DEBUG, "CFIL: INFO FLAG - SHUT_RD");
		}
	}

	CFIL_LOG(LOG_DEBUG, "CFIL: INFO SHOW:total cfil_info shown: %d", count);

	cfil_rw_unlock_shared(&cfil_lck_rw);
}

bool
cfil_info_action_timed_out(struct cfil_info *cfil_info, int timeout)
{
	struct cfil_entry *entry;
	struct timeval current_tv;
	struct timeval diff_time;

	if (cfil_info == NULL) {
		return false;
	}

	/*
	 * If we have queued up more data than passed offset and we haven't received
	 * an action from user space for a while (the user space filter might have crashed),
	 * return action timed out.
	 */
	if (cfil_info->cfi_snd.cfi_pending_last > cfil_info->cfi_snd.cfi_pass_offset ||
	    cfil_info->cfi_rcv.cfi_pending_last > cfil_info->cfi_rcv.cfi_pass_offset) {
		microuptime(&current_tv);

		for (int kcunit = 1; kcunit <= MAX_CONTENT_FILTER; kcunit++) {
			entry = &cfil_info->cfi_entries[kcunit - 1];

			if (entry->cfe_filter == NULL) {
				continue;
			}

			if (cfil_info->cfi_snd.cfi_pending_last > entry->cfe_snd.cfe_pass_offset ||
			    cfil_info->cfi_rcv.cfi_pending_last > entry->cfe_rcv.cfe_pass_offset) {
				// haven't gotten an action from this filter, check timeout
				timersub(&current_tv, &entry->cfe_last_action, &diff_time);
				if (diff_time.tv_sec >= timeout) {
					if (cfil_info->cfi_debug) {
						cfil_info_log(LOG_ERR, cfil_info, "CFIL: flow ACTION timeout expired");
					}
					return true;
				}
			}
		}
	}
	return false;
}

bool
cfil_info_buffer_threshold_exceeded(struct cfil_info *cfil_info)
{
	if (cfil_info == NULL) {
		return false;
	}

	/*
	 * Clean up flow if it exceeded queue thresholds
	 */
	if (cfil_info->cfi_snd.cfi_tail_drop_cnt ||
	    cfil_info->cfi_rcv.cfi_tail_drop_cnt) {
		if (cfil_info->cfi_debug) {
			CFIL_LOG(LOG_ERR, "CFIL: queue threshold exceeded:mbuf max < count: %d bytes: %d > tail drop count < OUT: %d IN: %d > ",
			    cfil_udp_gc_mbuf_num_max,
			    cfil_udp_gc_mbuf_cnt_max,
			    cfil_info->cfi_snd.cfi_tail_drop_cnt,
			    cfil_info->cfi_rcv.cfi_tail_drop_cnt);
			cfil_info_log(LOG_ERR, cfil_info, "CFIL: queue threshold exceeded");
		}
		return true;
	}

	return false;
}

static bool
cfil_dgram_gc_needed(struct socket *so, struct soflow_hash_entry *hash_entry, u_int64_t current_time)
{
#pragma unused(current_time)
	struct cfil_info *cfil_info = NULL;

	if (so == NULL || hash_entry == NULL || hash_entry->soflow_feat_ctxt == NULL) {
		return false;
	}
	cfil_info = (struct cfil_info *) hash_entry->soflow_feat_ctxt;

	cfil_rw_lock_shared(&cfil_lck_rw);

	if (cfil_info_action_timed_out(cfil_info, UDP_FLOW_GC_ACTION_TO) ||
	    cfil_info_buffer_threshold_exceeded(cfil_info)) {
		if (cfil_info->cfi_debug) {
			cfil_info_log(LOG_ERR, cfil_info, "CFIL: UDP PER-FLOW GC NEEDED");
		}
		cfil_rw_unlock_shared(&cfil_lck_rw);
		return true;
	}

	cfil_rw_unlock_shared(&cfil_lck_rw);
	return false;
}

static bool
cfil_dgram_gc_perform(struct socket *so, struct soflow_hash_entry *hash_entry)
{
	struct cfil_info *cfil_info = NULL;

	if (so == NULL || hash_entry == NULL || hash_entry->soflow_feat_ctxt == NULL) {
		return false;
	}
	cfil_info = (struct cfil_info *) hash_entry->soflow_feat_ctxt;

	if (cfil_info->cfi_debug) {
		cfil_info_log(LOG_ERR, cfil_info, "CFIL: UDP PER-FLOW GC PERFORM");
	}

	for (int kcunit = 1; kcunit <= MAX_CONTENT_FILTER; kcunit++) {
		/* Let the filters know of the closing */
		cfil_dispatch_closed_event(so, cfil_info, kcunit);
	}
	cfil_sock_udp_unlink_flow(so, hash_entry, cfil_info);
	CFIL_INFO_FREE(cfil_info);
	OSIncrementAtomic(&cfil_stats.cfs_sock_detached);
	return true;
}

static bool
cfil_dgram_detach_entry(struct socket *so, struct soflow_hash_entry *hash_entry)
{
	struct cfil_info *cfil_info = NULL;

	if (hash_entry == NULL || hash_entry->soflow_feat_ctxt == NULL) {
		return true;
	}
	cfil_info = (struct cfil_info *) hash_entry->soflow_feat_ctxt;

	if (cfil_info->cfi_debug) {
		cfil_info_log(LOG_ERR, cfil_info, "CFIL: DGRAM DETACH ENTRY");
	}

	cfil_sock_udp_unlink_flow(so, hash_entry, cfil_info);
	CFIL_INFO_FREE(cfil_info);
	OSIncrementAtomic(&cfil_stats.cfs_sock_detached);

	return true;
}

static bool
cfil_dgram_detach_db(struct socket *so, struct soflow_db *db)
{
#pragma unused(db)
	if (so && so->so_flags & SOF_CONTENT_FILTER) {
		so->so_flags &= ~SOF_CONTENT_FILTER;
		CFIL_LOG(LOG_DEBUG, "CFIL: DGRAM DETACH DB <so %llx>", (uint64_t)VM_KERNEL_ADDRPERM(so));
	}
	return true;
}

struct m_tag *
cfil_dgram_save_socket_state(struct cfil_info *cfil_info, struct mbuf *m)
{
	struct m_tag *tag = NULL;
	struct cfil_tag *ctag = NULL;
	struct soflow_hash_entry *hash_entry = NULL;
	struct inpcb *inp = NULL;

	if (cfil_info == NULL || cfil_info->cfi_so == NULL ||
	    cfil_info->cfi_hash_entry == NULL || m == NULL || !(m->m_flags & M_PKTHDR)) {
		return NULL;
	}

	inp = sotoinpcb(cfil_info->cfi_so);

	/* Allocate a tag */
	tag = m_tag_create(KERNEL_MODULE_TAG_ID, KERNEL_TAG_TYPE_CFIL_UDP,
	    sizeof(struct cfil_tag), M_DONTWAIT, m);

	if (tag) {
		ctag = (struct cfil_tag *)(tag->m_tag_data);
		ctag->cfil_so_state_change_cnt = cfil_info->cfi_so->so_state_change_cnt;
		ctag->cfil_so_options = cfil_info->cfi_so->so_options;
		ctag->cfil_inp_flags = inp ? inp->inp_flags : 0;

		hash_entry = cfil_info->cfi_hash_entry;
		if (hash_entry->soflow_family == AF_INET6) {
			fill_ip6_sockaddr_4_6(&ctag->cfil_faddr,
			    &hash_entry->soflow_faddr.addr6,
			    hash_entry->soflow_fport, hash_entry->soflow_faddr6_ifscope);
		} else if (hash_entry->soflow_family == AF_INET) {
			fill_ip_sockaddr_4_6(&ctag->cfil_faddr,
			    hash_entry->soflow_faddr.addr46.ia46_addr4,
			    hash_entry->soflow_fport);
		}
		m_tag_prepend(m, tag);
		return tag;
	}
	return NULL;
}

struct m_tag *
cfil_dgram_get_socket_state(struct mbuf *m, uint32_t *state_change_cnt, uint32_t *options,
    struct sockaddr **faddr, int *inp_flags)
{
	struct m_tag *tag = NULL;
	struct cfil_tag *ctag = NULL;

	tag = m_tag_locate(m, KERNEL_MODULE_TAG_ID, KERNEL_TAG_TYPE_CFIL_UDP);
	if (tag) {
		ctag = (struct cfil_tag *)(tag->m_tag_data);
		if (state_change_cnt) {
			*state_change_cnt = ctag->cfil_so_state_change_cnt;
		}
		if (options) {
			*options = ctag->cfil_so_options;
		}
		if (faddr) {
			*faddr = SA(&ctag->cfil_faddr);
		}
		if (inp_flags) {
			*inp_flags = ctag->cfil_inp_flags;
		}

		/*
		 * Unlink tag and hand it over to caller.
		 * Note that caller will be responsible to free it.
		 */
		m_tag_unlink(m, tag);
		return tag;
	}
	return NULL;
}

boolean_t
cfil_dgram_peek_socket_state(struct mbuf *m, int *inp_flags)
{
	struct m_tag *tag = NULL;
	struct cfil_tag *ctag = NULL;

	tag = m_tag_locate(m, KERNEL_MODULE_TAG_ID, KERNEL_TAG_TYPE_CFIL_UDP);
	if (tag) {
		ctag = (struct cfil_tag *)(tag->m_tag_data);
		if (inp_flags) {
			*inp_flags = ctag->cfil_inp_flags;
		}
		return true;
	}
	return false;
}

static int
cfil_dispatch_stats_event_locked(int kcunit, struct cfil_stats_report_buffer *buffer, uint32_t stats_count)
{
	struct content_filter *cfc = NULL;
	errno_t error = 0;
	size_t msgsize = 0;

	if (buffer == NULL || stats_count == 0) {
		return error;
	}

	if (kcunit > MAX_CONTENT_FILTER) {
		return error;
	}

	cfc = content_filters[kcunit - 1];
	if (cfc == NULL) {
		return error;
	}

	/* Would be wasteful to try */
	if (cfc->cf_flags & CFF_FLOW_CONTROLLED) {
		error = ENOBUFS;
		goto done;
	}

	msgsize = sizeof(struct cfil_msg_stats_report) + (sizeof(struct cfil_msg_sock_stats) * stats_count);
	buffer->msghdr.cfm_len = (uint32_t)msgsize;
	buffer->msghdr.cfm_version = 1;
	buffer->msghdr.cfm_type = CFM_TYPE_EVENT;
	buffer->msghdr.cfm_op = CFM_OP_STATS;
	buffer->msghdr.cfm_sock_id = 0;
	buffer->count = stats_count;

	if (cfil_log_stats) {
		CFIL_LOG(LOG_DEBUG, "STATS (kcunit %d): msg size %lu - %lu %lu %lu",
		    kcunit,
		    (unsigned long)msgsize,
		    (unsigned long)sizeof(struct cfil_msg_stats_report),
		    (unsigned long)sizeof(struct cfil_msg_sock_stats),
		    (unsigned long)stats_count);
	}

	error = ctl_enqueuedata(cfc->cf_kcref, cfc->cf_kcunit,
	    buffer,
	    sizeof(struct cfil_stats_report_buffer),
	    CTL_DATA_EOR);
	if (error != 0) {
		CFIL_LOG(LOG_ERR, "ctl_enqueuedata() failed:%d", error);
		goto done;
	}
	OSIncrementAtomic(&cfil_stats.cfs_stats_event_ok);

	if (cfil_log_stats) {
		CFIL_LOG(LOG_DEBUG, "CFIL: STATS REPORT:send msg to %d", kcunit);
	}
done:

	if (error == ENOBUFS) {
		OSIncrementAtomic(
			&cfil_stats.cfs_stats_event_flow_control);

		if (!cfil_rw_lock_shared_to_exclusive(&cfil_lck_rw)) {
			cfil_rw_lock_exclusive(&cfil_lck_rw);
		}

		cfc->cf_flags |= CFF_FLOW_CONTROLLED;

		cfil_rw_lock_exclusive_to_shared(&cfil_lck_rw);
	} else if (error != 0) {
		OSIncrementAtomic(&cfil_stats.cfs_stats_event_fail);
	}

	return error;
}

static void
cfil_stats_report_thread_sleep(bool forever)
{
	if (cfil_log_stats) {
		CFIL_LOG(LOG_DEBUG, "CFIL: STATS COLLECTION SLEEP");
	}

	if (forever) {
		(void) assert_wait((event_t) &cfil_sock_attached_stats_count,
		    THREAD_INTERRUPTIBLE);
	} else {
		uint64_t deadline = 0;
		nanoseconds_to_absolutetime(CFIL_STATS_REPORT_RUN_INTERVAL_NSEC, &deadline);
		clock_absolutetime_interval_to_deadline(deadline, &deadline);

		(void) assert_wait_deadline(&cfil_sock_attached_stats_count,
		    THREAD_INTERRUPTIBLE, deadline);
	}
}

static void
cfil_stats_report_thread_func(void *v, wait_result_t w)
{
#pragma unused(v, w)

	ASSERT(cfil_stats_report_thread == current_thread());
	thread_set_thread_name(current_thread(), "CFIL_STATS_REPORT");

	// Kick off gc shortly
	cfil_stats_report_thread_sleep(false);
	thread_block_parameter((thread_continue_t) cfil_stats_report, NULL);
	/* NOTREACHED */
}

static bool
cfil_stats_collect_flow_stats_for_filter(int kcunit,
    struct cfil_info *cfil_info,
    struct cfil_entry *entry,
    struct timeval current_tv)
{
	struct cfil_stats_report_buffer *buffer = NULL;
	struct cfil_msg_sock_stats *flow_array = NULL;
	struct cfil_msg_sock_stats *stats = NULL;
	struct inpcb *inp = NULL;
	struct timeval diff_time;
	uint64_t diff_time_usecs;
	int index = 0;

	if (entry->cfe_stats_report_frequency == 0) {
		return false;
	}

	buffer = global_cfil_stats_report_buffers[kcunit - 1];
	if (buffer == NULL) {
		CFIL_LOG(LOG_ERR, "CFIL: STATS: no buffer");
		return false;
	}

	timersub(&current_tv, &entry->cfe_stats_report_ts, &diff_time);
	diff_time_usecs = (diff_time.tv_sec * USEC_PER_SEC) + diff_time.tv_usec;

	if (cfil_info->cfi_debug && cfil_log_stats) {
		CFIL_LOG(LOG_ERR, "CFIL: STATS REPORT - elapsed time - ts %llu %llu cur ts %llu %llu diff %llu %llu(usecs %llu) @freq %llu usecs sockID %llu <%llx>",
		    (unsigned long long)entry->cfe_stats_report_ts.tv_sec,
		    (unsigned long long)entry->cfe_stats_report_ts.tv_usec,
		    (unsigned long long)current_tv.tv_sec,
		    (unsigned long long)current_tv.tv_usec,
		    (unsigned long long)diff_time.tv_sec,
		    (unsigned long long)diff_time.tv_usec,
		    (unsigned long long)diff_time_usecs,
		    (unsigned long long)((entry->cfe_stats_report_frequency * NSEC_PER_MSEC) / NSEC_PER_USEC),
		    cfil_info->cfi_sock_id, cfil_info->cfi_sock_id);
	}

	// Compare elapsed time in usecs
	if (diff_time_usecs >= (entry->cfe_stats_report_frequency * NSEC_PER_MSEC) / NSEC_PER_USEC) {
		if (cfil_info->cfi_debug && cfil_log_stats) {
			CFIL_LOG(LOG_ERR, "CFIL: STATS REPORT - in %llu reported %llu",
			    cfil_info->cfi_byte_inbound_count,
			    entry->cfe_byte_inbound_count_reported);
			CFIL_LOG(LOG_ERR, "CFIL: STATS REPORT - out %llu reported %llu",
			    cfil_info->cfi_byte_outbound_count,
			    entry->cfe_byte_outbound_count_reported);
		}
		// Check if flow has new bytes that have not been reported
		if (entry->cfe_byte_inbound_count_reported < cfil_info->cfi_byte_inbound_count ||
		    entry->cfe_byte_outbound_count_reported < cfil_info->cfi_byte_outbound_count) {
			flow_array = (struct cfil_msg_sock_stats *)&buffer->stats;
			index = global_cfil_stats_counts[kcunit - 1];

			stats = &flow_array[index];
			stats->cfs_sock_id = cfil_info->cfi_sock_id;
			stats->cfs_byte_inbound_count = cfil_info->cfi_byte_inbound_count;
			stats->cfs_byte_outbound_count = cfil_info->cfi_byte_outbound_count;

			if (entry->cfe_laddr_sent == false) {
				/* cache it if necessary */
				if (cfil_info->cfi_so_attach_laddr.sa.sa_len == 0) {
					inp = cfil_info->cfi_so ? sotoinpcb(cfil_info->cfi_so) : NULL;
					if (inp != NULL) {
						boolean_t outgoing = (cfil_info->cfi_dir == CFS_CONNECTION_DIR_OUT);
						union sockaddr_in_4_6 *src = outgoing ? &cfil_info->cfi_so_attach_laddr : NULL;
						union sockaddr_in_4_6 *dst = outgoing ? NULL : &cfil_info->cfi_so_attach_laddr;
						cfil_fill_event_msg_addresses(cfil_info->cfi_hash_entry, inp,
						    src, dst, !IS_INP_V6(inp), outgoing);
					}
				}

				if (cfil_info->cfi_so_attach_laddr.sa.sa_len != 0) {
					stats->cfs_laddr.sin6 = cfil_info->cfi_so_attach_laddr.sin6;
					entry->cfe_laddr_sent = true;
				}
			}

			global_cfil_stats_counts[kcunit - 1]++;

			entry->cfe_stats_report_ts = current_tv;
			entry->cfe_byte_inbound_count_reported = cfil_info->cfi_byte_inbound_count;
			entry->cfe_byte_outbound_count_reported = cfil_info->cfi_byte_outbound_count;
			if (cfil_info->cfi_debug && cfil_log_stats) {
				cfil_info_log(LOG_ERR, cfil_info, "CFIL: STATS COLLECTED");
			}
			CFI_ADD_TIME_LOG(cfil_info, &current_tv, &cfil_info->cfi_first_event, CFM_OP_STATS);
			return true;
		}
	}
	return false;
}

static void
cfil_stats_report(void *v, wait_result_t w)
{
#pragma unused(v, w)

	struct cfil_info *cfil_info = NULL;
	struct cfil_entry *entry = NULL;
	struct timeval current_tv;
	uint32_t flow_count = 0;
	uint64_t saved_next_sock_id = 0; // Next sock id to be reported for next loop
	bool flow_reported = false;

	if (cfil_log_stats) {
		CFIL_LOG(LOG_DEBUG, "CFIL: STATS COLLECTION RUNNING");
	}

	do {
		// Collect all sock ids of flows that has new stats
		cfil_rw_lock_shared(&cfil_lck_rw);

		if (cfil_sock_attached_stats_count == 0) {
			if (cfil_log_stats) {
				CFIL_LOG(LOG_DEBUG, "CFIL: STATS: no flow");
			}
			cfil_rw_unlock_shared(&cfil_lck_rw);
			goto go_sleep;
		}

		for (int kcunit = 1; kcunit <= MAX_CONTENT_FILTER; kcunit++) {
			if (global_cfil_stats_report_buffers[kcunit - 1] != NULL) {
				memset(global_cfil_stats_report_buffers[kcunit - 1], 0, sizeof(struct cfil_stats_report_buffer));
			}
			global_cfil_stats_counts[kcunit - 1] = 0;
		}

		microuptime(&current_tv);
		flow_count = 0;

		TAILQ_FOREACH(cfil_info, &cfil_sock_head_stats, cfi_link_stats) {
			if (saved_next_sock_id != 0 &&
			    saved_next_sock_id == cfil_info->cfi_sock_id) {
				// Here is where we left off previously, start accumulating
				saved_next_sock_id = 0;
			}

			if (saved_next_sock_id == 0) {
				if (flow_count >= CFIL_STATS_REPORT_MAX_COUNT) {
					// Examine a fixed number of flows each round.  Remember the current flow
					// so we can start from here for next loop
					saved_next_sock_id = cfil_info->cfi_sock_id;
					break;
				}

				flow_reported = false;
				for (int kcunit = 1; kcunit <= MAX_CONTENT_FILTER; kcunit++) {
					entry = &cfil_info->cfi_entries[kcunit - 1];
					if (entry->cfe_filter == NULL) {
						if (cfil_info->cfi_debug && cfil_log_stats) {
							CFIL_LOG(LOG_ERR, "CFIL: STATS REPORT - so %llx no filter",
							    cfil_info->cfi_so ? (uint64_t)VM_KERNEL_ADDRPERM(cfil_info->cfi_so) : 0);
						}
						continue;
					}

					if ((entry->cfe_stats_report_frequency > 0) &&
					    cfil_stats_collect_flow_stats_for_filter(kcunit, cfil_info, entry, current_tv) == true) {
						flow_reported = true;
					}
				}
				if (flow_reported == true) {
					flow_count++;
				}
			}
		}

		if (flow_count > 0) {
			if (cfil_log_stats) {
				CFIL_LOG(LOG_DEBUG, "CFIL: STATS reporting for %d flows", flow_count);
			}
			for (int kcunit = 1; kcunit <= MAX_CONTENT_FILTER; kcunit++) {
				if (global_cfil_stats_report_buffers[kcunit - 1] != NULL &&
				    global_cfil_stats_counts[kcunit - 1] > 0) {
					cfil_dispatch_stats_event_locked(kcunit,
					    global_cfil_stats_report_buffers[kcunit - 1],
					    global_cfil_stats_counts[kcunit - 1]);
				}
			}
		} else {
			cfil_rw_unlock_shared(&cfil_lck_rw);
			goto go_sleep;
		}

		cfil_rw_unlock_shared(&cfil_lck_rw);

		// Loop again if we haven't finished the whole cfil_info list
	} while (saved_next_sock_id != 0);

go_sleep:

	// Sleep forever (until waken up) if no more flow to report
	cfil_rw_lock_shared(&cfil_lck_rw);
	cfil_stats_report_thread_sleep(cfil_sock_attached_stats_count == 0 ? true : false);
	cfil_rw_unlock_shared(&cfil_lck_rw);
	thread_block_parameter((thread_continue_t) cfil_stats_report, NULL);
	/* NOTREACHED */
}
