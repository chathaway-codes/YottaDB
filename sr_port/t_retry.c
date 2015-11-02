/****************************************************************
 *								*
 *	Copyright 2001, 2002  Fidelity Information Services, Inc	*
 *								*
 *	This source code contains the intellectual property	*
 *	of its copyright holder(s), and is made available	*
 *	under a license.  If you do not know the terms of	*
 *	the license, please stop and do not read further.	*
 *								*
 ****************************************************************/

#include "mdef.h"

#include "gdsroot.h"
#include "gtm_facility.h"
#include "fileinfo.h"
#include "cdb_sc.h"
#include "gdsbt.h"
#include "gdsfhead.h"
#include "gdskill.h"
#include "gdscc.h"
#include "gdsbml.h"
#include "ccp.h"
#include "error.h"
#include "filestruct.h"
#include "jnl.h"
#include "buddy_list.h"		/* needed for tp.h */
#include "hashtab_int4.h"	/* needed for tp.h and cws_insert.h */
#include "tp.h"
#include "tp_frame.h"
#include "sleep_cnt.h"
#include "t_retry.h"
#include "format_targ_key.h"
#include "send_msg.h"
#include "longset.h"		/* needed for cws_insert.h */
#include "cws_insert.h"
#include "wcs_mm_recover.h"
#include "wcs_sleep.h"
#include "have_crit.h"
#include "gdsbgtr.h"		/* for the BG_TRACE_PRO macros */
#include "wcs_backoff.h"
#include "tp_restart.h"
#include "gtm_ctype.h"		/* for ISALPHA_ASCII */

GBLREF	sgmnt_addrs		*cs_addrs;
GBLREF	sgmnt_data_ptr_t	cs_data;
GBLREF	short			crash_count, dollar_tlevel;
GBLREF	gd_region		*gv_cur_region;
GBLREF	gv_key			*gv_currkey;
GBLREF	gv_namehead		*gv_target;
GBLREF	tp_frame		*tp_pointer;
GBLREF	trans_num		start_tn;
GBLREF	unsigned char		cw_set_depth, cw_map_depth, t_fail_hist[CDB_MAX_TRIES];
GBLREF	boolean_t		mu_reorg_process;
GBLREF	unsigned int		t_tries;
GBLREF	uint4			t_err;
GBLREF	jnl_gbls_t		jgbl;
GBLREF	boolean_t		is_dollar_incr;
GBLREF	uint4			update_trans;
#ifdef GTM_TRIGGER
GBLREF	boolean_t		skip_INVOKE_RESTART;
#endif
#ifdef DEBUG
GBLREF	boolean_t		ok_to_call_wcs_recover;	/* see comment in gbldefs.c for purpose */
GBLREF	uint4			donot_commit;	/* see gdsfhead.h for the purpose of this debug-only global */
#endif

