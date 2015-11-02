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

#include "hashtab_mname.h"	/* needed for lv_val.h */
#include "lv_val.h"
#include "sbs_blk.h"

lv_val *lv_ins_num_sbs(sbs_search_status *stat, mval *key, lv_sbs_tbl *tbl)
{
       	sbs_blk	       	*blk, *new, *nxt, *prev;
       	sbs_flt_struct 	*src, *dst, *slot;
 	int  	       	max_count;
	lv_val		*lv;
       	char		*top;

	assert(tbl->sym->ident == MV_SYM);
       	lv = lv_getslot(tbl->sym);
	memset(lv, 0, SIZEOF(lv_val));
       	lv->ptrs.val_ent.parent.sbs = tbl;

	assert(stat->prev == stat->blk || stat->prev->nxt == stat->blk);
       	assert(stat->prev->cnt <= SBS_NUM_FLT_ELE && stat->prev->cnt >= 0);
       	assert(stat->blk->cnt <= SBS_NUM_FLT_ELE && stat->blk->cnt >= 0);

	blk = stat->blk;
       	max_count = SBS_NUM_FLT_ELE;
       	if (blk->cnt < max_count)
	{
       	       	dst = &blk->ptr.sbs_flt[blk->cnt];
       	       	src = dst - 1;
	       	for ( ; src >= (sbs_flt_struct *) stat->ptr; src--, dst--)
			*dst = *src;
	 	slot = (sbs_flt_struct *)stat->ptr;
	       	blk->cnt++;
		assert(blk->cnt <= max_count);
       	} else if (stat->prev != stat->blk && stat->prev->cnt < max_count)
 	{	/* flow into previous block */
 	       	prev = stat->prev;
       	       	dst = &prev->ptr.sbs_flt[prev->cnt];
       	       	if (stat->ptr == (char *)&blk->ptr.sbs_flt[0])
			slot = dst;
	 	else
	 	{
			src = &blk->ptr.sbs_flt[0];
       	       	       	*dst = *src;
       	       	       	dst = src;
       	       	       	src += 1;
       	       	       	memcpy(dst, src, (char *)stat->ptr - (char *)src);
       	       	       	slot = (sbs_flt_struct *)((char *)stat->ptr - SIZEOF(sbs_flt_struct));
	 	}
 	       	prev->cnt++;
		assert(prev->cnt <= max_count);
	} else if (blk->nxt && blk->nxt->cnt < max_count)
	{      	/* flow into next block */
	 	nxt = blk->nxt;
       	       	dst = &nxt->ptr.sbs_flt[nxt->cnt];
       	       	src = dst - 1;
       	       	for ( ; src >= &nxt->ptr.sbs_flt[0]; src--, dst--)
			*dst = *src;

	 	if (stat->ptr == (char *)&blk->ptr.sbs_flt[blk->cnt])
			slot = dst;
	     	else
	     	{
			*dst = blk->ptr.sbs_flt[blk->cnt - 1];
	       	       	dst = &blk->ptr.sbs_flt[blk->cnt - 1];
       	     	       	src = dst - 1;
	       	 	for ( ; src >= (sbs_flt_struct *)stat->ptr; src--, dst--)
				*dst = *src;
	 	 	slot = (sbs_flt_struct *)stat->ptr;
	 	}
	 	nxt->cnt++;
		assert(nxt->cnt <= max_count);
       	} else
	{    	/* split block */
	     	new = lv_get_sbs_blk (tbl->sym);
		assert (new->cnt == 0);
		assert (new->nxt == 0);
		assert (new->sbs_que.fl && new->sbs_que.bl);
		new->nxt = blk;
		if (stat->prev == stat->blk)
			tbl->num = new;
		else
			stat->prev->nxt = new;

       	       	dst = &new->ptr.sbs_flt[0];
       	       	if (stat->ptr == (char *)&blk->ptr.sbs_flt[0])
	     	{
			slot = dst;
			new->cnt = 1;
	     	} else
	     	{
       	       	       	src = &blk->ptr.sbs_flt[0];
       	       	       	for ( ; src < (sbs_flt_struct *)stat->ptr; src++, dst++)
				*dst = *src;
			new->cnt = INTCAST(dst - &new->ptr.sbs_flt[0]);
	     		slot = &blk->ptr.sbs_flt[0];
	     		dst = slot + 1;
	     		top = (char *)&blk->ptr.sbs_flt[blk->cnt];
       	       	       	for ( ; src < (sbs_flt_struct *)top; src++, dst++)
				*dst = *src;
			blk->cnt = blk->cnt - new->cnt + 1;
			assert((blk->cnt <= max_count) && (0 < blk->cnt));
	 	}
	}
	MV_ASGN_MVAL2FLT(slot->flt,(*key)) ;
	slot->lv = lv;
	return (lv);
}
