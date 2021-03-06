/****************************************************************
 *								*
 * Copyright (c) 2001-2015 Fidelity National Information 	*
 * Services, Inc. and/or its subsidiaries. All rights reserved.	*
 *								*
 *	This source code contains the intellectual property	*
 *	of its copyright holder(s), and is made available	*
 *	under a license.  If you do not know the terms of	*
 *	the license, please stop and do not read further.	*
 *								*
 ****************************************************************/

#include "mdef.h"
#include "io.h"
#include "iottdef.h"
#include "outofband.h"
#include "deferred_events.h"
#include "std_dev_outbndset.h"

#define SHFT_MSK 0x00000001

GBLREF boolean_t		ctrlc_on;
GBLREF volatile io_pair		io_std_device;
GBLREF volatile int4		spc_inp_prc;
GBLREF volatile bool		ctrlu_occurred;
GBLREF volatile bool		std_dev_outbnd;

/* NOTE: xfer_set_handlers() returns success or failure for attempts to set
 *       xfer_table. That value is not currently used here, hence the
 *       cast to void.
 */

void std_dev_outbndset(int4 ob_char)
{
	uint4		mask;
	unsigned short	n;
	d_tt_struct	*tt_ptr;

	assertpro(MAXOUTOFBAND >= ob_char);
	if ((NULL != io_std_device.in) && (tt == io_std_device.in->type))
	{	/* The NULL check above protects against a <CTRL-C> hitting while we're initializing the terminal */
		tt_ptr = (d_tt_struct *)io_std_device.in->dev_sp;
		std_dev_outbnd = TRUE;
		mask = SHFT_MSK << ob_char;
		if (mask & tt_ptr->enbld_outofbands.mask)
			(void)xfer_set_handlers(outofband_event, &ctrap_set, ob_char);
		else if (mask & CTRLC_MSK)
		{
			if (ctrlc_on)
		        	(void)xfer_set_handlers(outofband_event, &ctrlc_set, 0);
		} else if (mask & CTRLY_MSK)
	        	(void)xfer_set_handlers(outofband_event, &ctrly_set, 0);
		else if ((CTRL_U == ob_char) && (spc_inp_prc & (SHFT_MSK << CTRL_U)))
			ctrlu_occurred = TRUE;
		else
			assertpro((mask & tt_ptr->enbld_outofbands.mask) || (mask & CTRLC_MSK) || (mask & CTRLY_MSK)
				|| ((CTRL_U == ob_char) && (spc_inp_prc & (SHFT_MSK << CTRL_U))));
	}
}
