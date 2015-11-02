/****************************************************************
 *								*
 *	Copyright 2001, 2010 Fidelity Information Services, Inc	*
 *								*
 *	This source code contains the intellectual property	*
 *	of its copyright holder(s), and is made available	*
 *	under a license.  If you do not know the terms of	*
 *	the license, please stop and do not read further.	*
 *								*
 ****************************************************************/

#include "mdef.h"

#include "gtm_string.h"

#ifdef VMS
#include <descrip.h>
#endif

#include "gdsroot.h"
#include "gdsblk.h"
#include "gdskill.h"
#include "gtm_facility.h"
#include "fileinfo.h"
#include "gdsbt.h"
#include "gdsfhead.h"
#include "filestruct.h"
#include "gdscc.h"
#include "cdb_sc.h"
#include "error.h"
#include "iosp.h"		/* for declaration of SS_NORMAL */
#include "jnl.h"
#include "rtnhdr.h"
#include "mv_stent.h"
#include "stack_frame.h"
#include "hashtab_int4.h"	/* needed for tp.h */
#include "buddy_list.h"		/* needed for tp.h */
#include "tp.h"
#include "tp_frame.h"
#include "gtm_stdio.h"
#include "gtm_stdlib.h"		/* for ATOI */
#include "send_msg.h"
#include "op.h"
#include "io.h"
#include "targ_alloc.h"
#include "getzposition.h"
#include "wcs_recover.h"
#include "tp_unwind.h"
#include "wcs_backoff.h"
#include "rel_quant.h"
#include "wcs_mm_recover.h"
#include "tp_restart.h"
#include "repl_msg.h"		/* for gtmsource.h */
#include "gtmsource.h"		/* for jnlpool_addrs structure definition */
#include "wbox_test_init.h"
#include "gtmimagename.h"
#ifdef GTM_TRIGGER
#include "gv_trigger.h"
#include "gtm_trigger.h"
#endif

error_def(ERR_TLVLZERO);
error_def(ERR_TPFAIL);
error_def(ERR_TPRESTART);
error_def(ERR_TRESTNOT);
error_def(ERR_TRESTLOC);
error_def(ERR_TPRETRY);
UNIX_ONLY(error_def(ERR_GVFAILCORE);)

#define	MAX_TRESTARTS		16
#define	FAIL_HIST_ARRAY_SIZE	32
#define	GVNAME_UNKNOWN		"*UNKNOWN"

static	int		num_tprestart = 0;
static	char		gvname_unknown[] = GVNAME_UNKNOWN;
static	int4		gvname_unknown_len = STR_LIT_LEN(GVNAME_UNKNOWN);

GBLDEF	int4		tprestart_syslog_limit;			/* limit TPRESTARTs */
GBLDEF	int4		tprestart_syslog_delta; 		/* limit TPRESTARTs */
GBLDEF	trans_num	tp_fail_histtn[CDB_MAX_TRIES], tp_fail_bttn[CDB_MAX_TRIES];
GBLDEF	int4		tp_fail_n, tp_fail_level;
GBLDEF	int4		n_pvtmods, n_blkmods;
GBLDEF	gv_namehead	*tp_fail_hist[CDB_MAX_TRIES];
GBLDEF	block_id	t_fail_hist_blk[CDB_MAX_TRIES];
GBLDEF	gd_region	*tp_fail_hist_reg[CDB_MAX_TRIES];

