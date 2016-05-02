/*
 * vim:noexpandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright Stony Brook University  (2016)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 3 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 * ---------------------------------------
 */

/**
 * @file    nfs4_op_copy.c
 * @brief   Routines used for managing the NFS4 COMPOUND functions.
 *
 * Routines used for managing the NFS4 COMPOUND functions.
 *
 *
 */
#include "config.h"
#include "fsal.h"
#include "log.h"
#include "nfs4.h"
#include "nfs_core.h"
#include "sal_functions.h"
#include "nfs_proto_functions.h"
#include "nfs_proto_tools.h"
#include "nfs_convert.h"
#include "nfs_file_handle.h"
#include "sal_functions.h"

/**
 * @brief The NFS4_OP_COPY operation
 *
 * This function implemenats the NFS4_OP_COPY operation. This
 * function can be called only from nfs4_Compound
 *
 * @param[in]     op   Arguments for nfs4_op
 * @param[in,out] data Compound request's data
 * @param[out]    resp Results for nfs4_op
 *
 * @return per RFC5661, p. 373
 */

int nfs4_op_copy(struct nfs_argop4 *op, compound_data_t *data,
                 struct nfs_resop4 *resp) {
	COPY4args *const arg_COPY4 = &op->nfs_argop4_u.opcopy;
	COPY4res *const res_COPY4 = &resp->nfs_resop4_u.opcopy;
	cache_entry_t *dst_entry = NULL;
	cache_entry_t *src_entry = NULL;
	cache_inode_status_t cache_status = CACHE_INODE_SUCCESS;
	size_t copied = 0;
	struct gsh_buffdesc verf_desc;

	resp->resop = NFS4_OP_COPY;
	res_COPY4->cr_status = NFS4_OK;

	/* Do basic checks on a filehandle */
	res_COPY4->cr_status = nfs4_sanity_check_FH(data, REGULAR_FILE, false);
	if (res_COPY4->cr_status != NFS4_OK)
		goto out;

	res_COPY4->cr_status =
	    nfs4_sanity_check_saved_FH(data, REGULAR_FILE, false);
	if (res_COPY4->cr_status != NFS4_OK)
		goto out;

	if (nfs_in_grace()) {
		res_COPY4->cr_status = NFS4ERR_GRACE;
		goto out;
	}

	dst_entry = data->current_entry;
	src_entry = data->saved_entry;
	/* RFC: "SAVED_FH and CURRENT_FH must be different files." */
	if (src_entry == dst_entry) {
		res_COPY4->cr_status = NFS4ERR_INVAL;
		goto out;
	}

	cache_status = cache_inode_copy(src_entry, arg_COPY4->ca_src_offset,
					dst_entry, arg_COPY4->ca_dst_offset,
					arg_COPY4->ca_count, &copied);
	if (cache_status != CACHE_INODE_SUCCESS) {
		res_COPY4->cr_status = nfs4_Errno(cache_status);
		goto out;
	}

	res_COPY4->COPY4res_u.cr_bytes_copied = copied;
	res_COPY4->COPY4res_u.cr_resok4.wr_ids = 0;
	res_COPY4->COPY4res_u.cr_resok4.wr_count = copied;
	/* FIXME: for simplicity, we always sync file after copy */
	res_COPY4->COPY4res_u.cr_resok4.wr_committed = FILE_SYNC4;

	verf_desc.addr = &res_COPY4->COPY4res_u.cr_resok4.wr_writeverf;
	verf_desc.len = sizeof(verifier4);
	op_ctx->fsal_export->exp_ops.get_write_verifier(&verf_desc);

out:
	return res_COPY4->cr_status;
}

/**
 * @brief Free memory allocated for COPY result
 *
 * This function frees any memory allocated for the result of the
 * NFS4_OP_COPY operation.
 *
 * @param[in,out] resp nfs4_op results
 */
void nfs4_op_copy_Free(nfs_resop4 *resp)
{
	/* Nothing to be done */
}
