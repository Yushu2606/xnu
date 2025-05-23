/*
 * Copyright (c) 2012-2017 Apple Inc. All rights reserved.
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
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/sysctl.h>
#include <netinet/in_systm.h>
#include <sys/socket.h>
#include <sys/socketvar.h>
#include <sys/syslog.h>
#include <net/route.h>
#include <netinet/in.h>
#include <net/if.h>

#include <netinet/ip.h>
#include <netinet/ip_var.h>
#include <netinet/in_var.h>
#include <netinet/tcp.h>
#include <netinet/tcp_cache.h>
#include <netinet/tcp_seq.h>
#include <netinet/tcpip.h>
#include <netinet/tcp_fsm.h>
#include <netinet/mptcp_var.h>
#include <netinet/mptcp.h>
#include <netinet/mptcp_opt.h>
#include <netinet/mptcp_seq.h>

#include <libkern/crypto/sha1.h>
#include <libkern/crypto/sha2.h>
#include <netinet/mptcp_timer.h>

#include <mach/sdt.h>

static int mptcp_validate_join_hmac(struct tcpcb *, u_char* __sized_by(maclen), int maclen);
static int mptcp_snd_mpprio(struct tcpcb *tp, u_char *cp __ended_by(optend), u_char *optend, int optlen);
static void mptcp_send_remaddr_opt(struct tcpcb *, struct mptcp_remaddr_opt *);
static int mptcp_echo_add_addr(struct tcpcb *, u_char * __ended_by(optend), u_char *optend, unsigned int);

/*
 * MPTCP Options Output Processing
 */

static unsigned
mptcp_setup_first_subflow_syn_opts(struct socket *so, u_char *opt __ended_by(optend), u_char *optend __unused, unsigned optlen)
{
	struct mptcp_mpcapable_opt_rsp mptcp_opt;
	struct tcpcb *tp = sototcpcb(so);
	struct mptcb *mp_tp = tptomptp(tp);
	struct mptses *mpte = mp_tp->mpt_mpte;
	int ret;

	uint8_t mmco_len = mp_tp->mpt_version == MPTCP_VERSION_0 ?
	    sizeof(struct mptcp_mpcapable_opt_rsp) :
	    sizeof(struct mptcp_mpcapable_opt_common);

	ret = tcp_heuristic_do_mptcp(tp);
	if (ret > 0) {
		os_log(mptcp_log_handle, "%s - %lx: Not doing MPTCP due to heuristics",
		    __func__, (unsigned long)VM_KERNEL_ADDRPERM(mp_tp->mpt_mpte));
		mp_tp->mpt_flags |= MPTCPF_FALLBACK_HEURISTIC;
		return optlen;
	}

	/*
	 * Avoid retransmitting the MP_CAPABLE option.
	 */
	if (ret == 0 &&
	    tp->t_rxtshift > mptcp_mpcap_retries &&
	    !(mpte->mpte_flags & MPTE_FORCE_ENABLE)) {
		if (!(mp_tp->mpt_flags & (MPTCPF_FALLBACK_HEURISTIC | MPTCPF_HEURISTIC_TRAC))) {
			mp_tp->mpt_flags |= MPTCPF_HEURISTIC_TRAC;
			tcp_heuristic_mptcp_loss(tp);
		}
		return optlen;
	}

	bzero(&mptcp_opt, sizeof(struct mptcp_mpcapable_opt_rsp));

	mptcp_opt.mmc_common.mmco_kind = TCPOPT_MULTIPATH;
	mptcp_opt.mmc_common.mmco_len = mmco_len;
	mptcp_opt.mmc_common.mmco_subtype = MPO_CAPABLE;
	mptcp_opt.mmc_common.mmco_version = mp_tp->mpt_version;
	mptcp_opt.mmc_common.mmco_flags |= MPCAP_PROPOSAL_SBIT;
	if (mp_tp->mpt_flags & MPTCPF_CHECKSUM) {
		mptcp_opt.mmc_common.mmco_flags |= MPCAP_CHECKSUM_CBIT;
	}
	mptcp_opt.mmc_localkey = mp_tp->mpt_localkey;

	memcpy(opt + optlen, &mptcp_opt, mmco_len);
	optlen += mmco_len;

	return optlen;
}

static unsigned
mptcp_setup_join_subflow_syn_opts(struct socket *so, u_char *opt __ended_by(optend), u_char *optend __unused, unsigned optlen)
{
	struct mptcp_mpjoin_opt_req mpjoin_req;
	struct inpcb *inp = sotoinpcb(so);
	struct tcpcb *tp = NULL;
	struct mptsub *mpts;

	if (!inp) {
		return optlen;
	}

	tp = intotcpcb(inp);
	if (!tp) {
		return optlen;
	}

	mpts = tp->t_mpsub;

	bzero(&mpjoin_req, sizeof(mpjoin_req));
	mpjoin_req.mmjo_kind = TCPOPT_MULTIPATH;
	mpjoin_req.mmjo_len = sizeof(mpjoin_req);
	mpjoin_req.mmjo_subtype_bkp = MPO_JOIN << 4;

	if (tp->t_mpflags & TMPF_BACKUP_PATH) {
		mpjoin_req.mmjo_subtype_bkp |= MPTCP_BACKUP;
	} else if (inp->inp_boundifp && IFNET_IS_CELLULAR(inp->inp_boundifp) &&
	    mptcp_subflows_need_backup_flag(mpts->mpts_mpte)) {
		mpjoin_req.mmjo_subtype_bkp |= MPTCP_BACKUP;
		tp->t_mpflags |= TMPF_BACKUP_PATH;
	} else {
		mpts->mpts_flags |= MPTSF_PREFERRED;
	}

	mpjoin_req.mmjo_addr_id = tp->t_local_aid;
	mpjoin_req.mmjo_peer_token = tptomptp(tp)->mpt_remotetoken;
	mptcp_get_rands(tp->t_local_aid, tptomptp(tp),
	    &mpjoin_req.mmjo_rand, NULL);
	memcpy(opt + optlen, &mpjoin_req, mpjoin_req.mmjo_len);
	optlen += mpjoin_req.mmjo_len;

	return optlen;
}

unsigned
mptcp_setup_join_ack_opts(struct tcpcb *tp, u_char *opt __ended_by(optend), u_char *optend __unused, unsigned optlen)
{
	unsigned new_optlen;
	struct mptcp_mpjoin_opt_rsp2 join_rsp2;

	if ((MAX_TCPOPTLEN - optlen) < sizeof(struct mptcp_mpjoin_opt_rsp2)) {
		printf("%s: no space left %d \n", __func__, optlen);
		return optlen;
	}

	bzero(&join_rsp2, sizeof(struct mptcp_mpjoin_opt_rsp2));
	join_rsp2.mmjo_kind = TCPOPT_MULTIPATH;
	join_rsp2.mmjo_len = sizeof(struct mptcp_mpjoin_opt_rsp2);
	join_rsp2.mmjo_subtype = MPO_JOIN;
	mptcp_get_mpjoin_hmac(tp->t_local_aid, tptomptp(tp),
	    (u_char*)&join_rsp2.mmjo_mac, HMAC_TRUNCATED_ACK);
	memcpy(opt + optlen, &join_rsp2, join_rsp2.mmjo_len);
	new_optlen = optlen + join_rsp2.mmjo_len;
	return new_optlen;
}

unsigned
mptcp_setup_syn_opts(struct socket *so, u_char *opt __ended_by(optend), u_char *optend, unsigned optlen)
{
	unsigned new_optlen;

	if (!(so->so_flags & SOF_MP_SEC_SUBFLOW)) {
		new_optlen = mptcp_setup_first_subflow_syn_opts(so, opt, optend, optlen);
	} else {
		new_optlen = mptcp_setup_join_subflow_syn_opts(so, opt, optend, optlen);
	}

	return new_optlen;
}

static int
mptcp_send_mpfail(struct tcpcb *tp, u_char *opt __ended_by(optend), u_char *optend, unsigned int optlen)
{
#pragma unused(tp, opt, optend, optlen)

	struct mptcb *mp_tp = NULL;
	struct mptcp_mpfail_opt fail_opt;
	uint64_t dsn;
	uint8_t len = sizeof(struct mptcp_mpfail_opt);

	mp_tp = tptomptp(tp);
	if (mp_tp == NULL) {
		tp->t_mpflags &= ~TMPF_SND_MPFAIL;
		return optlen;
	}

	/* if option space low give up */
	if ((MAX_TCPOPTLEN - optlen) < sizeof(struct mptcp_mpfail_opt)) {
		tp->t_mpflags &= ~TMPF_SND_MPFAIL;
		return optlen;
	}

	dsn = mp_tp->mpt_rcvnxt;

	bzero(&fail_opt, sizeof(fail_opt));
	fail_opt.mfail_kind = TCPOPT_MULTIPATH;
	fail_opt.mfail_len = len;
	fail_opt.mfail_subtype = MPO_FAIL;
	fail_opt.mfail_dsn = mptcp_hton64(dsn);
	memcpy(opt + optlen, &fail_opt, len);
	optlen += len;
	tp->t_mpflags &= ~TMPF_SND_MPFAIL;
	return optlen;
}

static int
mptcp_send_infinite_mapping(struct tcpcb *tp, u_char *opt __ended_by(optend), u_char *optend __unused, unsigned int optlen)
{
	struct socket *so = tp->t_inpcb->inp_socket;
	uint8_t len = sizeof(struct mptcp_dsn_opt);
	struct mptcp_dsn_opt infin_opt;
	struct mptcb *mp_tp = NULL;
	uint8_t csum_len = 0;

	if (!so) {
		return optlen;
	}

	mp_tp = tptomptp(tp);
	if (mp_tp == NULL) {
		return optlen;
	}

	if (mp_tp->mpt_flags & MPTCPF_CHECKSUM) {
		csum_len = 2;
	}

	/* try later */
	if ((MAX_TCPOPTLEN - optlen) < (len + csum_len)) {
		return optlen;
	}

	bzero(&infin_opt, sizeof(infin_opt));
	infin_opt.mdss_copt.mdss_kind = TCPOPT_MULTIPATH;
	infin_opt.mdss_copt.mdss_len = len + csum_len;
	infin_opt.mdss_copt.mdss_subtype = MPO_DSS;
	infin_opt.mdss_copt.mdss_flags |= MDSS_M;
	if (mp_tp->mpt_flags & MPTCPF_RECVD_MPFAIL) {
		infin_opt.mdss_dsn = (u_int32_t)
		    MPTCP_DATASEQ_LOW32(mp_tp->mpt_dsn_at_csum_fail);
		infin_opt.mdss_subflow_seqn = mp_tp->mpt_ssn_at_csum_fail;
	} else {
		/*
		 * If MPTCP fallback happens, but TFO succeeds, the data on the
		 * SYN does not belong to the MPTCP data sequence space.
		 */
		if ((tp->t_tfo_stats & TFO_S_SYN_DATA_ACKED) &&
		    ((mp_tp->mpt_local_idsn + 1) == mp_tp->mpt_snduna)) {
			infin_opt.mdss_subflow_seqn = 1;
		} else {
			infin_opt.mdss_subflow_seqn = tp->snd_una - tp->t_mpsub->mpts_iss;
		}
		infin_opt.mdss_dsn = (u_int32_t)
		    MPTCP_DATASEQ_LOW32(mp_tp->mpt_snduna);
	}

	if ((infin_opt.mdss_dsn == 0) || (infin_opt.mdss_subflow_seqn == 0)) {
		return optlen;
	}
	infin_opt.mdss_dsn = htonl(infin_opt.mdss_dsn);
	infin_opt.mdss_subflow_seqn = htonl(infin_opt.mdss_subflow_seqn);
	infin_opt.mdss_data_len = 0;

	memcpy(opt + optlen, &infin_opt, len);
	optlen += len;
	if (csum_len != 0) {
		/* The checksum field is set to 0 for infinite mapping */
		uint16_t csum = 0;
		memcpy(opt + optlen, &csum, csum_len);
		optlen += csum_len;
	}

	tp->t_mpflags |= TMPF_INFIN_SENT;
	tcpstat.tcps_estab_fallback++;
	return optlen;
}


