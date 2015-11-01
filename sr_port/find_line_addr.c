/****************************************************************
 *								*
 *	Copyright 2001 Sanchez Computer Associates, Inc.	*
 *								*
 *	This source code contains the intellectual property	*
 *	of its copyright holder(s), and is made available	*
 *	under a license.  If you do not know the terms of	*
 *	the license, please stop and do not read further.	*
 *								*
 ****************************************************************/

#include "mdef.h"

#include "gtm_string.h"

#include "rtnhdr.h"
#include "cmd_qlf.h"
#include "gtm_caseconv.h"

GBLREF command_qualifier	cmd_qlf;

int4 *find_line_addr (rhdtyp *routine, mstr *label, short int offset)
{
 	lbl_tables	*base, *top, *ptr;
	rhdtyp		*real_routine;
	mident		target_label;
	int4		*line_table, *first_line;
	int		stat, n;
	error_def(ERR_LABELONLY);

	if (!routine)
		return 0;

	if (routine->label_only  &&  offset != 0)
		rts_error(VARLSTCNT(4) ERR_LABELONLY, 2, mid_len((mident *)&routine->routine_name), routine->routine_name);

	real_routine = (rhdtyp *)((char *)routine + routine->current_rhead_ptr);
	first_line = (int4 *)((char *)real_routine + real_routine->lnrtab_ptr);

	if (!label->len  ||  !*label->addr)
	{
		line_table = first_line;
	}
	else
	{
		memset(&target_label.c[0], 0, sizeof(mident));
		if (cmd_qlf.qlf & CQ_LOWER_LABELS)
			memcpy(&target_label.c[0], label->addr,
				(label->len <= sizeof(mident)) ? label->len : sizeof(mident));
		else
			lower_to_upper((uchar_ptr_t)&target_label.c[0], (uchar_ptr_t)label->addr,
				(label->len <= sizeof(mident)) ? label->len : sizeof(mident));

		ptr = base = (lbl_tables *)((char *) real_routine + real_routine->labtab_ptr);
		top = base + real_routine->labtab_len;
		for (  ;  ;  )
		{
			n = (top - base) / 2;
			ptr = base + n;
			stat = memcmp(&target_label.c[0], &ptr->lab_name.c[0], sizeof(mident));
			if (!stat)
			{
				line_table = (int4 *)((char *)real_routine + ptr->lab_ln_ptr);
				break;
			}
			else if (stat > 0)
				base = ptr;
			else
				top = ptr;

			if (n < 1)
				return 0;
		}
	}

	line_table += offset;
	if (line_table < first_line  ||  line_table >= first_line + real_routine->lnrtab_len)
		return 0;

	return line_table;
}