GBLREF	short			dollar_tlevel, dollar_trestart;
GBLREF	int			dollar_truth;
GBLREF	mval			dollar_zgbldir;
GBLREF	gd_addr			*gd_header;
GBLREF	gv_key			*gv_currkey;
GBLREF	gv_namehead		*gv_target;
GBLREF	stack_frame		*frame_pointer;
GBLREF	tp_frame		*tp_pointer;
GBLREF	sgm_info		*sgm_info_ptr;
GBLREF	tp_region		*tp_reg_list;	/* Chained list of regions used in this transaction not cleared on tp_restart */
GBLREF	mv_stent		*mv_chain;
GBLREF	unsigned char		*msp, *stackbase, *stacktop, t_fail_hist[CDB_MAX_TRIES];
GBLREF	sgm_info		*first_sgm_info;
GBLREF	unsigned int		t_tries;
GBLREF	int			process_id;
GBLREF	gd_region		*gv_cur_region;
GBLREF	jnlpool_addrs		jnlpool;
GBLREF	bool			caller_id_flag;
GBLREF	unsigned char		*tpstackbase, *tpstacktop;
GBLREF	trans_num		local_tn;	/* transaction number for THIS PROCESS */
GBLREF	sgmnt_addrs		*cs_addrs;
GBLREF	sgmnt_data_ptr_t	cs_data;
GBLREF	symval			*curr_symval;
GBLREF	boolean_t		hold_onto_locks;
#ifdef VMS
GBLREF	struct chf$signal_array	*tp_restart_fail_sig;
GBLREF	boolean_t	tp_restart_fail_sig_used;
#endif
#ifdef DEBUG
GBLREF	boolean_t	ok_to_call_wcs_recover;	/* see comment in gbldefs.c for purpose */
#endif
#ifdef GTM_TRIGGER
GBLREF	int		tprestart_state;	/* When triggers restart, multiple states possible. See tp_restart.h */
GBLREF	mval		dollar_ztwormhole;	/* Previous value (mval) restored on restart */
GBLREF	mval		dollar_ztslate;
LITREF	mval		literal_null;
#endif

STATICDEF mval 		bangHere;	/* Mval created early in tprestart processing needs to survive across tp_restart states */

CONDITION_HANDLER(tp_restart_ch)
{
	START_CH;
	/* On Unix, there is only one set of the signal info and this error will handily replace it. For VMS,
	   far more subterfuge is required. We will save the signal information and paramters and overlay the
	   TPRETRY signal information with it so that the signal will be handled properly. Note also that since
	   VMS does not do trigger, no special avoidance of the below needs to occur when we are dealing with
	   a trigger unwind initiated rethrow.
	*/
#ifdef VMS
	assert(TPRESTART_ARG_CNT >= sig->chf$is_sig_args);
	if (NULL == tp_restart_fail_sig)
		tp_restart_fail_sig = (struct chf$signal_array *)malloc((TPRESTART_ARG_CNT + 1) * SIZEOF(int));
	memcpy(tp_restart_fail_sig, sig, (sig->chf$is_sig_args + 1) * SIZEOF(int));
	assert(FALSE == tp_restart_fail_sig_used);
	tp_restart_fail_sig_used = TRUE;
#endif
	UNWIND(NULL, NULL);
}

/* Note that adding a new rts_error in "tp_restart" might need a change to the INVOKE_RESTART macro in tp.h and
 * TPRESTART_ARG_CNT in errorsp.h (sl_vvms). See comment in tp.h for INVOKE_RESTART macro for the details.
 */