static int
mptcp_ok_to_fin(struct tcpcb *tp, u_int64_t dsn, u_int32_t datalen)
{
	struct mptcb *mp_tp = tptomptp(tp);

	dsn = (mp_tp->mpt_sndmax & MPTCP_DATASEQ_LOW32_MASK) | dsn;
	if ((dsn + datalen) == mp_tp->mpt_sndmax) {
		return 1;
	}

	return 0;
}

unsigned int
mptcp_setup_opts(struct tcpcb *tp, int32_t off, u_char *opt __ended_by(optend), u_char *optend,
    unsigned int optlen, int flags, int len,
    boolean_t *p_mptcp_acknow, boolean_t *do_not_compress)
{
	struct inpcb *inp = (struct inpcb *)tp->t_inpcb;
	struct socket *so = inp->inp_socket;
	struct mptcb *mp_tp = tptomptp(tp);
	boolean_t do_csum = FALSE;
	boolean_t send_64bit_dsn = FALSE;
	boolean_t send_64bit_ack = FALSE;
	uint32_t old_mpt_flags = tp->t_mpflags & TMPF_MPTCP_SIGNALS;
	boolean_t initial_data = FALSE;

	/* There is a case where offset can become negative. tcp_output()
	 * gracefully handles this. So, let's make MPTCP more robust as well.
	 */
	if (off < 0) {
		off = 0;
	}

	if (mptcp_enable == 0 || mp_tp == NULL || tp->t_state == TCPS_CLOSED) {
		/* do nothing */
		goto ret_optlen;
	}

	socket_lock_assert_owned(mptetoso(mp_tp->mpt_mpte));

	if (mp_tp->mpt_flags & MPTCPF_CHECKSUM) {
		do_csum = TRUE;
	}

	/* tcp_output handles the SYN path separately */
	if (flags & TH_SYN) {
		goto ret_optlen;
	}

	if ((MAX_TCPOPTLEN - optlen) <
	    sizeof(struct mptcp_mpcapable_opt_common)) {
		os_log_error(mptcp_log_handle, "%s - %lx: no space left %d flags %x tp->t_mpflags %x len %d\n",
		    __func__, (unsigned long)VM_KERNEL_ADDRPERM(mp_tp->mpt_mpte),
		    optlen, flags, tp->t_mpflags, len);
		goto ret_optlen;
	}

	if (tp->t_mpflags & TMPF_TCP_FALLBACK) {
		if (tp->t_mpflags & TMPF_SND_MPFAIL) {
			optlen = mptcp_send_mpfail(tp, opt, optend, optlen);
		} else if (!(tp->t_mpflags & TMPF_INFIN_SENT)) {
			optlen = mptcp_send_infinite_mapping(tp, opt, optend, optlen);
		}

		*do_not_compress = TRUE;

		goto ret_optlen;
	}

	if (len > 0 && off == 0 && tp->t_mpflags & TMPF_SEND_DSN && tp->t_mpflags & TMPF_SND_KEYS) {
		uint64_t dsn = 0;
		uint32_t relseq = 0;
		uint16_t data_len = 0, dss_csum = 0;
		mptcp_output_getm_dsnmap64(so, off, &dsn, &relseq, &data_len, &dss_csum);
		if (dsn == mp_tp->mpt_local_idsn + 1) {
			initial_data = TRUE;
		}
	}

	/* send MP_CAPABLE when it's the INITIAL ACK or data */
	if (tp->t_mpflags & TMPF_SND_KEYS &&
	    (mp_tp->mpt_version == MPTCP_VERSION_0 || initial_data ||
	    (mp_tp->mpt_sndnxt == mp_tp->mpt_local_idsn + 1 && len == 0))) {
		struct mptcp_mpcapable_opt_rsp2 mptcp_opt;
		boolean_t send_data_level_details = tp->t_mpflags & TMPF_SEND_DSN ? TRUE : FALSE;

		uint8_t mmco_len = sizeof(struct mptcp_mpcapable_opt_rsp1);
		if (send_data_level_details) {
			mmco_len += 2;
			if (do_csum) {
				mmco_len += 2;
			}
		}
		if ((MAX_TCPOPTLEN - optlen) < mmco_len) {
			os_log_error(mptcp_log_handle, "%s - %lx: not enough space in TCP option, "
			    "optlen: %u, mmco_len: %d\n", __func__,
			    (unsigned long)VM_KERNEL_ADDRPERM(mp_tp->mpt_mpte),
			    optlen, mmco_len);
			goto ret_optlen;
		}

		bzero(&mptcp_opt, sizeof(struct mptcp_mpcapable_opt_rsp2));
		mptcp_opt.mmc_rsp1.mmc_common.mmco_kind = TCPOPT_MULTIPATH;
		mptcp_opt.mmc_rsp1.mmc_common.mmco_len = mmco_len;
		mptcp_opt.mmc_rsp1.mmc_common.mmco_subtype = MPO_CAPABLE;
		mptcp_opt.mmc_rsp1.mmc_common.mmco_version = mp_tp->mpt_version;
		mptcp_opt.mmc_rsp1.mmc_common.mmco_flags |= MPCAP_PROPOSAL_SBIT;
		if (do_csum) {
			mptcp_opt.mmc_rsp1.mmc_common.mmco_flags |= MPCAP_CHECKSUM_CBIT;
		}
		mptcp_opt.mmc_rsp1.mmc_localkey = mp_tp->mpt_localkey;
		mptcp_opt.mmc_rsp1.mmc_remotekey = mp_tp->mpt_remotekey;
		if (send_data_level_details) {
			mptcp_output_getm_data_level_details(so, off, &mptcp_opt.data_len, &mptcp_opt.csum);
			mptcp_opt.data_len = htons(mptcp_opt.data_len);
		}
		memcpy(opt + optlen, &mptcp_opt, mmco_len);

		if (mp_tp->mpt_version == MPTCP_VERSION_0) {
			tp->t_mpflags &= ~TMPF_SND_KEYS;
		}
		optlen += mmco_len;

		if (!tp->t_mpuna) {
			tp->t_mpuna = tp->snd_una;
		} else {
			/* its a retransmission of the MP_CAPABLE ACK */
		}

		*do_not_compress = TRUE;

		goto ret_optlen;
	}

	if (tp->t_mpflags & TMPF_SND_JACK) {
		*do_not_compress = TRUE;
		optlen = mptcp_setup_join_ack_opts(tp, opt, optend, optlen);
		if (!tp->t_mpuna) {
			tp->t_mpuna = tp->snd_una;
		}
		/* Start a timer to retransmit the ACK */
		tp->t_timer[TCPT_JACK_RXMT] =
		    OFFSET_FROM_START(tp, tcp_jack_rxmt);

		tp->t_mpflags &= ~TMPF_SND_JACK;
		goto ret_optlen;
	}

	if (!(tp->t_mpflags & (TMPF_MPTCP_TRUE | TMPF_PREESTABLISHED))) {
		goto ret_optlen;
	}
	/*
	 * From here on, all options are sent only if MPTCP_TRUE
	 * or when data is sent early on as in Fast Join
	 */

	if ((tp->t_mpflags & TMPF_MPTCP_TRUE) &&
	    (tp->t_mpflags & TMPF_SND_REM_ADDR)) {
		int rem_opt_len = sizeof(struct mptcp_remaddr_opt);
		if (optlen + rem_opt_len <= MAX_TCPOPTLEN) {
			mptcp_send_remaddr_opt(tp,
			    (struct mptcp_remaddr_opt *)(opt + optlen));
			optlen += rem_opt_len;
		} else {
			tp->t_mpflags &= ~TMPF_SND_REM_ADDR;
		}

		*do_not_compress = TRUE;
	}

	if (tp->t_mpflags & TMPF_MPTCP_ECHO_ADDR) {
		optlen = mptcp_echo_add_addr(tp, opt, optend, optlen);
	}

	if (tp->t_mpflags & TMPF_SND_MPPRIO) {
		optlen = mptcp_snd_mpprio(tp, opt, optend, optlen);

		*do_not_compress = TRUE;
	}

	if (mp_tp->mpt_flags & MPTCPF_SND_64BITDSN) {
		send_64bit_dsn = TRUE;
	}
	if (mp_tp->mpt_flags & MPTCPF_SND_64BITACK) {
		send_64bit_ack = TRUE;
	}

#define CHECK_OPTLEN    {                                                                   \
	if (MAX_TCPOPTLEN - optlen < dssoptlen) {                                         \
	        os_log_error(mptcp_log_handle, "%s: dssoptlen %d optlen %d \n", __func__,   \
	            dssoptlen, optlen);                                                     \
	            goto ret_optlen;                                                        \
	}                                                                                   \
}

#define DO_FIN(dsn_opt) {                                               \
	int sndfin = 0;                                                 \
	sndfin = mptcp_ok_to_fin(tp, dsn_opt.mdss_dsn, len);            \
	if (sndfin) {                                                   \
	        dsn_opt.mdss_copt.mdss_flags |= MDSS_F;                 \
	        dsn_opt.mdss_data_len += 1;                             \
	        if (do_csum)                                            \
	                dss_csum = in_addword(dss_csum, 1);             \
	}                                                               \
}

