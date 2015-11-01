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

#include <stddef.h>

#include "gdsroot.h"
#include "gtm_facility.h"
#include "fileinfo.h"
#include "gdsbt.h"
#include "gdsfhead.h"
#include "filestruct.h"
#include "jnl.h"
#include "copy.h"
#include "iosp.h"
#include "repl_filter.h"
#include "repl_errno.h"

GBLREF unsigned int	jnl_source_datalen, jnl_dest_maxdatalen;
GBLREF unsigned char	jnl_source_rectype, jnl_dest_maxrectype;
GBLREF char		jn_tid[8];

LITREF int      	v07_jnl_fixed_size[], jnl_fixed_size[];

int jnl_v12tov07(uchar_ptr_t jnl_buff, uint4 *jnl_len, uchar_ptr_t conv_buff, uint4 *conv_len, uint4 conv_bufsiz)
{
	/* Convert a transaction from jnl version 07 (V4.1) to jnl version 12 (post V4.2-000) */

	/* Differences between ver 07, and 12 :
	 *					       07			        12
	 *------------------------------------------------------------------------------------------------------------
	 * {tcom,ztcom}.token			4 bytes				8 bytes
	 * {tcom,ztcom}.participants		NA				Shifted by 8 bytes due to addition
	 * 										of orig_tc_short_time
	 * {tcom,ztcom}.ts_short_time		same as above
	 * An addtional field orig_ts_short_time is added.
	 * mumps data value for SET type	ushort(2 bytes), char[]		uint(4 bytes), char[]
	 * records (len, string) for non VAX
	 */

	unsigned char		*jb, *cb, *cstart, *jstart, rectype;
	int			status, reclen;
	unsigned short		key_len, data_len;
	unsigned int		long_data_len, total_data, jlen, less_space, nzeros, conv_reclen, clen_without_sfx, total_key;
	boolean_t		is_set, is_com;

	jb = jnl_buff;
	cb = conv_buff;
	status = SS_NORMAL;
	jlen = *jnl_len;
	while (0 < jlen)
	{
		if (0 < (reclen = jnl_record_length((jnl_record *)jb, jlen)))
		{
			if (reclen <= jlen)
			{
				rectype = REF_CHAR(jb + V12_JREC_TYPE_OFFSET);
				if (rectype >= V07_JRT_RECTYPES)
				{
					jnl_source_rectype = rectype;
					jnl_dest_maxrectype = V07_JRT_RECTYPES - 1;
					repl_errno = EREPL_INTLFILTER_NEWREC;
					status = -1;
					break;
				}
				is_set = is_com = FALSE;
				less_space = total_key = total_data = 0;
				if ((is_set = (JRT_SET == rectype || JRT_TSET == rectype || JRT_USET == rectype
							|| JRT_FSET == rectype || JRT_GSET == rectype))
					|| (JRT_KILL == rectype || JRT_TKILL == rectype || JRT_UKILL == rectype
						|| JRT_FKILL == rectype || JRT_GKILL == rectype)
					|| (JRT_ZKILL == rectype || JRT_FZKILL == rectype || JRT_GZKILL == rectype
						|| JRT_TZKILL == rectype || JRT_UZKILL == rectype))
				{
					GET_USHORT(key_len, jb + V12_JREC_PREFIX_SIZE + jnl_fixed_size[rectype]);
					total_key = key_len + sizeof(unsigned short);
					if (is_set)
					{
						less_space = sizeof(mstr_len_t) - sizeof(unsigned short);
						GET_MSTR_LEN(long_data_len,
							     jb + V12_JREC_PREFIX_SIZE + jnl_fixed_size[rectype] + total_key);
						if (long_data_len >= (1L << (BITS_PER_UCHAR * sizeof(unsigned short))))
						{
							jnl_source_datalen = long_data_len;
							jnl_dest_maxdatalen = (1L << (BITS_PER_UCHAR * sizeof(unsigned short))) - 1;
							repl_errno = EREPL_INTLFILTER_DATA2LONG;
							status = -1;
							break;
						}
						total_data = long_data_len + sizeof(mstr_len_t);
					}
				}
				if (JRT_TCOM == rectype || JRT_ZTCOM == rectype)
					is_com = TRUE;
				less_space += jnl_fixed_size[rectype] - v07_jnl_fixed_size[rectype];

				assert(V07_JNL_REC_START_BNDRY == V12_JNL_REC_START_BNDRY);
				assert(V07_JREC_PREFIX_SIZE == V12_JREC_PREFIX_SIZE);
				clen_without_sfx = ROUND_UP(V12_JREC_PREFIX_SIZE + jnl_fixed_size[rectype] + total_key +
							    total_data - less_space, V07_JNL_REC_START_BNDRY);
				conv_reclen = clen_without_sfx + V07_JREC_SUFFIX_SIZE;
				if (cb - conv_buff + conv_reclen > conv_bufsiz)
				{
					repl_errno = EREPL_INTLFILTER_NOSPC;
					status = -1;
					break;
				}
				cstart = cb;
				jstart = jb;
				memcpy(cb, jb, V07_JREC_PREFIX_SIZE + V07_MUMPS_NODE_OFFSET);  /* copy till Mumps Node */
				cb += (V07_JREC_PREFIX_SIZE + V07_MUMPS_NODE_OFFSET);
				jb += (V12_JREC_PREFIX_SIZE + V12_MUMPS_NODE_OFFSET);
				if ((JRT_TSET == rectype || JRT_USET == rectype || JRT_FSET == rectype || JRT_GSET == rectype)
						|| (JRT_TKILL == rectype || JRT_UKILL == rectype || JRT_FKILL == rectype
						|| JRT_GKILL == rectype) || (JRT_FZKILL == rectype || JRT_GZKILL == rectype
						|| JRT_TZKILL == rectype || JRT_UZKILL == rectype))
				{
					memcpy(cb, jb, TP_TOKEN_TID_SIZE); /* This is ok as we have filler for the token in 07 */
					cb += TP_TOKEN_TID_SIZE;
					jb += TP_TOKEN_TID_SIZE;
				}
				memcpy(cb, jb, total_key);
				cb += total_key;
				jb += total_key;
				if (is_set)
				{
					data_len = (unsigned short)long_data_len;
					PUT_USHORT(cb, data_len);
					cb += sizeof(unsigned short);
					jb += sizeof(mstr_len_t);
					memcpy(cb, jb, data_len);
					cb += data_len;
					jb += data_len;
				} else if (is_com)
				{
					assert(0 == ((unsigned long)(cstart + V07_JREC_PREFIX_SIZE + V07_TCOM_TOKEN_OFFSET) &
						     (sizeof(uint4) - 1)));
					*(uint4 *)(cstart + V07_JREC_PREFIX_SIZE + V07_TCOM_TOKEN_OFFSET) = 0; /* don't care */
					assert(0 == ((unsigned long)(cstart + V07_JREC_PREFIX_SIZE + V07_TCOM_PARTICIPANTS_OFFSET) &
						     (sizeof(uint4) - 1)));
					assert(0 == ((unsigned long)(jstart + V12_JREC_PREFIX_SIZE + V12_TCOM_PARTICIPANTS_OFFSET) &
						     (sizeof(uint4) - 1)));
					*(uint4 *)(cstart + V07_JREC_PREFIX_SIZE + V07_TCOM_PARTICIPANTS_OFFSET) =
						*(uint4 *)(jstart + V12_JREC_PREFIX_SIZE + V12_TCOM_PARTICIPANTS_OFFSET);
				}
				nzeros = (cstart + clen_without_sfx - cb);
				if (nzeros > 0)
				{
					memset(cb, 0, nzeros);
					cb += nzeros;
				}
				jb = jstart + reclen;
				assert(V07_JREC_SUFFIX_SIZE == V12_JREC_SUFFIX_SIZE);
				memcpy(cb, jb - V12_JREC_SUFFIX_SIZE, V12_JREC_SUFFIX_SIZE);
				cb += V07_JREC_SUFFIX_SIZE;
				assert(cb == cstart + conv_reclen);
				jlen -= reclen;
				continue;
			}
			repl_errno = EREPL_INTLFILTER_INCMPLREC;
			status = -1;
			break;
		}
		repl_errno = EREPL_INTLFILTER_BADREC;
		status = -1;
		break;
	}
	assert(0 == jlen || -1 == status);
	*conv_len = cb - conv_buff;
	*jnl_len = jb - jnl_buff;
	return(status);
}