int tp_restart(int newlevel, boolean_t handle_errors_internally)
{
	unsigned char		*cp;
	short			top;
	unsigned int		hist_index;
	tp_frame		*tf;
	mv_stent		*mvc;
	tp_region		*tr;
	mval			beganHere;
	sgmnt_addrs		*csa;
	int4			num_closed = 0;
	boolean_t		tp_tend_status;
	mstr			gvname_mstr, reg_mstr;
	gd_region		*restart_reg, *reg;
	int			tprestart_rc;
#	ifdef DEBUG
	static int4		uncounted_restarts;	/* do not count some failure codes towards MAX_TRESTARTS */
	static int4		t_fail_hist_index;
	static int4		t_fail_hist_array[FAIL_HIST_ARRAY_SIZE];
#	endif

	tprestart_rc = 0;
	/* Some callers of "tp_restart" want this function to return with an error code instead of issue an rts_error
	 * if there is an error inside tp_restart. The SIGNAL macro is supposed to reflect the error code in that
	 * case and the caller handles this error code accordingly after tp_restart returns.  For those callers,
	 * establish the "tp_restart_ch" condition handler to catch all errors. For the remaining callers, any errors
	 * inside tp_restart will invoke whatever parent condition handler is active at that point.
	 *
	 * The reason why a few callers prefer this inside-tprestart error handling is because they are already a
	 * condition/error handler (e.g. mdb_condition_handler) when they invoke tp_restart and do not want another
	 * rts_error to happen inside tp_restart and trigger any other condition/error handlers that can alter the
	 * flow of control elsewhere until this condition handler returns.
	 */
	if (handle_errors_internally)
	{	/* Currently, the only callers of tp_restart with handle_errors_internally set to TRUE are
		 * "mdb_condition_handler", "updproc.c" and "mupip_recover.c". All of those have SIGNAL set
		 * to ERR_TPRETRY so assert that. This is one way of protecting against a new caller of tp_restart
		 * inadvertently using a TRUE value for "handle_errors_internally". One reason for being paranoid
		 * about the TRUE usage is for example in gv_trigger.c, if tp_restart is incorrectly invoked with
		 * TRUE as the second paramter, it will result in indefinite number of cores on a broken database.
		 * In VMS, SIGNAL can be used only if we are inside a condition handler so do the assert only in Unix.
		 */
		UNIX_ONLY(assert(ERR_TPRETRY == SIGNAL);)
		ESTABLISH_RET(tp_restart_ch, tprestart_rc);
	}
	assert(1 == newlevel);
	if (0 == dollar_tlevel)
	{
		rts_error(VARLSTCNT(1) ERR_TLVLZERO);
		return 0; /* for the compiler only -- never executed */
	}
#	ifdef DEBUG
	if (uncounted_restarts >= dollar_trestart)
		uncounted_restarts = dollar_trestart;
#	endif

#	ifdef GTM_TRIGGER
	DBGTRIGR((stderr, "tp_restart: Entry state: %d\n", tprestart_state));
	if (TPRESTART_STATE_NORMAL == tprestart_state)
	{	/* Only do if a normal invocation - otherwise we've already done this code for this TP restart */
#	endif
		/* Increment restart counts for each region in this transaction */
		for (tr = tp_reg_list;  NULL != tr;  tr = tr->fPtr)
		{
			reg = tr->reg;
			if (reg->open)
			{
				csa = &FILE_INFO(reg)->s_addrs;
				switch (dollar_trestart)
				{
					case 0:
						INCR_GVSTATS_COUNTER(csa, csa->nl, n_tp_tot_retries_0, 1);
						break;
					case 1:
						INCR_GVSTATS_COUNTER(csa, csa->nl, n_tp_tot_retries_1, 1);
						break;
					case 2:
						INCR_GVSTATS_COUNTER(csa, csa->nl, n_tp_tot_retries_2, 1);
						break;
					case 3:
						INCR_GVSTATS_COUNTER(csa, csa->nl, n_tp_tot_retries_3, 1);
						break;
					default:
						INCR_GVSTATS_COUNTER(csa, csa->nl, n_tp_tot_retries_4, 1);
						break;
				}
			} else
			{
				assert(cdb_sc_needcrit == t_fail_hist[t_tries]);
				assert(!num_closed);	/* we can have at the most 1 region not opened in the whole tp_reg_list */
				num_closed++;
			}
		}

		if (tprestart_syslog_delta && (num_tprestart++ < tprestart_syslog_limit
					       || 0 == ((num_tprestart - tprestart_syslog_limit) % tprestart_syslog_delta)))
		{
			if (NULL != tp_fail_hist[t_tries])
				gvname_mstr = tp_fail_hist[t_tries]->gvname.var_name;
			else
			{
				gvname_mstr.addr = (char *)&gvname_unknown[0];
				gvname_mstr.len = gvname_unknown_len;
			}
			caller_id_flag = FALSE;		/* don't want caller_id in the operator log */
			assert(0 == cdb_sc_normal);
			if (cdb_sc_normal == t_fail_hist[t_tries])
				t_fail_hist[t_tries] = '0';	/* temporarily reset just for pretty printing */
			restart_reg = tp_fail_hist_reg[t_tries];
			if (NULL != restart_reg)
			{
				reg_mstr.len = restart_reg->dyn.addr->fname_len;
				reg_mstr.addr = (char *)&restart_reg->dyn.addr->fname[0];
			} else
			{
				reg_mstr.len = 0;
				reg_mstr.addr = NULL;
			}
			if (cdb_sc_blkmod != t_fail_hist[t_tries])
			{
				send_msg(VARLSTCNT(16) ERR_TPRESTART, 14, reg_mstr.len, reg_mstr.addr,
					 t_tries + 1, t_fail_hist, t_fail_hist_blk[t_tries], gvname_mstr.len, gvname_mstr.addr,
					 0, 0, 0, tp_blkmod_nomod,
					 (NULL != sgm_info_ptr) ? sgm_info_ptr->num_of_blks : 0,
					 (NULL != sgm_info_ptr) ? sgm_info_ptr->cw_set_depth : 0, &local_tn);
			} else
			{
				send_msg(VARLSTCNT(16) ERR_TPRESTART, 14, reg_mstr.len, reg_mstr.addr,
					 t_tries + 1, t_fail_hist, t_fail_hist_blk[t_tries], gvname_mstr.len, gvname_mstr.addr,
					 n_pvtmods, n_blkmods, tp_fail_level, tp_fail_n,
					 sgm_info_ptr->num_of_blks,
					 sgm_info_ptr->cw_set_depth, &local_tn);
			}
			tp_fail_hist_reg[t_tries] = NULL;
			tp_fail_hist[t_tries] = NULL;
			if ('0' == t_fail_hist[t_tries])
				t_fail_hist[t_tries] = cdb_sc_normal;	/* get back to where it was */
			caller_id_flag = TRUE;
			n_pvtmods = n_blkmods = 0;
		}
		/* Should never come here with a normal restart code unless it is the TRESTART command which resets t_tries to 0 */
		assert((cdb_sc_normal != t_fail_hist[t_tries]) || (0 == t_tries));
#		ifdef DEBUG
		t_fail_hist_array[t_fail_hist_index++] = t_fail_hist[t_tries];
		if (FAIL_HIST_ARRAY_SIZE <= t_fail_hist_index)
			t_fail_hist_index = 0;
#		endif
		if (cdb_sc_normal != t_fail_hist[t_tries])
		{	/* the following code is parallel, but not identical, to code in t_retry, which should also be maintained
			 *  in parallel
			 */
			switch (t_fail_hist[t_tries])
			{
				case cdb_sc_helpedout:
					csa = sgm_info_ptr->tp_csa;
					if (dba_bg == csa->hdr->acc_meth)
					{
						if (!csa->now_crit)
						{	/* The following grab/rel crit logic is purely to ensure that wcs_recover
							 * gets called if needed. This is because we saw wc_blocked to be TRUE in
							 * tp_tend and decided to restart.
							 */
							assert(!csa->hold_onto_crit);
							grab_crit(sgm_info_ptr->gv_cur_region);
							rel_crit(sgm_info_ptr->gv_cur_region);
						} else
						{	/* Some non-crit holding process set wc_blocked to TRUE causing us to
							 * restart even though we held crit. Most likely phase2 commit or a
							 * process in wcs_wtstart encountered an error. In any case, need to run
							 * cache-recovery to fix the shared memory structures. Since we hold crit,
							 * so no need to grab/rel crit. Call wcs_recover right away.
							 */
							DEBUG_ONLY(ok_to_call_wcs_recover = TRUE);
							wcs_recover(sgm_info_ptr->gv_cur_region);
							DEBUG_ONLY(ok_to_call_wcs_recover = FALSE);
						}
					} else
					{
						assert(dba_mm == csa->hdr->acc_meth);
						wcs_recover(sgm_info_ptr->gv_cur_region);
					}
					DEBUG_ONLY(uncounted_restarts++);
					if (CDB_STAGNATE > t_tries)
						break;
					/* WARNING - fallthrough !!! */
				case cdb_sc_future_read:
					t_tries = CDB_STAGNATE;		/* go straight to crit, pay $200 and do not pass go */
					/* WARNING - fallthrough !!! */
				case cdb_sc_needcrit:
					/* Here when a final (4th) attempt has failed with a need for crit in some routine. The
					 * assumption is that the previous attempt failed somewhere before transaction end
					 * therefore tp_reg_list did not have a complete list of regions necessary to complete the
					 * transaction and therefore not all the regions have been locked down. The new region (by
					 * virtue of it having now been referenced) has been added to tp_reg_list so all we need
					 * now is a retry.
					*/
					assert(CDB_STAGNATE == t_tries);
					for (tr = tp_reg_list;  NULL != tr;  tr = tr->fPtr)
					{	/* regions might not have been opened if we t_retried in gvcst_init(). dont
						 * rel_crit in that case.
						 */
						reg = tr->reg;
						if (reg->open)
						{
							DEBUG_ONLY(csa = &FILE_INFO(reg)->s_addrs;)
							assert(!csa->hold_onto_crit);
							rel_crit(reg);  /* to ensure deadlock safe order, release all regions
									 * before retry
									 */
						}
					}
					if ((NULL != jnlpool.jnlpool_dummy_reg) && jnlpool.jnlpool_dummy_reg->open)
					{
						csa = &FILE_INFO(jnlpool.jnlpool_dummy_reg)->s_addrs;
						if (csa->now_crit)
						{
							assert(!csa->hold_onto_crit);
							rel_lock(jnlpool.jnlpool_dummy_reg);
						}
					}
					wcs_backoff(dollar_trestart * TP_DEADLOCK_FACTOR); /* Sleep so needed locks have a chance
											    *  to get released
											    */
					break;
				case cdb_sc_jnlclose:
				case cdb_sc_jnlstatemod:
					if (CDB_STAGNATE <= t_tries)
					{
						t_tries--;
						DEBUG_ONLY(uncounted_restarts++);
					}
					/* fall through */
				default:
					if (CDB_STAGNATE < ++t_tries)
					{
						hist_index = t_tries;
						t_tries = 0;
						assert(gtm_white_box_test_case_enabled
						       && (WBTEST_TP_HIST_CDB_SC_BLKMOD == gtm_white_box_test_case_number));
#						ifdef UNIX
						send_msg(VARLSTCNT(5) ERR_TPFAIL, 2, hist_index, t_fail_hist, ERR_GVFAILCORE);
						/* Generate core only if not triggering this codepath using white-box tests */
						if (!gtm_white_box_test_case_enabled
						    || (WBTEST_TP_HIST_CDB_SC_BLKMOD != gtm_white_box_test_case_number))
							gtm_fork_n_core();
#						endif
						VMS_ONLY(send_msg(VARLSTCNT(4) ERR_TPFAIL, 2, hist_index, t_fail_hist));
						rts_error(VARLSTCNT(4) ERR_TPFAIL, 2, hist_index, t_fail_hist);
						return 0; /* for the compiler only -- never executed */
					} else
					{
						/* Yield the CPU so that the restarting process does not block the crit holder
						 * 	and/or other processes (referencing potentially non-intersecting database
						 * 	regions) in case they are waiting for the CPU. This is done using the
						 * 	rel_quant function.
						 * As of this writing, this operates only between the 2nd and 3rd tries;
						 * The 2nd is fast with the assumption of coincidental conflict in an attempt
						 * 	to take advantage of the buffer state created by the 1st try.
						 * The next to last try is not followed by a rel_quant as it may leave the buffers
						 * 	locked, to reduce live lock and deadlock issues.
						 * With only 4 tries that leaves only the "middle" for rel_quant.
						 */
						if ((0 < dollar_trestart) && ((CDB_STAGNATE - 1) > dollar_trestart))
							rel_quant();
					}
			}
			if ((CDB_STAGNATE <= t_tries))
			{	/* If there are any regions that haven't yet been opened, open them before attempting for crit on
				 * all. Since we don't hold any crit locks now, we can rest assured this cannot cause a deadlock.
				 * The only exception (to holding crit) currently is mupip journal rollback/recovery if online when
				 * we will be holding crit on all regions but in that case we should have opened all necessary
				 * regions at process startup (and set "hold_onto_locks" to TRUE) so we should not be here at all.
				 * Assert this.
				 */
				assert(!hold_onto_locks);
				if (num_closed)
				{
					for (tr = tp_reg_list;  NULL != tr;  tr = tr->fPtr)
					{	/* to open region use gv_init_reg() instead of gvcst_init() since that does extra
						 * manipulations with gv_keysize, gv_currkey and gv_altkey.
						 */
						reg = tr->reg;
						if (!reg->open)
						{
							gv_init_reg(reg);
							assert(reg->open);
						}
					}
					DBG_CHECK_TP_REG_LIST_SORTING(tp_reg_list);
				}
				DEBUG_ONLY(ok_to_call_wcs_recover = TRUE);
				tp_tend_status = tp_crit_all_regions();	/* grab crits on all regions */
				DEBUG_ONLY(ok_to_call_wcs_recover = FALSE);
				assert(FALSE != tp_tend_status);
				/* pick up all MM extension information */
				for (tr = tp_reg_list; NULL != tr; tr = tr->fPtr)
				{
					reg = tr->reg;
					if (dba_mm == reg->dyn.addr->acc_meth)
					{
						TP_CHANGE_REG_IF_NEEDED(reg);
						MM_DBFILEXT_REMAP_IF_NEEDED(cs_addrs, gv_cur_region);
					}
				}
			}
		}
#	ifdef GTM_TRIGGER
	}
#	endif
	/* The below code to determine the roll-back point depends on tp_frame sized blocks being pushed on the TP
	 * stack. If ever other sized blocks are pushed on, a different method will need to be found.
	 */
	assert(0 == ((tpstackbase - (unsigned char *)tp_pointer) % SIZEOF(tp_frame))); /* Simple check for above condition */
	tf = (tp_frame *)(tpstackbase - (newlevel * SIZEOF(tp_frame)));
	assert(NULL != tf);
	assert(tpstacktop < (unsigned char *)tf);
#	ifdef GTM_TRIGGER
	if (TPRESTART_STATE_NORMAL == tprestart_state)
	{	/* Only if normal tp_restart call - else we've already done this for this tp_restart */
#	endif
		/* Before we get too far unwound here, if this is a nonrestartable transaction,
		   let's record where we are for the message later on. */
		if (FALSE == tf->restartable && IS_MCODE_RUNNING)
			getzposition(&bangHere);
		/* Do a rollback type cleanup (invalidate gv_target clues of read as well as updated blocks) */
		tp_clean_up(TRUE);
#	ifdef GTM_TRIGGER
	}
	if (TPRESTART_STATE_TPUNW >= tprestart_state)
	{	/* Either this is a normal tp_restart call or we ran into a trigger base frame while tp_unwind()
		   was running which required M and C stack unwinding before we could proceed so this call is
		   being restarted.
		*/
#	endif
		/* Note that this form of tp_unwind() will not only unwind the TP stack but also most if not all of
		   the M stackframe and mv_stent chain as well.
		*/
		tp_unwind(newlevel, RESTART_INVOCATION, &tprestart_rc);
		assert(tf == tp_pointer);	/* Needs to be true for now. Revisit when can restart to other than newlevel == 1 */
		gd_header = tp_pointer->gd_header;
		gv_target = tp_pointer->orig_gv_target;
		gv_cur_region = tp_pointer->gd_reg;
		TP_CHANGE_REG(gv_cur_region);
		DBG_CHECK_GVTARGET_CSADDRS_IN_SYNC;
		dollar_tlevel = newlevel;
		top = gv_currkey->top;
		/* ensure proper alignment before dereferencing tp_pointer->orig_key->end */
		assert(0 == (((unsigned long)tp_pointer->orig_key) % SIZEOF(tp_pointer->orig_key->end)));
		memcpy(gv_currkey, tp_pointer->orig_key, SIZEOF(*tp_pointer->orig_key) + tp_pointer->orig_key->end);
		gv_currkey->top = top;
		DBG_CHECK_GVTARGET_GVCURRKEY_IN_SYNC;
		tp_pointer->fp->mpc = tp_pointer->restart_pc;
		tp_pointer->fp->ctxt = tp_pointer->restart_ctxt;
#	ifdef GTM_TRIGGER
	} else
		assert(TPRESTART_STATE_MSTKUNW == tprestart_state);
	/* From here on, all states run */
#	endif
	/* Make sure everything else added to the stack since the transaction started is unwound. Note this loop only
	 * has work to do if there were NO local vars to restore. Otherwise tp_unwind would have already unwound the
	 * stack for us.
	 */
	while (frame_pointer != tf->fp)
	{
#		ifdef GTM_TRIGGER
		if (SFT_TRIGR & frame_pointer->type)
		{	/* We have encountered a trigger base frame. We cannot unroll it because there are C frames
			   associated with it so we must interrupt this tp_restart and return to gtm_trigger() so
			   it can unroll the base frame and rethrow the error to properly unroll the C stack.
			*/
			tprestart_rc = ERR_TPRETRY;
			tprestart_state = TPRESTART_STATE_MSTKUNW;
			DBGTRIGR((stderr, "tp_restart: Encountered trigger base frame in M-stack unwind - rethrowing\n"));
			INVOKE_RESTART;
		}
#		endif
		op_unwind();
	}
	/* From here on, no further rethrows of tp_restart() - the final finishing touches */
	assert((msp <= stackbase) && (msp > stacktop));
	assert((mv_chain <= (mv_stent *)stackbase) && (mv_chain > (mv_stent *)stacktop));
	assert(MVST_TPHOLD == tf->mvc->mv_st_type);
	for (mvc = mv_chain;  mvc < tf->mvc;)
	{
		unw_mv_ent(mvc);
		mvc = (mv_stent *)(mvc->mv_st_next + (char *)mvc);
	}
	assert((void *)mvc < (void *)frame_pointer);
	assert(mvc == tf->mvc);
	assert(mvc->mv_st_cont.mvs_tp_holder.tphold_tlevel == (dollar_tlevel - 1));
	mv_chain = mvc;
	msp = (unsigned char *)mvc;
#	ifdef GTM_TRIGGER
	/* Revert $ZTWormhole to its previous value */
	memcpy(&dollar_ztwormhole, &mvc->mv_st_cont.mvs_tp_holder.ztwormhole_save, SIZEOF(mval));
	if (1 == newlevel)
		memcpy(&dollar_ztslate, &literal_null, SIZEOF(mval));	/* Zap $ZTSLate at (re)start of lvl 1 transaction */
#	endif
	assert(curr_symval == tf->sym);
	if (frame_pointer->flags & SFF_UNW_SYMVAL)
	{	/* A symval was popped in THIS stackframe by one of our last mv_stent unwinds which means
		   l_symtab is fairly borked.
		*/
		assert(frame_pointer->l_symtab);	/* Would be NULL in replication processor */
		if ((unsigned char *)frame_pointer->l_symtab < msp)
		{	/* This condition is set up when a local routine is called which, since it is using the
			   same code, uses the same l_symtab as the caller. But when an exclusive new is done in
			   this frame, op_xnew creates a NEW symtab just for this frame. But when this code
			   unwound back to the TSTART, we also unwound the l_symtab this frame was using. So here
			   we verify this frame is a simple call frame from the previous and restore the use of its
			   l_symtab if so. If not, GTMASSERT. Note the outer SFF_UWN_SYMVAL check keeps us from having
			   non-existant l_symtab issues which is possible when we are MUPIP.
			*/
			if ((frame_pointer->rvector == frame_pointer->old_frame_pointer->rvector)
			    && (frame_pointer->vartab_ptr == frame_pointer->old_frame_pointer->vartab_ptr))
			{
				frame_pointer->l_symtab = frame_pointer->old_frame_pointer->l_symtab;
				frame_pointer->flags &= SFF_UNW_SYMVAL_OFF;	/* No need to clear symtab now */
			} else
				GTMASSERT;
		} else
		{	/* Otherwise the l_symtab needs to be cleared so its references get re-resolved to *this* symtab */
			memset(frame_pointer->l_symtab, 0, frame_pointer->vartab_len * SIZEOF(ht_ent_mname *));
			frame_pointer->flags &= SFF_UNW_SYMVAL_OFF;
		}
	}
	if (FALSE == tf->restartable)
	{
		if (IS_MCODE_RUNNING)
		{
			getzposition(&beganHere);
			send_msg(VARLSTCNT(1) ERR_TRESTNOT);		/* Separate msgs so we get both */
			send_msg(VARLSTCNT(6) ERR_TRESTLOC, 4, beganHere.str.len, beganHere.str.addr,
				 bangHere.str.len, bangHere.str.addr);
			rts_error(VARLSTCNT(8) ERR_TRESTNOT, 0,
				  ERR_TRESTLOC, 4, beganHere.str.len, beganHere.str.addr, bangHere.str.len, bangHere.str.addr);
		} else
			rts_error(VARLSTCNT(1) ERR_TRESTNOT);
		return 0; /* for the compiler only -- never executed */
	}
	++dollar_trestart;
	assert(dollar_trestart >= uncounted_restarts);
	assert(MAX_TRESTARTS > (dollar_trestart - uncounted_restarts)); /* a magic number to ensure we dont do too many restarts */
	if (!dollar_trestart)		/* in case of a wrap */
		dollar_trestart--;
	dollar_truth = tp_pointer->dlr_t;
	dollar_zgbldir = tp_pointer->zgbldir;
	if (handle_errors_internally)
		REVERT;
	GTMTRIG_ONLY(tprestart_state = TPRESTART_STATE_NORMAL);
	GTMTRIG_ONLY(DBGTRIGR((stderr, "tp_restart: completed\n"));)
	return 0;
}