#define CHECK_DATALEN {                                                             \
	/* MPTCP socket does not support IP options */                              \
	if ((len + optlen + dssoptlen) > tp->t_maxopd) {                            \
	        os_log_error(mptcp_log_handle, "%s: nosp %d len %d opt %d %d %d\n", \
	            __func__, len, dssoptlen, optlen,                               \
	            tp->t_maxseg, tp->t_maxopd);                                    \
	/* remove option length from payload len */                         \
	        len = tp->t_maxopd - optlen - dssoptlen;                            \
	}                                                                           \
}

	if ((tp->t_mpflags & TMPF_SEND_DSN) &&
	    (send_64bit_dsn)) {
		/*
		 * If there was the need to send 64-bit Data ACK along
		 * with 64-bit DSN, then 26 or 28 bytes would be used.
		 * With timestamps and NOOP padding that will cause
		 * overflow. Hence, in the rare event that both 64-bit
		 * DSN and 64-bit ACK have to be sent, delay the send of
		 * 64-bit ACK until our 64-bit DSN is acked with a 64-bit ack.
		 * XXX If this delay causes issue, remove the 2-byte padding.
		 */
		struct mptcp_dss64_ack32_opt dsn_ack_opt;
		uint8_t dssoptlen = sizeof(dsn_ack_opt);
		uint16_t dss_csum;

		if (do_csum) {
			dssoptlen += 2;
		}

		CHECK_OPTLEN;

		bzero(&dsn_ack_opt, sizeof(dsn_ack_opt));
		dsn_ack_opt.mdss_copt.mdss_kind = TCPOPT_MULTIPATH;
		dsn_ack_opt.mdss_copt.mdss_subtype = MPO_DSS;
		dsn_ack_opt.mdss_copt.mdss_len = dssoptlen;
		dsn_ack_opt.mdss_copt.mdss_flags |=
		    MDSS_M | MDSS_m | MDSS_A;

		CHECK_DATALEN;

		mptcp_output_getm_dsnmap64(so, off,
		    &dsn_ack_opt.mdss_dsn,
		    &dsn_ack_opt.mdss_subflow_seqn,
		    &dsn_ack_opt.mdss_data_len,
		    &dss_csum);

		if ((dsn_ack_opt.mdss_data_len == 0) ||
		    (dsn_ack_opt.mdss_dsn == 0)) {
			goto ret_optlen;
		}

		if (tp->t_mpflags & TMPF_SEND_DFIN) {
			DO_FIN(dsn_ack_opt);
		}

		dsn_ack_opt.mdss_ack =
		    htonl(MPTCP_DATAACK_LOW32(mp_tp->mpt_rcvnxt));

		dsn_ack_opt.mdss_dsn = mptcp_hton64(dsn_ack_opt.mdss_dsn);
		dsn_ack_opt.mdss_subflow_seqn = htonl(
			dsn_ack_opt.mdss_subflow_seqn);
		dsn_ack_opt.mdss_data_len = htons(
			dsn_ack_opt.mdss_data_len);

		memcpy(opt + optlen, &dsn_ack_opt, sizeof(dsn_ack_opt));
		if (do_csum) {
			*((uint16_t *)(void *)(opt + optlen + sizeof(dsn_ack_opt))) = dss_csum;
		}

		optlen += dssoptlen;

		tp->t_mpflags &= ~TMPF_MPTCP_ACKNOW;

		*do_not_compress = TRUE;

		goto ret_optlen;
	}

	if ((tp->t_mpflags & TMPF_SEND_DSN) &&
	    (!send_64bit_dsn) &&
	    !(tp->t_mpflags & TMPF_MPTCP_ACKNOW)) {
		struct mptcp_dsn_opt dsn_opt;
		uint8_t dssoptlen = sizeof(struct mptcp_dsn_opt);
		uint16_t dss_csum;

		if (do_csum) {
			dssoptlen += 2;
		}

		CHECK_OPTLEN;

		bzero(&dsn_opt, sizeof(dsn_opt));
		dsn_opt.mdss_copt.mdss_kind = TCPOPT_MULTIPATH;
		dsn_opt.mdss_copt.mdss_subtype = MPO_DSS;
		dsn_opt.mdss_copt.mdss_len = dssoptlen;
		dsn_opt.mdss_copt.mdss_flags |= MDSS_M;

		CHECK_DATALEN;

		mptcp_output_getm_dsnmap32(so, off, &dsn_opt.mdss_dsn,
		    &dsn_opt.mdss_subflow_seqn,
		    &dsn_opt.mdss_data_len,
		    &dss_csum);

		if ((dsn_opt.mdss_data_len == 0) ||
		    (dsn_opt.mdss_dsn == 0)) {
			goto ret_optlen;
		}

		if (tp->t_mpflags & TMPF_SEND_DFIN) {
			DO_FIN(dsn_opt);
		}

		dsn_opt.mdss_dsn = htonl(dsn_opt.mdss_dsn);
		dsn_opt.mdss_subflow_seqn = htonl(dsn_opt.mdss_subflow_seqn);
		dsn_opt.mdss_data_len = htons(dsn_opt.mdss_data_len);
		memcpy(opt + optlen, &dsn_opt, sizeof(dsn_opt));
		if (do_csum) {
			*((uint16_t *)(void *)(opt + optlen + sizeof(dsn_opt))) = dss_csum;
		}

		optlen += dssoptlen;
		tp->t_mpflags &= ~TMPF_MPTCP_ACKNOW;

		*do_not_compress = TRUE;

		goto ret_optlen;
	}

	/* 32-bit Data ACK option */
	if ((tp->t_mpflags & TMPF_MPTCP_ACKNOW) &&
	    (!send_64bit_ack) &&
	    !(tp->t_mpflags & TMPF_SEND_DSN) &&
	    !(tp->t_mpflags & TMPF_SEND_DFIN)) {
		struct mptcp_data_ack_opt dack_opt;
		uint8_t dssoptlen = 0;
do_ack32_only:
		dssoptlen = sizeof(dack_opt);

		CHECK_OPTLEN;

		bzero(&dack_opt, dssoptlen);
		dack_opt.mdss_copt.mdss_kind = TCPOPT_MULTIPATH;
		dack_opt.mdss_copt.mdss_len = dssoptlen;
		dack_opt.mdss_copt.mdss_subtype = MPO_DSS;
		dack_opt.mdss_copt.mdss_flags |= MDSS_A;
		dack_opt.mdss_ack =
		    htonl(MPTCP_DATAACK_LOW32(mp_tp->mpt_rcvnxt));
		memcpy(opt + optlen, &dack_opt, dssoptlen);
		optlen += dssoptlen;
		VERIFY(optlen <= MAX_TCPOPTLEN);
		tp->t_mpflags &= ~TMPF_MPTCP_ACKNOW;
		goto ret_optlen;
	}

	/* 64-bit Data ACK option */
	if ((tp->t_mpflags & TMPF_MPTCP_ACKNOW) &&
	    (send_64bit_ack) &&
	    !(tp->t_mpflags & TMPF_SEND_DSN) &&
	    !(tp->t_mpflags & TMPF_SEND_DFIN)) {
		struct mptcp_data_ack64_opt dack_opt;
		uint8_t dssoptlen = 0;
do_ack64_only:
		dssoptlen = sizeof(dack_opt);

		CHECK_OPTLEN;

		bzero(&dack_opt, dssoptlen);
		dack_opt.mdss_copt.mdss_kind = TCPOPT_MULTIPATH;
		dack_opt.mdss_copt.mdss_len = dssoptlen;
		dack_opt.mdss_copt.mdss_subtype = MPO_DSS;
		dack_opt.mdss_copt.mdss_flags |= (MDSS_A | MDSS_a);
		dack_opt.mdss_ack = mptcp_hton64(mp_tp->mpt_rcvnxt);
		/*
		 * The other end should retransmit 64-bit DSN until it
		 * receives a 64-bit ACK.
		 */
		mp_tp->mpt_flags &= ~MPTCPF_SND_64BITACK;
		memcpy(opt + optlen, &dack_opt, dssoptlen);
		optlen += dssoptlen;
		VERIFY(optlen <= MAX_TCPOPTLEN);
		tp->t_mpflags &= ~TMPF_MPTCP_ACKNOW;
		goto ret_optlen;
	}

	/* 32-bit DSS+Data ACK option */
	if ((tp->t_mpflags & TMPF_SEND_DSN) &&
	    (!send_64bit_dsn) &&
	    (!send_64bit_ack) &&
	    (tp->t_mpflags & TMPF_MPTCP_ACKNOW)) {
		struct mptcp_dss_ack_opt dss_ack_opt;
		uint8_t dssoptlen = sizeof(dss_ack_opt);
		uint16_t dss_csum;

		if (do_csum) {
			dssoptlen += 2;
		}

		CHECK_OPTLEN;

		bzero(&dss_ack_opt, sizeof(dss_ack_opt));
		dss_ack_opt.mdss_copt.mdss_kind = TCPOPT_MULTIPATH;
		dss_ack_opt.mdss_copt.mdss_len = dssoptlen;
		dss_ack_opt.mdss_copt.mdss_subtype = MPO_DSS;
		dss_ack_opt.mdss_copt.mdss_flags |= MDSS_A | MDSS_M;
		dss_ack_opt.mdss_ack =
		    htonl(MPTCP_DATAACK_LOW32(mp_tp->mpt_rcvnxt));

		CHECK_DATALEN;

		mptcp_output_getm_dsnmap32(so, off, &dss_ack_opt.mdss_dsn,
		    &dss_ack_opt.mdss_subflow_seqn,
		    &dss_ack_opt.mdss_data_len,
		    &dss_csum);

		if ((dss_ack_opt.mdss_data_len == 0) ||
		    (dss_ack_opt.mdss_dsn == 0)) {
			goto do_ack32_only;
		}

		if (tp->t_mpflags & TMPF_SEND_DFIN) {
			DO_FIN(dss_ack_opt);
		}

		dss_ack_opt.mdss_dsn = htonl(dss_ack_opt.mdss_dsn);
		dss_ack_opt.mdss_subflow_seqn =
		    htonl(dss_ack_opt.mdss_subflow_seqn);
		dss_ack_opt.mdss_data_len = htons(dss_ack_opt.mdss_data_len);
		memcpy(opt + optlen, &dss_ack_opt, sizeof(dss_ack_opt));
		if (do_csum) {
			*((uint16_t *)(void *)(opt + optlen + sizeof(dss_ack_opt))) = dss_csum;
		}

		optlen += dssoptlen;

		if (optlen > MAX_TCPOPTLEN) {
			panic("optlen too large");
		}
		tp->t_mpflags &= ~TMPF_MPTCP_ACKNOW;
		goto ret_optlen;
	}

	/* 32-bit DSS + 64-bit DACK option */
	if ((tp->t_mpflags & TMPF_SEND_DSN) &&
	    (!send_64bit_dsn) &&
	    (send_64bit_ack) &&
	    (tp->t_mpflags & TMPF_MPTCP_ACKNOW)) {
		struct mptcp_dss32_ack64_opt dss_ack_opt;
		uint8_t dssoptlen = sizeof(dss_ack_opt);
		uint16_t dss_csum;

		if (do_csum) {
			dssoptlen += 2;
		}

		CHECK_OPTLEN;

		bzero(&dss_ack_opt, sizeof(dss_ack_opt));
		dss_ack_opt.mdss_copt.mdss_kind = TCPOPT_MULTIPATH;
		dss_ack_opt.mdss_copt.mdss_len = dssoptlen;
		dss_ack_opt.mdss_copt.mdss_subtype = MPO_DSS;
		dss_ack_opt.mdss_copt.mdss_flags |= MDSS_M | MDSS_A | MDSS_a;
		dss_ack_opt.mdss_ack =
		    mptcp_hton64(mp_tp->mpt_rcvnxt);

		CHECK_DATALEN;

		mptcp_output_getm_dsnmap32(so, off, &dss_ack_opt.mdss_dsn,
		    &dss_ack_opt.mdss_subflow_seqn,
		    &dss_ack_opt.mdss_data_len,
		    &dss_csum);

		if ((dss_ack_opt.mdss_data_len == 0) ||
		    (dss_ack_opt.mdss_dsn == 0)) {
			goto do_ack64_only;
		}

		if (tp->t_mpflags & TMPF_SEND_DFIN) {
			DO_FIN(dss_ack_opt);
		}

		dss_ack_opt.mdss_dsn = htonl(dss_ack_opt.mdss_dsn);
		dss_ack_opt.mdss_subflow_seqn =
		    htonl(dss_ack_opt.mdss_subflow_seqn);
		dss_ack_opt.mdss_data_len = htons(dss_ack_opt.mdss_data_len);
		memcpy(opt + optlen, &dss_ack_opt, sizeof(dss_ack_opt));
		if (do_csum) {
			*((uint16_t *)(void *)(opt + optlen + sizeof(dss_ack_opt))) = dss_csum;
		}

		optlen += dssoptlen;

		if (optlen > MAX_TCPOPTLEN) {
			panic("optlen too large");
		}
		tp->t_mpflags &= ~TMPF_MPTCP_ACKNOW;

		*do_not_compress = TRUE;

		goto ret_optlen;
	}

	if (tp->t_mpflags & TMPF_SEND_DFIN) {
		uint8_t dssoptlen = sizeof(struct mptcp_dss_ack_opt);
		struct mptcp_dss_ack_opt dss_ack_opt;
		uint16_t dss_csum;

		if (do_csum) {
			uint64_t dss_val = mptcp_hton64(mp_tp->mpt_sndmax - 1);
			uint16_t dlen = htons(1);
			uint32_t sseq = 0;
			uint32_t sum;


			dssoptlen += 2;

			sum = in_pseudo64(dss_val, sseq, dlen);
			ADDCARRY(sum);
			dss_csum = ~sum & 0xffff;
		}

		CHECK_OPTLEN;

		bzero(&dss_ack_opt, sizeof(dss_ack_opt));

		/*
		 * Data FIN occupies one sequence space.
		 * Don't send it if it has been Acked.
		 */
		if ((mp_tp->mpt_sndnxt + 1 != mp_tp->mpt_sndmax) ||
		    (mp_tp->mpt_snduna == mp_tp->mpt_sndmax)) {
			goto ret_optlen;
		}

		dss_ack_opt.mdss_copt.mdss_kind = TCPOPT_MULTIPATH;
		dss_ack_opt.mdss_copt.mdss_len = dssoptlen;
		dss_ack_opt.mdss_copt.mdss_subtype = MPO_DSS;
		dss_ack_opt.mdss_copt.mdss_flags |= MDSS_A | MDSS_M | MDSS_F;
		dss_ack_opt.mdss_ack =
		    htonl(MPTCP_DATAACK_LOW32(mp_tp->mpt_rcvnxt));
		dss_ack_opt.mdss_dsn =
		    htonl(MPTCP_DATASEQ_LOW32(mp_tp->mpt_sndmax - 1));
		dss_ack_opt.mdss_subflow_seqn = 0;
		dss_ack_opt.mdss_data_len = 1;
		dss_ack_opt.mdss_data_len = htons(dss_ack_opt.mdss_data_len);
		memcpy(opt + optlen, &dss_ack_opt, sizeof(dss_ack_opt));
		if (do_csum) {
			*((uint16_t *)(void *)(opt + optlen + sizeof(dss_ack_opt))) = dss_csum;
		}

		optlen += dssoptlen;

		*do_not_compress = TRUE;
	}