void t_retry(enum cdb_sc failure)
{
	tp_frame		*tf;
	unsigned char		*end, buff[MAX_ZWR_KEY_SZ];
	short			tl;
	sgmnt_addrs		*csa;
	sgmnt_data_ptr_t	csd;
	boolean_t		skip_invoke_restart;

	error_def(ERR_GBLOFLOW);
	error_def(ERR_GVIS);
	error_def(ERR_TPRETRY);
	error_def(ERR_GVPUTFAIL);
	error_def(ERR_GVINCRFAIL);
	UNIX_ONLY(error_def(ERR_GVFAILCORE);)

#	ifdef GTM_TRIGGER
	skip_invoke_restart = skip_INVOKE_RESTART;	/* note down global value in local variable */
	skip_INVOKE_RESTART = FALSE;	/* reset global variable to default state as soon as possible */
#	else
	skip_invoke_restart = FALSE;	/* no triggers so set local variable to default state */
#	endif
	/* We expect t_retry to be invoked with an abnormal failure code. mupip reorg is the only exception and can pass
	 * cdb_sc_normal failure code in case it finds a global variable existed at start of reorg, but not when it came
	 * into mu_reorg and did a gvcst_search. It cannot confirm this unless it holds crit for which it has to wait
	 * until the final retry which is why we accept this way of invoking t_retry. Assert accordingly.
	 */
	assert((cdb_sc_normal != failure) || mu_reorg_process);
	t_fail_hist[t_tries] = (unsigned char)failure;
	if (mu_reorg_process)
		CWS_RESET;
	DEBUG_ONLY(donot_commit = FALSE;)
	csa = cs_addrs;
	if (0 == dollar_tlevel)
	{
		SET_WC_BLOCKED_FINAL_RETRY_IF_NEEDED(csa, failure);	/* set wc_blocked if cache related status */
		switch(t_tries)
		{
			case 0:
				INCR_GVSTATS_COUNTER(csa, csa->nl, n_nontp_retries_0, 1);
				break;
			case 1:
				INCR_GVSTATS_COUNTER(csa, csa->nl, n_nontp_retries_1, 1);
				break;
			case 2:
				INCR_GVSTATS_COUNTER(csa, csa->nl, n_nontp_retries_2, 1);
				break;
			default:
				assert(3 == t_tries);
				INCR_GVSTATS_COUNTER(csa, csa->nl, n_nontp_retries_3, 1);
				break;
		}
		if (csa->critical)
			crash_count = csa->critical->crashcnt;
		/* If the restart code is something that should not increment t_tries, handle that by decrementing t_tries
		 * for these special codes just before incrementing it unconditionally. Note that this should be done ONLY IF
		 * t_tries is CDB_STAGNATE or higher and not for lower values as otherwise it can cause livelocks (e.g.
		 * because csa->hdr->wc_blocked is set to TRUE, it is possible we end up restarting with cdb_sc_helpedout
		 * without even doing a cache-recovery (due to the fast path in t_end that does not invoke grab_crit in case
		 * of read-only transactions). In this case, not incrementing t_tries will cause us to eternally retry
		 * the transaction with no one eventually grabbing crit and doing the cache-recovery).
		 */
		assert(CDB_STAGNATE >= t_tries);
		if (CDB_STAGNATE <= t_tries)
		{
			/* Both backup and snapshot state change cannot happen when we are in final retry as they need crit
			 * which is held by us in the final retry.
			 */
			assert(cdb_sc_bkupss_statemod != failure);
			if ((cdb_sc_jnlstatemod == failure) || (cdb_sc_jnlclose == failure) || (cdb_sc_future_read == failure)
				|| (cdb_sc_helpedout == failure))
				t_tries = CDB_STAGNATE - 1;
		}
		if (CDB_STAGNATE <= ++t_tries)
		{
			DEBUG_ONLY(ok_to_call_wcs_recover = TRUE;)
			if (!csa->hold_onto_crit)
				grab_crit(gv_cur_region);
			assert(csa->now_crit);
			CHECK_MM_DBFILEXT_REMAP_IF_NEEDED(csa, gv_cur_region);
			DEBUG_ONLY(ok_to_call_wcs_recover = FALSE;)
			csd = cs_data;
			if (CDB_STAGNATE == t_tries)
			{
				if (csd->freeze && update_trans)
				{	/* Final retry on an update transaction and region is frozen.
					 * Wait for it to be unfrozen and only then grab crit.
					 */
					GRAB_UNFROZEN_CRIT(gv_cur_region, csa, csd);
				}
			} else
			{
				assert((failure != cdb_sc_helpedout) && (failure != cdb_sc_future_read)
					&& (failure != cdb_sc_jnlclose) && (failure != cdb_sc_jnlstatemod)
					&& (failure != cdb_sc_bkupss_statemod) && (failure != cdb_sc_inhibitkills));
				assert(csa->now_crit);
				if (!csa->hold_onto_crit)
					rel_crit(gv_cur_region);
				if (NULL == (end = format_targ_key(buff, MAX_ZWR_KEY_SZ, gv_currkey, TRUE)))
					end = &buff[MAX_ZWR_KEY_SZ - 1];
				if (cdb_sc_gbloflow == failure)
					rts_error(VARLSTCNT(6) ERR_GBLOFLOW, 0, ERR_GVIS, 2, end - buff, buff);
				if (IS_DOLLAR_INCREMENT)
				{
					assert(ERR_GVPUTFAIL == t_err);
					t_err = ERR_GVINCRFAIL;	/* print more specific error message */
				}
				UNIX_ONLY(send_msg(VARLSTCNT(9) t_err, 2, t_tries, t_fail_hist,
						   ERR_GVIS, 2, end-buff, buff, ERR_GVFAILCORE));
				UNIX_ONLY(gtm_fork_n_core());
				VMS_ONLY(send_msg(VARLSTCNT(8) t_err, 2, t_tries, t_fail_hist,
						   ERR_GVIS, 2, end-buff, buff));
				rts_error(VARLSTCNT(8) t_err, 2, t_tries, t_fail_hist, ERR_GVIS, 2, end-buff, buff);
			}
		}
		if ((cdb_sc_blockflush == failure) && !CCP_SEGMENT_STATE(csa->nl, CCST_MASK_HAVE_DIRTY_BUFFERS))
		{
			assert(csa->hdr->clustered);
			CCP_FID_MSG(gv_cur_region, CCTR_FLUSHLK);
			ccp_userwait(gv_cur_region, CCST_MASK_HAVE_DIRTY_BUFFERS, 0, csa->nl->ccp_cycle);
		}
		cw_set_depth = 0;
		cw_map_depth = 0;
		/* In case triggers are supported, make sure we start with latest copy of file header's db_trigger_cycle
		 * to avoid unnecessary cdb_sc_triggermod type of restarts.
		 */
		GTMTRIG_ONLY(csa->db_trigger_cycle = csa->hdr->db_trigger_cycle;)
		start_tn = csa->ti->curr_tn;
		assert(NULL != gv_target);
		if (NULL != gv_target)
			gv_target->clue.end = 0;
	} else
	{	/* for TP, do the minimum; most of the logic is in tp_retry, because it is also invoked directly from t_commit */
		assert(failure == t_fail_hist[t_tries]);
		assert((NULL == csa) || (NULL != csa->hdr));	/* both csa and csa->hdr should be NULL or non-NULL. */
		assert((NULL != csa) || (cdb_sc_needcrit == failure)); /* csa can be NULL in case of retry in op_lock2 */
		if (NULL != csa)					/*  in which case failure code should be cdb_sc_needcrit. */
		{
			SET_WC_BLOCKED_FINAL_RETRY_IF_NEEDED(csa, failure);
			TP_RETRY_ACCOUNTING(csa, csa->nl, failure);
		}
		assert((NULL != gv_target)
				|| (cdb_sc_needcrit == failure) && (CDB_STAGNATE <= t_tries) && have_crit(CRIT_HAVE_ANY_REG));
		/* only known case of gv_target being NULL is if a t_retry is done from gvcst_init. the above assert checks this */
		if (NULL != gv_target)
		{
			if (cdb_sc_blkmod != failure)
				TP_TRACE_HIST(CR_BLKEMPTY, gv_target);
			gv_target->clue.end = 0;
		}
		if (!skip_invoke_restart)
		{
			INVOKE_RESTART;
		} else	/* explicit trigger update caused implicit tp wrap so should return to caller without rts_error */
			tp_restart(1, !TP_RESTART_HANDLES_ERRORS);
	}
}