ret_optlen:
	if (TRUE == *p_mptcp_acknow) {
		uint32_t new_mpt_flags = tp->t_mpflags & TMPF_MPTCP_SIGNALS;

		/*
		 * If none of the above mpflags were acted on by
		 * this routine, reset these flags and set p_mptcp_acknow
		 * to false.
		 *
		 * XXX The reset value of p_mptcp_acknow can be used
		 * to communicate tcp_output to NOT send a pure ack without any
		 * MPTCP options as it will be treated as a dup ack.
		 * Since the instances of mptcp_setup_opts not acting on
		 * these options are mostly corner cases and sending a dup
		 * ack here would only have an impact if the system
		 * has sent consecutive dup acks before this false one,
		 * we haven't modified the logic in tcp_output to avoid
		 * that.
		 */
		if (old_mpt_flags == new_mpt_flags) {
			tp->t_mpflags &= ~TMPF_MPTCP_SIGNALS;
			*p_mptcp_acknow = FALSE;
		}
	}

	return optlen;
}

/*
 * MPTCP Options Input Processing
 */

/*
 * In most cases, option can be parsed by performing the cast
 *
 *       opt_type *opt = (opt_type*)optp;
 *
 * However, in some cases there will be less bytes on the wire
 * the size of the corresponding C struct, i.e.:
 *
 *              (optend - optp) < sizeof(opt_type)
 *
 * In such cases, the bounds of `opt' will be smaller than
 * the size of its declared pointee type. Any attempt to
 * dereference `opt' (or to access its fields)
 * will lead to an `-fbounds-safety' trap.
 *
 * To prevent such undesirable situation, we are using
 * the "shadow storage" pattern:
 * - If there are enough bytes so that the cast expression
 *       opt_type *opt = (opt_type*)optp;
 *   will produce a "valid" pointer, we will perform a cast.
 * - Otherwise, we will copy the bytes into a stack allocated
 *   structure, and return a pointer to that structure.
 *
 * If the `VERBOSE_OPTION_PARSING_LOGGING' is set to 1,
 * the code will produce additional logging at the detriment
 * of performance. This is off by default, but the code is kept for now.
 */
#define VERBOSE_OPTION_PARSING_LOGGING 0
#if VERBOSE_OPTION_PARSING_LOGGING

#define MPTCP_OPT_CHECK_UNDERRUN(shadow_opt, optlen)  do {                                  \
	if (__improbable(sizeof((shadow_opt)) < (optlen))) {                                    \
	        size_t ignored = (optlen) - sizeof((shadow_opt));                               \
	        os_log(mptcp_log_handle,                                                        \
	                "%s - option length exceeds the size of underlying storage "            \
	                "(optlen=%lu, storage size=%lu) %lu bytes will be ignored\n",           \
	                __func__, (size_t)(optlen), sizeof((shadow_opt)), ignored);             \
	}                                                                                       \
} while(0)

#define MPTCP_OPT_REPORT_COPY(shadow_opt, available)  do {                                  \
	os_log(mptcp_log_handle,                                                                \
	        "%s - insufficent input to use cast-parsing (required=%lu; available=%ld); "    \
	        " option data will be copied to local storage\n",                               \
	                __func__, sizeof((shadow_opt)), available);                             \
                                                                                            \
} while(0)

#else /* !VERBOSE_OPTION_PARSING_LOGGING*/

#define MPTCP_OPT_CHECK_UNDERRUN(shadow_opt, optlen)  do {                                 \
	(void)(optlen);                                                                        \
} while(0)

#define MPTCP_OPT_REPORT_COPY(shadow_opt, optlen)     do {                                 \
	(void)(optlen);                                                                        \
} while(0)
#endif /* DEBUG || DEVELOPMENT */


#define MPTCP_OPT_GET(shadow_opt, optp, optend, optlen)   ({                                \
	__typeof__((shadow_opt)) * __single opt_ptr;                                            \
                                                                                            \
	ptrdiff_t available = (optend) - (optp);                                                \
                                                                                            \
    MPTCP_OPT_CHECK_UNDERRUN(shadow_opt, optlen);                                           \
                                                                                            \
	if (__improbable(available < sizeof((shadow_opt)))) {                                   \
	        MPTCP_OPT_REPORT_COPY(shadow_opt, available);                                   \
	        memset((caddr_t)&(shadow_opt) + available,                                      \
	                0, sizeof((shadow_opt)) - available);                                   \
	        memcpy(&(shadow_opt), (optp), available);                                       \
	        opt_ptr = &(shadow_opt);                                                        \
	} else {                                                                                \
	        opt_ptr = __unsafe_forge_single(__typeof__((shadow_opt))*, (optp));             \
	}                                                                                       \
	opt_ptr;                                                                                \
})

static int
mptcp_sanitize_option(struct tcpcb *tp, int mptcp_subtype)
{
	struct mptcb *mp_tp = tptomptp(tp);
	int ret = 1;

	switch (mptcp_subtype) {
	case MPO_CAPABLE:
		break;
	case MPO_JOIN:                  /* fall through */
	case MPO_DSS:                   /* fall through */
	case MPO_FASTCLOSE:             /* fall through */
	case MPO_FAIL:                  /* fall through */
	case MPO_REMOVE_ADDR:           /* fall through */
	case MPO_ADD_ADDR:              /* fall through */
	case MPO_PRIO:                  /* fall through */
		if (mp_tp->mpt_state < MPTCPS_ESTABLISHED) {
			ret = 0;
		}
		break;
	default:
		ret = 0;
		os_log_error(mptcp_log_handle, "%s - %lx: type = %d \n", __func__,
		    (unsigned long)VM_KERNEL_ADDRPERM(mp_tp->mpt_mpte), mptcp_subtype);
		break;
	}
	return ret;
}

static int
mptcp_valid_mpcapable_common_opt(struct mptcp_mpcapable_opt_common  *crsp)
{
	/* mmco_kind, mmco_len and mmco_subtype are validated before */

	if (!(crsp->mmco_flags & MPCAP_PROPOSAL_SBIT)) {
		return 0;
	}

	if (crsp->mmco_flags & (MPCAP_BBIT | MPCAP_DBIT |
	    MPCAP_EBIT | MPCAP_FBIT | MPCAP_GBIT)) {
		return 0;
	}

	return 1;
}


static void
mptcp_do_mpcapable_opt(struct tcpcb *tp, u_char *cp __ended_by(optend), u_char *optend, struct tcphdr *th,
    uint8_t optlen)
{
	struct mptcp_mpcapable_opt_common   crsp_s, *crsp;
	crsp = MPTCP_OPT_GET(crsp_s, cp, optend, optlen);
	struct mptcp_mpcapable_opt_rsp rsp_s, *rsp = NULL;
	struct mptcb *mp_tp = tptomptp(tp);
	struct mptses *mpte = mp_tp->mpt_mpte;

	/* Only valid on SYN/ACK */
	if ((th->th_flags & (TH_SYN | TH_ACK)) != (TH_SYN | TH_ACK)) {
		return;
	}

	/* Validate the kind, len, flags */
	if (mptcp_valid_mpcapable_common_opt(crsp) != 1) {
		tcpstat.tcps_invalid_mpcap++;
		return;
	}

	/* handle SYN/ACK retransmission by acknowledging with ACK */
	if (mp_tp->mpt_state >= MPTCPS_ESTABLISHED) {
		return;
	}

	/* A SYN/ACK contains peer's key and flags */
	if (optlen != sizeof(struct mptcp_mpcapable_opt_rsp)) {
		/* complain */
		os_log_error(mptcp_log_handle, "%s - %lx: SYN_ACK optlen = %u, sizeof mp opt = %lu \n",
		    __func__, (unsigned long)VM_KERNEL_ADDRPERM(mpte), optlen,
		    sizeof(struct mptcp_mpcapable_opt_rsp));
		tcpstat.tcps_invalid_mpcap++;
		return;
	}

	/*
	 * If checksum flag is set, enable MPTCP checksum, even if
	 * it was not negotiated on the first SYN.
	 */
	if (crsp->mmco_flags & MPCAP_CHECKSUM_CBIT) {
		mp_tp->mpt_flags |= MPTCPF_CHECKSUM;
	}

	if (crsp->mmco_flags & MPCAP_UNICAST_IPBIT) {
		mpte->mpte_flags |= MPTE_UNICAST_IP;

		/* We need an explicit signal for the addresses - zero the existing ones */
		memset(&mpte->mpte_sub_dst_v4, 0, sizeof(mpte->mpte_sub_dst_v4));
		memset(&mpte->mpte_sub_dst_v6, 0, sizeof(mpte->mpte_sub_dst_v6));
	}

	rsp = MPTCP_OPT_GET(rsp_s, cp, optend, optlen);
	mp_tp->mpt_remotekey = rsp->mmc_localkey;
	/* For now just downgrade to the peer's version */
	if (rsp->mmc_common.mmco_version < mp_tp->mpt_version) {
		os_log_error(mptcp_log_handle, "local version: %d > peer version %d", mp_tp->mpt_version, rsp->mmc_common.mmco_version);
		mp_tp->mpt_version = rsp->mmc_common.mmco_version;
		tcpstat.tcps_mp_verdowngrade++;
		return;
	}
	if (mptcp_init_remote_parms(mp_tp) != 0) {
		tcpstat.tcps_invalid_mpcap++;
		return;
	}
	tcp_heuristic_mptcp_success(tp);
	tcp_cache_update_mptcp_version(tp, TRUE);
	tp->t_mpflags |= (TMPF_SND_KEYS | TMPF_MPTCP_TRUE);
}


static void
mptcp_do_mpjoin_opt(struct tcpcb *tp, u_char *cp __ended_by(optend), u_char *optend, struct tcphdr *th, uint8_t optlen)
{
#define MPTCP_JOPT_ERROR_PATH(tp) {                                     \
	tcpstat.tcps_invalid_joins++;                                   \
	if (tp->t_inpcb->inp_socket != NULL) {                          \
	        soevent(tp->t_inpcb->inp_socket,                        \
	            SO_FILT_HINT_LOCKED | SO_FILT_HINT_MUSTRST);        \
	}                                                               \
}
	int error = 0;
	struct mptcp_mpjoin_opt_rsp join_rsp_s, *join_rsp;
	join_rsp = MPTCP_OPT_GET(join_rsp_s, cp, optend, optlen);

	/* Only valid on SYN/ACK */
	if ((th->th_flags & (TH_SYN | TH_ACK)) != (TH_SYN | TH_ACK)) {
		return;
	}

	if (optlen != sizeof(struct mptcp_mpjoin_opt_rsp)) {
		os_log_error(mptcp_log_handle, "%s - %lx: SYN_ACK: unexpected optlen = %u mp option = %lu\n",
		    __func__, (unsigned long)VM_KERNEL_ADDRPERM(tptomptp(tp)->mpt_mpte),
		    optlen, sizeof(struct mptcp_mpjoin_opt_rsp));
		tp->t_mpflags &= ~TMPF_PREESTABLISHED;
		/* send RST and close */
		MPTCP_JOPT_ERROR_PATH(tp);
		return;
	}

	mptcp_set_raddr_rand(tp->t_local_aid, tptomptp(tp),
	    join_rsp->mmjo_addr_id, join_rsp->mmjo_rand);
	error = mptcp_validate_join_hmac(tp,
	    (u_char*)&join_rsp->mmjo_mac, HMAC_TRUNCATED_SYNACK);
	if (error) {
		os_log_error(mptcp_log_handle, "%s - %lx: SYN_ACK error = %d \n",
		    __func__, (unsigned long)VM_KERNEL_ADDRPERM(tptomptp(tp)->mpt_mpte),
		    error);
		tp->t_mpflags &= ~TMPF_PREESTABLISHED;
		/* send RST and close */
		MPTCP_JOPT_ERROR_PATH(tp);
		return;
	}
	tp->t_mpflags |= (TMPF_SENT_JOIN | TMPF_SND_JACK);
}

static int
mptcp_validate_join_hmac(struct tcpcb *tp, u_char* hmac __sized_by(mac_len), int mac_len)
{
	u_char digest[MAX(SHA1_RESULTLEN, SHA256_DIGEST_LENGTH)] = {0};
	struct mptcb *mp_tp = tptomptp(tp);
	u_int32_t rem_rand, loc_rand;

	rem_rand = loc_rand = 0;

	mptcp_get_rands(tp->t_local_aid, mp_tp, &loc_rand, &rem_rand);
	if ((rem_rand == 0) || (loc_rand == 0)) {
		return -1;
	}

	if (mp_tp->mpt_version == MPTCP_VERSION_0) {
		mptcp_hmac_sha1(mp_tp->mpt_remotekey, mp_tp->mpt_localkey, rem_rand, loc_rand,
		    digest);
	} else {
		uint32_t data[2];
		data[0] = rem_rand;
		data[1] = loc_rand;
		mptcp_hmac_sha256(mp_tp->mpt_remotekey, mp_tp->mpt_localkey, (u_char *)data, 8, digest);
	}

	if (bcmp(digest, hmac, mac_len) == 0) {
		return 0; /* matches */
	} else {
		printf("%s: remote key %llx local key %llx remote rand %x "
		    "local rand %x \n", __func__, mp_tp->mpt_remotekey, mp_tp->mpt_localkey,
		    rem_rand, loc_rand);
		return -1;
	}
}

/*
 * Update the mptcb send state variables, but the actual sbdrop occurs
 * in MPTCP layer
 */
void
mptcp_data_ack_rcvd(struct mptcb *mp_tp, struct tcpcb *tp, u_int64_t full_dack)
{
	uint64_t acked = full_dack - mp_tp->mpt_snduna;

	VERIFY(acked <= INT_MAX);

	if (acked) {
		struct socket *mp_so = mptetoso(mp_tp->mpt_mpte);

		if (acked > mp_so->so_snd.sb_cc) {
			if (acked > mp_so->so_snd.sb_cc + 1 ||
			    mp_tp->mpt_state < MPTCPS_FIN_WAIT_1) {
				os_log_error(mptcp_log_handle, "%s - %lx: acked %u, sb_cc %u full %u suna %u state %u\n",
				    __func__, (unsigned long)VM_KERNEL_ADDRPERM(mp_tp->mpt_mpte),
				    (uint32_t)acked, mp_so->so_snd.sb_cc,
				    (uint32_t)full_dack, (uint32_t)mp_tp->mpt_snduna,
				    mp_tp->mpt_state);
			}

			sbdrop(&mp_so->so_snd, (int)mp_so->so_snd.sb_cc);
		} else {
			sbdrop(&mp_so->so_snd, (int)acked);
		}

		mp_tp->mpt_snduna += acked;
		/* In degraded mode, we may get some Data ACKs */
		if ((tp->t_mpflags & TMPF_TCP_FALLBACK) &&
		    !(mp_tp->mpt_flags & MPTCPF_POST_FALLBACK_SYNC) &&
		    MPTCP_SEQ_GT(mp_tp->mpt_sndnxt, mp_tp->mpt_snduna)) {
			/* bring back sndnxt to retransmit MPTCP data */
			mp_tp->mpt_sndnxt = mp_tp->mpt_dsn_at_csum_fail;
			mp_tp->mpt_flags |= MPTCPF_POST_FALLBACK_SYNC;
			tp->t_inpcb->inp_socket->so_flags1 |=
			    SOF1_POST_FALLBACK_SYNC;
		}

		mptcp_clean_reinjectq(mp_tp->mpt_mpte);

		sowwakeup(mp_so);
	}
	if (full_dack == mp_tp->mpt_sndmax &&
	    mp_tp->mpt_state >= MPTCPS_FIN_WAIT_1) {
		mptcp_close_fsm(mp_tp, MPCE_RECV_DATA_ACK);
		tp->t_mpflags &= ~TMPF_SEND_DFIN;
	}

	if ((tp->t_mpflags & TMPF_SND_KEYS) &&
	    MPTCP_SEQ_GT(mp_tp->mpt_snduna, mp_tp->mpt_local_idsn + 1)) {
		tp->t_mpflags &= ~TMPF_SND_KEYS;
	}
}

void
mptcp_update_window_wakeup(struct tcpcb *tp)
{
	struct mptcb *mp_tp = tptomptp(tp);

	socket_lock_assert_owned(mptetoso(mp_tp->mpt_mpte));

	if (mp_tp->mpt_flags & MPTCPF_FALLBACK_TO_TCP) {
		mp_tp->mpt_sndwnd = tp->snd_wnd;
		mp_tp->mpt_sndwl1 = mp_tp->mpt_rcvnxt;
		mp_tp->mpt_sndwl2 = mp_tp->mpt_snduna;
	}

	sowwakeup(tp->t_inpcb->inp_socket);
}

static void
mptcp_update_window(struct mptcb *mp_tp, u_int64_t ack, u_int64_t seq, u_int32_t tiwin)
{
	if (MPTCP_SEQ_LT(mp_tp->mpt_sndwl1, seq) ||
	    (mp_tp->mpt_sndwl1 == seq &&
	    (MPTCP_SEQ_LT(mp_tp->mpt_sndwl2, ack) ||
	    (mp_tp->mpt_sndwl2 == ack && tiwin > mp_tp->mpt_sndwnd)))) {
		mp_tp->mpt_sndwnd = tiwin;
		mp_tp->mpt_sndwl1 = seq;
		mp_tp->mpt_sndwl2 = ack;
	}
}

static void
mptcp_do_dss_opt_ack_meat(u_int64_t full_dack, u_int64_t full_dsn,
    struct tcpcb *tp, u_int32_t tiwin)
{
	struct mptcb *mp_tp = tptomptp(tp);
	int close_notify = 0;

	tp->t_mpflags |= TMPF_RCVD_DACK;

	if (MPTCP_SEQ_LEQ(full_dack, mp_tp->mpt_sndmax) &&
	    MPTCP_SEQ_GEQ(full_dack, mp_tp->mpt_snduna)) {
		mptcp_data_ack_rcvd(mp_tp, tp, full_dack);
		if (mp_tp->mpt_state > MPTCPS_FIN_WAIT_2) {
			close_notify = 1;
		}
		if (mp_tp->mpt_flags & MPTCPF_RCVD_64BITACK) {
			mp_tp->mpt_flags &= ~MPTCPF_RCVD_64BITACK;
			mp_tp->mpt_flags &= ~MPTCPF_SND_64BITDSN;
		}
		mptcp_notify_mpready(tp->t_inpcb->inp_socket);
		if (close_notify) {
			mptcp_notify_close(tp->t_inpcb->inp_socket);
		}
	}

	mptcp_update_window(mp_tp, full_dack, full_dsn, tiwin);
}

static void
mptcp_do_dss_opt_meat(u_char *cp __ended_by(optend), u_char *optend __unused, struct tcpcb *tp, struct tcphdr *th, uint8_t optlen)
{
	struct mptcp_dss_copt dss_rsp_s, *dss_rsp;
	dss_rsp = MPTCP_OPT_GET(dss_rsp_s, cp, optend, optlen);
	u_int64_t full_dack = 0;
	u_int32_t tiwin = th->th_win << tp->snd_scale;
	struct mptcb *mp_tp = tptomptp(tp);
	int csum_len = 0;

#define MPTCP_DSS_OPT_SZ_CHK(len, expected_len) {                                 \
	if (len != expected_len) {                                                \
	        os_log_error(mptcp_log_handle, "%s - %lx: bad len = %d dss: %x\n",\
	            __func__, (unsigned long)VM_KERNEL_ADDRPERM(mp_tp->mpt_mpte), \
	            len, dss_rsp->mdss_flags);                                    \
	        return;                                                           \
	}                                                                         \
}

	if (mp_tp->mpt_flags & MPTCPF_CHECKSUM) {
		csum_len = 2;
	}

	dss_rsp->mdss_flags &= (MDSS_A | MDSS_a | MDSS_M | MDSS_m);
	switch (dss_rsp->mdss_flags) {
	case (MDSS_M):
	{
		/* 32-bit DSS, No Data ACK */
		struct mptcp_dsn_opt dss_rsp1_s, *dss_rsp1;
		dss_rsp1 = MPTCP_OPT_GET(dss_rsp1_s, cp, optend, optlen);

		MPTCP_DSS_OPT_SZ_CHK(dss_rsp1->mdss_copt.mdss_len,
		    sizeof(struct mptcp_dsn_opt) + csum_len);
		if (csum_len == 0) {
			mptcp_update_dss_rcv_state(dss_rsp1, tp, 0);
		} else {
			mptcp_update_dss_rcv_state(dss_rsp1, tp,
			    *(uint16_t *)(void *)(cp +
			    (dss_rsp1->mdss_copt.mdss_len - csum_len)));
		}
		break;
	}
	case (MDSS_A):
	{
		/* 32-bit Data ACK, no DSS */
		struct mptcp_data_ack_opt dack_opt_s, *dack_opt;
		dack_opt = MPTCP_OPT_GET(dack_opt_s, cp, optend, optlen);

		MPTCP_DSS_OPT_SZ_CHK(dack_opt->mdss_copt.mdss_len,
		    sizeof(struct mptcp_data_ack_opt));

		u_int32_t dack = dack_opt->mdss_ack;
		NTOHL(dack);
		MPTCP_EXTEND_DSN(mp_tp->mpt_snduna, dack, full_dack);
		mptcp_do_dss_opt_ack_meat(full_dack, mp_tp->mpt_sndwl1, tp, tiwin);
		break;
	}
	case (MDSS_M | MDSS_A):
	{
		/* 32-bit Data ACK + 32-bit DSS */
		struct mptcp_dss_ack_opt dss_ack_rsp_s, *dss_ack_rsp;
		dss_ack_rsp = MPTCP_OPT_GET(dss_ack_rsp_s, cp, optend, optlen);
		u_int64_t full_dsn;
		uint16_t csum = 0;

		MPTCP_DSS_OPT_SZ_CHK(dss_ack_rsp->mdss_copt.mdss_len,
		    sizeof(struct mptcp_dss_ack_opt) + csum_len);

		u_int32_t dack = dss_ack_rsp->mdss_ack;
		NTOHL(dack);
		MPTCP_EXTEND_DSN(mp_tp->mpt_snduna, dack, full_dack);

		NTOHL(dss_ack_rsp->mdss_dsn);
		NTOHL(dss_ack_rsp->mdss_subflow_seqn);
		NTOHS(dss_ack_rsp->mdss_data_len);
		MPTCP_EXTEND_DSN(mp_tp->mpt_rcvnxt, dss_ack_rsp->mdss_dsn, full_dsn);

		mptcp_do_dss_opt_ack_meat(full_dack, full_dsn, tp, tiwin);

		if (csum_len != 0) {
			csum = *(uint16_t *)(void *)(cp + (dss_ack_rsp->mdss_copt.mdss_len - csum_len));
		}

		mptcp_update_rcv_state_meat(mp_tp, tp,
		    full_dsn,
		    dss_ack_rsp->mdss_subflow_seqn,
		    dss_ack_rsp->mdss_data_len,
		    csum);
		break;
	}
	case (MDSS_M | MDSS_m):
	{
		/* 64-bit DSS , No Data ACK */
		struct mptcp_dsn64_opt dsn64_s, *dsn64;
		dsn64 = MPTCP_OPT_GET(dsn64_s, cp, optend, optlen);
		u_int64_t full_dsn;
		uint16_t csum = 0;

		MPTCP_DSS_OPT_SZ_CHK(dsn64->mdss_copt.mdss_len,
		    sizeof(struct mptcp_dsn64_opt) + csum_len);

		mp_tp->mpt_flags |= MPTCPF_SND_64BITACK;

		full_dsn = mptcp_ntoh64(dsn64->mdss_dsn);
		NTOHL(dsn64->mdss_subflow_seqn);
		NTOHS(dsn64->mdss_data_len);

		if (csum_len != 0) {
			csum = *(uint16_t *)(void *)(cp + dsn64->mdss_copt.mdss_len - csum_len);
		}

		mptcp_update_rcv_state_meat(mp_tp, tp, full_dsn,
		    dsn64->mdss_subflow_seqn,
		    dsn64->mdss_data_len,
		    csum);
		break;
	}
	case (MDSS_A | MDSS_a):
	{
		/* 64-bit Data ACK, no DSS */
		struct mptcp_data_ack64_opt dack64_s, *dack64;
		dack64 = MPTCP_OPT_GET(dack64_s, cp, optend, optlen);

		MPTCP_DSS_OPT_SZ_CHK(dack64->mdss_copt.mdss_len,
		    sizeof(struct mptcp_data_ack64_opt));

		mp_tp->mpt_flags |= MPTCPF_RCVD_64BITACK;

		full_dack = mptcp_ntoh64(dack64->mdss_ack);
		mptcp_do_dss_opt_ack_meat(full_dack, mp_tp->mpt_sndwl1, tp, tiwin);
		break;
	}
	case (MDSS_M | MDSS_m | MDSS_A):
	{
		/* 64-bit DSS + 32-bit Data ACK */
		struct mptcp_dss64_ack32_opt dss_ack_rsp_s, *dss_ack_rsp;
		dss_ack_rsp = MPTCP_OPT_GET(dss_ack_rsp_s, cp, optend, optlen);
		u_int64_t full_dsn;
		uint16_t csum = 0;

		MPTCP_DSS_OPT_SZ_CHK(dss_ack_rsp->mdss_copt.mdss_len,
		    sizeof(struct mptcp_dss64_ack32_opt) + csum_len);

		u_int32_t dack = dss_ack_rsp->mdss_ack;
		NTOHL(dack);
		mp_tp->mpt_flags |= MPTCPF_SND_64BITACK;
		MPTCP_EXTEND_DSN(mp_tp->mpt_snduna, dack, full_dack);

		full_dsn = mptcp_ntoh64(dss_ack_rsp->mdss_dsn);
		NTOHL(dss_ack_rsp->mdss_subflow_seqn);
		NTOHS(dss_ack_rsp->mdss_data_len);

		mptcp_do_dss_opt_ack_meat(full_dack, full_dsn, tp, tiwin);

		if (csum_len != 0) {
			csum = *(uint16_t *)(void *)(cp + dss_ack_rsp->mdss_copt.mdss_len - csum_len);
		}

		mptcp_update_rcv_state_meat(mp_tp, tp, full_dsn,
		    dss_ack_rsp->mdss_subflow_seqn,
		    dss_ack_rsp->mdss_data_len,
		    csum);

		break;
	}
	case (MDSS_M | MDSS_A | MDSS_a):
	{
		/* 32-bit DSS + 64-bit Data ACK */
		struct mptcp_dss32_ack64_opt dss32_ack_64_opt_s, *dss32_ack64_opt;
		dss32_ack64_opt = MPTCP_OPT_GET(dss32_ack_64_opt_s, cp, optend, optlen);
		u_int64_t full_dsn;

		MPTCP_DSS_OPT_SZ_CHK(
			dss32_ack64_opt->mdss_copt.mdss_len,
			sizeof(struct mptcp_dss32_ack64_opt) + csum_len);

		full_dack = mptcp_ntoh64(dss32_ack64_opt->mdss_ack);
		NTOHL(dss32_ack64_opt->mdss_dsn);
		mp_tp->mpt_flags |= MPTCPF_RCVD_64BITACK;
		MPTCP_EXTEND_DSN(mp_tp->mpt_rcvnxt,
		    dss32_ack64_opt->mdss_dsn, full_dsn);
		NTOHL(dss32_ack64_opt->mdss_subflow_seqn);
		NTOHS(dss32_ack64_opt->mdss_data_len);

		mptcp_do_dss_opt_ack_meat(full_dack, full_dsn, tp, tiwin);
		if (csum_len == 0) {
			mptcp_update_rcv_state_meat(mp_tp, tp, full_dsn,
			    dss32_ack64_opt->mdss_subflow_seqn,
			    dss32_ack64_opt->mdss_data_len, 0);
		} else {
			mptcp_update_rcv_state_meat(mp_tp, tp, full_dsn,
			    dss32_ack64_opt->mdss_subflow_seqn,
			    dss32_ack64_opt->mdss_data_len,
			    *(uint16_t *)(void *)(cp +
			    dss32_ack64_opt->mdss_copt.mdss_len -
			    csum_len));
		}
		break;
	}
	case (MDSS_M | MDSS_m | MDSS_A | MDSS_a):
	{
		/* 64-bit DSS + 64-bit Data ACK */
		struct mptcp_dss64_ack64_opt dss64_ack_64_s, *dss64_ack64;
		dss64_ack64 = MPTCP_OPT_GET(dss64_ack_64_s, cp, optend, optlen);
		u_int64_t full_dsn;

		MPTCP_DSS_OPT_SZ_CHK(dss64_ack64->mdss_copt.mdss_len,
		    sizeof(struct mptcp_dss64_ack64_opt) + csum_len);

		mp_tp->mpt_flags |= MPTCPF_RCVD_64BITACK;
		mp_tp->mpt_flags |= MPTCPF_SND_64BITACK;
		full_dsn = mptcp_ntoh64(dss64_ack64->mdss_dsn);
		full_dack = mptcp_ntoh64(dss64_ack64->mdss_dsn);
		mptcp_do_dss_opt_ack_meat(full_dack, full_dsn, tp, tiwin);
		NTOHL(dss64_ack64->mdss_subflow_seqn);
		NTOHS(dss64_ack64->mdss_data_len);
		if (csum_len == 0) {
			mptcp_update_rcv_state_meat(mp_tp, tp, full_dsn,
			    dss64_ack64->mdss_subflow_seqn,
			    dss64_ack64->mdss_data_len, 0);
		} else {
			mptcp_update_rcv_state_meat(mp_tp, tp, full_dsn,
			    dss64_ack64->mdss_subflow_seqn,
			    dss64_ack64->mdss_data_len,
			    *(uint16_t *)(void *)(cp +
			    dss64_ack64->mdss_copt.mdss_len -
			    csum_len));
		}
		break;
	}
	default:
		break;
	}
}

static void
mptcp_do_dss_opt(struct tcpcb *tp, u_char *cp __ended_by(optend), u_char *optend, struct tcphdr *th, uint8_t optlen)
{
	struct mptcp_dss_copt dss_rsp_s, *dss_rsp;
	dss_rsp = MPTCP_OPT_GET(dss_rsp_s, cp, optend, optlen);
	struct mptcb *mp_tp = tptomptp(tp);

	if (!mp_tp) {
		return;
	}

	if (dss_rsp->mdss_subtype == MPO_DSS) {
		if (dss_rsp->mdss_flags & MDSS_F) {
			tp->t_rcv_map.mpt_dfin = 1;
		} else {
			tp->t_rcv_map.mpt_dfin = 0;
		}

		mptcp_do_dss_opt_meat(cp, optend, tp, th, optlen);
	}
}

static void
mptcp_do_fastclose_opt(struct tcpcb *tp, u_char *cp __ended_by(optend), u_char *optend __unused, struct tcphdr *th, uint8_t optlen)
{
	struct mptcb *mp_tp = NULL;
	struct mptcp_fastclose_opt fc_opt_s, *fc_opt;
	fc_opt = MPTCP_OPT_GET(fc_opt_s, cp, optend, optlen);

	if (th->th_flags != TH_ACK) {
		return;
	}

	if (fc_opt->mfast_len != sizeof(struct mptcp_fastclose_opt)) {
		tcpstat.tcps_invalid_opt++;
		return;
	}

	mp_tp = tptomptp(tp);
	if (!mp_tp) {
		return;
	}

	if (fc_opt->mfast_key != mp_tp->mpt_localkey) {
		tcpstat.tcps_invalid_opt++;
		return;
	}

	/*
	 * fastclose could make us more vulnerable to attacks, hence
	 * accept only those that are at the next expected sequence number.
	 */
	if (th->th_seq != tp->rcv_nxt) {
		tcpstat.tcps_invalid_opt++;
		return;
	}

	/* Reset this flow */
	tp->t_mpflags |= TMPF_FASTCLOSERCV;

	if (tp->t_inpcb->inp_socket != NULL) {
		soevent(tp->t_inpcb->inp_socket,
		    SO_FILT_HINT_LOCKED | SO_FILT_HINT_MUSTRST);
	}
}


static void
mptcp_do_mpfail_opt(struct tcpcb *tp, u_char *cp __ended_by(optend), u_char *optend __unused, struct tcphdr *th, uint8_t optlen)
{
	struct mptcp_mpfail_opt fail_opt_s, *fail_opt;
	fail_opt = MPTCP_OPT_GET(fail_opt_s, cp, optend, optlen);
	u_int32_t mdss_subflow_seqn = 0;
	struct mptcb *mp_tp;
	int error = 0;

	/*
	 * mpfail could make us more vulnerable to attacks. Hence accept
	 * only those that are the next expected sequence number.
	 */
	if (th->th_seq != tp->rcv_nxt) {
		tcpstat.tcps_invalid_opt++;
		return;
	}

	/* A packet without RST, must atleast have the ACK bit set */
	if ((th->th_flags != TH_ACK) && (th->th_flags != TH_RST)) {
		return;
	}

	if (fail_opt->mfail_len != sizeof(struct mptcp_mpfail_opt)) {
		return;
	}

	mp_tp = tptomptp(tp);

	mp_tp->mpt_flags |= MPTCPF_RECVD_MPFAIL;
	mp_tp->mpt_dsn_at_csum_fail = mptcp_hton64(fail_opt->mfail_dsn);
	error = mptcp_get_map_for_dsn(tp->t_inpcb->inp_socket,
	    mp_tp->mpt_dsn_at_csum_fail, &mdss_subflow_seqn);
	if (error == 0) {
		mp_tp->mpt_ssn_at_csum_fail = mdss_subflow_seqn;
	}

	mptcp_notify_mpfail(tp->t_inpcb->inp_socket);
}

static boolean_t
mptcp_validate_add_addr_hmac(struct tcpcb *tp, u_char *hmac __sized_by(mac_len),
    u_char *msg __sized_by(msg_len), uint16_t msg_len, uint16_t mac_len)
{
	u_char digest[SHA256_DIGEST_LENGTH] = {0};
	struct mptcb *mp_tp = tptomptp(tp);

	VERIFY(mac_len <= SHA256_DIGEST_LENGTH);
	mptcp_hmac_sha256(mp_tp->mpt_remotekey, mp_tp->mpt_localkey, msg, msg_len, digest);

	if (bcmp(digest + SHA256_DIGEST_LENGTH - mac_len, hmac, mac_len) == 0) {
		return true; /* matches */
	} else {
		return false;
	}
}

static void
mptcp_do_add_addr_opt_v1(struct tcpcb *tp, u_char *cp __ended_by(optend), u_char *optend, uint8_t optlen)
{
	struct mptcb *mp_tp = tptomptp(tp);
	struct mptses *mpte = mp_tp->mpt_mpte;

	struct mptcp_add_addr_opt addr_opt_s, *addr_opt;
	addr_opt = MPTCP_OPT_GET(addr_opt_s, cp, optend, optlen);

	if (addr_opt->maddr_len != MPTCP_V1_ADD_ADDR_OPT_LEN_V4 &&
	    addr_opt->maddr_len != MPTCP_V1_ADD_ADDR_OPT_LEN_V4 + 2 &&
	    addr_opt->maddr_len != MPTCP_V1_ADD_ADDR_OPT_LEN_V6 &&
	    addr_opt->maddr_len != MPTCP_V1_ADD_ADDR_OPT_LEN_V6 + 2) {
		os_log_error(mptcp_log_handle, "%s - %lx: Wrong ADD_ADDR length %u\n",
		    __func__, (unsigned long)VM_KERNEL_ADDRPERM(mpte),
		    addr_opt->maddr_len);

		return;
	}

	if ((addr_opt->maddr_flags & MPTCP_V1_ADD_ADDR_ECHO) != 0) {
		os_log(mptcp_log_handle, "%s - %lx: Received ADD_ADDR with echo bit\n",
		    __func__, (unsigned long)VM_KERNEL_ADDRPERM(mpte));

		return;
	}

	if (addr_opt->maddr_len < MPTCP_V1_ADD_ADDR_OPT_LEN_V6) {
		struct sockaddr_in *dst = &mpte->mpte_sub_dst_v4;
		struct in_addr *addr = &addr_opt->maddr_u.maddr_addrv4;
		in_addr_t haddr = ntohl(addr->s_addr);

		if (IN_ZERONET(haddr) ||
		    IN_LOOPBACK(haddr) ||
		    IN_LINKLOCAL(haddr) ||
		    IN_DS_LITE(haddr) ||
		    IN_6TO4_RELAY_ANYCAST(haddr) ||
		    IN_MULTICAST(haddr) ||
		    INADDR_BROADCAST == haddr ||
		    IN_PRIVATE(haddr) ||
		    IN_SHARED_ADDRESS_SPACE(haddr)) {
			os_log_error(mptcp_log_handle, "%s - %lx: ADD_ADDR invalid addr: %x\n",
			    __func__, (unsigned long)VM_KERNEL_ADDRPERM(mpte),
			    addr->s_addr);

			return;
		}

		u_char *hmac = (void *)(cp + addr_opt->maddr_len - HMAC_TRUNCATED_ADD_ADDR);
		uint16_t msg_len = sizeof(struct mptcp_add_addr_hmac_msg_v4);
		struct mptcp_add_addr_hmac_msg_v4 msg  = {0};
		msg.maddr_addrid = addr_opt->maddr_addrid;
		msg.maddr_addr = addr_opt->maddr_u.maddr_addrv4;
		if (addr_opt->maddr_len > MPTCP_V1_ADD_ADDR_OPT_LEN_V4) {
			msg.maddr_port = *(uint16_t *)(void *)(cp + addr_opt->maddr_len - HMAC_TRUNCATED_ADD_ADDR - 2);
		}
		if (!mptcp_validate_add_addr_hmac(tp, hmac, (u_char *)&msg, msg_len, HMAC_TRUNCATED_ADD_ADDR)) {
			os_log_error(mptcp_log_handle, "%s - %lx: ADD_ADDR addr: %x invalid HMAC\n",
			    __func__, (unsigned long)VM_KERNEL_ADDRPERM(mpte),
			    addr->s_addr);
			return;
		}

		dst->sin_len = sizeof(*dst);
		dst->sin_family = AF_INET;
		if (addr_opt->maddr_len > MPTCP_V1_ADD_ADDR_OPT_LEN_V4) {
			dst->sin_port = *(uint16_t *)(void *)(cp + addr_opt->maddr_len - HMAC_TRUNCATED_ADD_ADDR - 2);
		} else {
			dst->sin_port = mpte->__mpte_dst_v4.sin_port;
		}
		dst->sin_addr.s_addr = addr->s_addr;
		mpte->sub_dst_addr_id_v4 = addr_opt->maddr_addrid;
		mpte->mpte_last_added_addr_is_v4 = TRUE;
	} else {
		struct sockaddr_in6 *dst = &mpte->mpte_sub_dst_v6;
		struct in6_addr *addr = &addr_opt->maddr_u.maddr_addrv6;

		if (IN6_IS_ADDR_LINKLOCAL(addr) ||
		    IN6_IS_ADDR_MULTICAST(addr) ||
		    IN6_IS_ADDR_UNSPECIFIED(addr) ||
		    IN6_IS_ADDR_LOOPBACK(addr) ||
		    IN6_IS_ADDR_V4COMPAT(addr) ||
		    IN6_IS_ADDR_V4MAPPED(addr)) {
			char dbuf[MAX_IPv6_STR_LEN];

			inet_ntop(AF_INET6, addr, dbuf, sizeof(dbuf));
			os_log_error(mptcp_log_handle, "%s - %lx: ADD_ADDRv6 invalid addr: %s\n",
			    __func__, (unsigned long)VM_KERNEL_ADDRPERM(mpte),
			    dbuf);

			return;
		}

		u_char *hmac = (void *)(cp + addr_opt->maddr_len - HMAC_TRUNCATED_ADD_ADDR);
		uint16_t msg_len = sizeof(struct mptcp_add_addr_hmac_msg_v6);
		struct mptcp_add_addr_hmac_msg_v6 msg  = {0};
		msg.maddr_addrid = addr_opt->maddr_addrid;
		msg.maddr_addr = addr_opt->maddr_u.maddr_addrv6;
		if (addr_opt->maddr_len > MPTCP_V1_ADD_ADDR_OPT_LEN_V6) {
			msg.maddr_port = *(uint16_t *)(void *)(cp + addr_opt->maddr_len - HMAC_TRUNCATED_ADD_ADDR - 2);
		}
		if (!mptcp_validate_add_addr_hmac(tp, hmac, (u_char *)&msg, msg_len, HMAC_TRUNCATED_ADD_ADDR)) {
			char dbuf[MAX_IPv6_STR_LEN];

			inet_ntop(AF_INET6, addr, dbuf, sizeof(dbuf));
			os_log_error(mptcp_log_handle, "%s - %lx: ADD_ADDR addr: %s invalid HMAC\n",
			    __func__, (unsigned long)VM_KERNEL_ADDRPERM(mpte),
			    dbuf);
			return;
		}

		dst->sin6_len = sizeof(*dst);
		dst->sin6_family = AF_INET6;
		if (addr_opt->maddr_len > MPTCP_V1_ADD_ADDR_OPT_LEN_V6) {
			dst->sin6_port = *(uint16_t *)(void *)(cp + addr_opt->maddr_len - HMAC_TRUNCATED_ADD_ADDR - 2);
		} else {
			dst->sin6_port = mpte->__mpte_dst_v6.sin6_port;
		}
		memcpy(&dst->sin6_addr, addr, sizeof(*addr));
		mpte->sub_dst_addr_id_v6 = addr_opt->maddr_addrid;
		mpte->mpte_last_added_addr_is_v4 = FALSE;
	}

	os_log(mptcp_log_handle, "%s - %lx: Received ADD_ADDRv1\n",
	    __func__, (unsigned long)VM_KERNEL_ADDRPERM(mpte));

	/* Once an incoming ADD_ADDR for v1 is valid, it means that the peer
	 * receiver our keys.
	 */
	tp->t_mpflags &= ~TMPF_SND_KEYS;
	tp->t_mpflags |= TMPF_MPTCP_ECHO_ADDR;
	tp->t_flags |= TF_ACKNOW;
	mptcp_sched_create_subflows(mpte);
}

static void
mptcp_do_add_addr_opt_v0(struct mptses *mpte, u_char *cp __ended_by(optend), u_char *optend __unused, uint8_t optlen)
{
	struct mptcp_add_addr_opt addr_opt_s, *addr_opt;
	addr_opt = MPTCP_OPT_GET(addr_opt_s, cp, optend, optlen);

	if (addr_opt->maddr_len != MPTCP_V0_ADD_ADDR_OPT_LEN_V4 &&
	    addr_opt->maddr_len != MPTCP_V0_ADD_ADDR_OPT_LEN_V6) {
		os_log_error(mptcp_log_handle, "%s - %lx: Wrong ADD_ADDR length %u\n",
		    __func__, (unsigned long)VM_KERNEL_ADDRPERM(mpte),
		    addr_opt->maddr_len);

		return;
	}

	if (addr_opt->maddr_len == MPTCP_V0_ADD_ADDR_OPT_LEN_V4 &&
	    addr_opt->maddr_flags != MPTCP_V0_ADD_ADDR_IPV4) {
		os_log_error(mptcp_log_handle, "%s - %lx: ADD_ADDR length for v4 but version is %u\n",
		    __func__, (unsigned long)VM_KERNEL_ADDRPERM(mpte),
		    addr_opt->maddr_flags);

		return;
	}

	if (addr_opt->maddr_len == MPTCP_V0_ADD_ADDR_OPT_LEN_V6 &&
	    addr_opt->maddr_flags != MPTCP_V0_ADD_ADDR_IPV6) {
		os_log_error(mptcp_log_handle, "%s - %lx: ADD_ADDR length for v6 but version is %u\n",
		    __func__, (unsigned long)VM_KERNEL_ADDRPERM(mpte),
		    addr_opt->maddr_flags);

		return;
	}

	if (addr_opt->maddr_len == MPTCP_V0_ADD_ADDR_OPT_LEN_V4) {
		struct sockaddr_in *dst = &mpte->mpte_sub_dst_v4;
		struct in_addr *addr = &addr_opt->maddr_u.maddr_addrv4;
		in_addr_t haddr = ntohl(addr->s_addr);

		if (IN_ZERONET(haddr) ||
		    IN_LOOPBACK(haddr) ||
		    IN_LINKLOCAL(haddr) ||
		    IN_DS_LITE(haddr) ||
		    IN_6TO4_RELAY_ANYCAST(haddr) ||
		    IN_MULTICAST(haddr) ||
		    INADDR_BROADCAST == haddr ||
		    IN_PRIVATE(haddr) ||
		    IN_SHARED_ADDRESS_SPACE(haddr)) {
			os_log_error(mptcp_log_handle, "%s - %lx: ADD_ADDR invalid addr: %x\n",
			    __func__, (unsigned long)VM_KERNEL_ADDRPERM(mpte),
			    addr->s_addr);

			return;
		}

		dst->sin_len = sizeof(*dst);
		dst->sin_family = AF_INET;
		dst->sin_port = mpte->__mpte_dst_v4.sin_port;
		dst->sin_addr.s_addr = addr->s_addr;
		mpte->mpte_last_added_addr_is_v4 = TRUE;
	} else {
		struct sockaddr_in6 *dst = &mpte->mpte_sub_dst_v6;
		struct in6_addr *addr = &addr_opt->maddr_u.maddr_addrv6;

		if (IN6_IS_ADDR_LINKLOCAL(addr) ||
		    IN6_IS_ADDR_MULTICAST(addr) ||
		    IN6_IS_ADDR_UNSPECIFIED(addr) ||
		    IN6_IS_ADDR_LOOPBACK(addr) ||
		    IN6_IS_ADDR_V4COMPAT(addr) ||
		    IN6_IS_ADDR_V4MAPPED(addr)) {
			char dbuf[MAX_IPv6_STR_LEN];

			inet_ntop(AF_INET6, addr, dbuf, sizeof(dbuf));
			os_log_error(mptcp_log_handle, "%s - %lx: ADD_ADDRv6 invalid addr: %s\n",
			    __func__, (unsigned long)VM_KERNEL_ADDRPERM(mpte),
			    dbuf);

			return;
		}

		dst->sin6_len = sizeof(*dst);
		dst->sin6_family = AF_INET6;
		dst->sin6_port = mpte->__mpte_dst_v6.sin6_port;
		dst->sin6_addr = *addr;
		mpte->mpte_last_added_addr_is_v4 = FALSE;
	}

	os_log(mptcp_log_handle, "%s - %lx: Received ADD_ADDRv0\n",
	    __func__, (unsigned long)VM_KERNEL_ADDRPERM(mpte));

	mptcp_sched_create_subflows(mpte);
}

void
tcp_do_mptcp_options(struct tcpcb *tp, u_char *cp __ended_by(optend), u_char *optend, struct tcphdr *th,
    struct tcpopt *to, uint8_t optlen)
{
	int mptcp_subtype = 0;
	struct mptcb *mp_tp = tptomptp(tp);

	/* We expect the TCP stack to ensure this */
	ASSERT(cp + optlen <= optend);

	if (mp_tp == NULL) {
		return;
	}

	socket_lock_assert_owned(mptetoso(mp_tp->mpt_mpte));

	/* All MPTCP options have atleast 4 bytes */
	if (optlen < 4) {
		return;
	}

	mptcp_subtype = (cp[2] >> 4);

	if (mptcp_sanitize_option(tp, mptcp_subtype) == 0) {
		return;
	}

	switch (mptcp_subtype) {
	case MPO_CAPABLE:
		mptcp_do_mpcapable_opt(tp, cp, optend, th, optlen);
		break;
	case MPO_JOIN:
		mptcp_do_mpjoin_opt(tp, cp, optend, th, optlen);
		break;
	case MPO_DSS:
		mptcp_do_dss_opt(tp, cp, optend, th, optlen);
		break;
	case MPO_FASTCLOSE:
		mptcp_do_fastclose_opt(tp, cp, optend, th, optlen);
		break;
	case MPO_FAIL:
		mptcp_do_mpfail_opt(tp, cp, optend, th, optlen);
		break;
	case MPO_ADD_ADDR:
		if (mp_tp->mpt_version == MPTCP_VERSION_0) {
			mptcp_do_add_addr_opt_v0(mp_tp->mpt_mpte, cp, optend, optlen);
		} else {
			mptcp_do_add_addr_opt_v1(tp, cp, optend, optlen);
		}
		break;
	case MPO_REMOVE_ADDR:           /* fall through */
	case MPO_PRIO:
		to->to_flags |= TOF_MPTCP;
		break;
	default:
		break;
	}
	return;
}

/* REMOVE_ADDR option is sent when a source address goes away */
static void
mptcp_send_remaddr_opt(struct tcpcb *tp, struct mptcp_remaddr_opt *opt)
{
	bzero(opt, sizeof(*opt));
	opt->mr_kind = TCPOPT_MULTIPATH;
	opt->mr_len = sizeof(*opt);
	opt->mr_subtype = MPO_REMOVE_ADDR;
	opt->mr_addr_id = tp->t_rem_aid;
	tp->t_mpflags &= ~TMPF_SND_REM_ADDR;
}

static int
mptcp_echo_add_addr(struct tcpcb *tp, u_char *cp __ended_by(optend), u_char *optend __unused, unsigned int optlen)
{
	struct mptcp_add_addr_opt mpaddr;
	struct mptcb *mp_tp = tptomptp(tp);
	struct mptses *mpte = mp_tp->mpt_mpte;

	// MPTCP v0 doesn't require echoing add_addr
	if (mp_tp->mpt_version == MPTCP_VERSION_0) {
		return optlen;
	}

	size_t mpaddr_size = mpte->mpte_last_added_addr_is_v4 ? MPTCP_V1_ADD_ADDR_ECHO_OPT_LEN_V4 : MPTCP_V1_ADD_ADDR_ECHO_OPT_LEN_V6;
	if ((MAX_TCPOPTLEN - optlen) < mpaddr_size) {
		return optlen;
	}

	bzero(&mpaddr, sizeof(mpaddr));
	mpaddr.maddr_kind = TCPOPT_MULTIPATH;
	mpaddr.maddr_len = (uint8_t)mpaddr_size;
	mpaddr.maddr_subtype = MPO_ADD_ADDR;
	mpaddr.maddr_flags = MPTCP_V1_ADD_ADDR_ECHO;
	if (mpte->mpte_last_added_addr_is_v4) {
		mpaddr.maddr_u.maddr_addrv4.s_addr = mpte->mpte_sub_dst_v4.sin_addr.s_addr;
		mpaddr.maddr_addrid = mpte->sub_dst_addr_id_v4;
	} else {
		mpaddr.maddr_u.maddr_addrv6 = mpte->mpte_sub_dst_v6.sin6_addr;
		mpaddr.maddr_addrid = mpte->sub_dst_addr_id_v6;
	}

	memcpy(cp + optlen, &mpaddr, mpaddr_size);
	optlen += mpaddr_size;
	tp->t_mpflags &= ~TMPF_MPTCP_ECHO_ADDR;
	return optlen;
}

/* We send MP_PRIO option based on the values set by the SIOCSCONNORDER ioctl */
static int
mptcp_snd_mpprio(struct tcpcb *tp, u_char *cp __ended_by(optend), u_char *optend __unused, int optlen)
{
	struct mptcp_mpprio_addr_opt mpprio;
	struct mptcb *mp_tp = tptomptp(tp);
	size_t mpprio_size = sizeof(mpprio);
	// MP_PRIO of MPTCPv1 doesn't include AddrID
	if (mp_tp->mpt_version == MPTCP_VERSION_1) {
		mpprio_size -= sizeof(uint8_t);
	}

	if (tp->t_state != TCPS_ESTABLISHED) {
		tp->t_mpflags &= ~TMPF_SND_MPPRIO;
		return optlen;
	}

	if ((MAX_TCPOPTLEN - optlen) < (int)mpprio_size) {
		return optlen;
	}

	bzero(&mpprio, sizeof(mpprio));
	mpprio.mpprio_kind = TCPOPT_MULTIPATH;
	mpprio.mpprio_len = (uint8_t)mpprio_size;
	mpprio.mpprio_subtype = MPO_PRIO;
	if (tp->t_mpflags & TMPF_BACKUP_PATH) {
		mpprio.mpprio_flags |= MPTCP_MPPRIO_BKP;
	}
	mpprio.mpprio_addrid = tp->t_local_aid;
	memcpy(cp + optlen, &mpprio, mpprio_size);
	optlen += mpprio_size;
	tp->t_mpflags &= ~TMPF_SND_MPPRIO;
	return optlen;
}
