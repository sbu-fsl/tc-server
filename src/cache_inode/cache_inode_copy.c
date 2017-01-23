/*
 * vim:noexpandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright Stony Brook University (2016)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
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
 * @addtogroup cache_inode
 * @{
 */

/**
 * @file cache_inode_copy.c
 * @brief Performs I/O on regular files
 */

#include "config.h"
#include "fsal.h"

#include "log.h"
#include "hashtable.h"
#include "cache_inode.h"
#include "cache_inode_lru.h"
#include "nfs_core.h"
#include "nfs_exports.h"
#include "export_mgr.h"

#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <time.h>
#include <pthread.h>
#include <assert.h>

/**
 * @brief Copy file content.
 *
 * @param[in]     src_entry    File to copy from
 * @param[in]     src_offset   Offset start from the source file
 * @param[in]     dst_entry    Destination file to copy to
 * @param[in]     dst_offset   Offset in the dest file
 * @param[out]    count	       Requested bytes to copy
 * @param[out]    copied       Bytes successfully copied
 *
 * @return CACHE_INODE_SUCCESS or various errors
 */
cache_inode_status_t cache_inode_copy(cache_entry_t *src_entry,
				      uint64_t src_offset,
				      cache_entry_t *dst_entry,
				      uint64_t dst_offset, uint64_t count,
				      uint64_t *copied)
{
	fsal_status_t fsal_status = { 0, 0 };
	cache_inode_status_t status;

	/**
	 * To avoid deadlock, we always lock the entry with a smaller address
	 * before the locking the other entry.  Note that "content_lock"
	 * protects "cache content" instead of file content.  So only reader
	 * lock is needed for either file.
	 */
	if ((size_t)src_entry < (size_t)dst_entry) {
		PTHREAD_RWLOCK_rdlock(&src_entry->content_lock);
		PTHREAD_RWLOCK_rdlock(&dst_entry->content_lock);
	} else {
		PTHREAD_RWLOCK_rdlock(&dst_entry->content_lock);
		PTHREAD_RWLOCK_rdlock(&src_entry->content_lock);
	}

	if (!is_open(src_entry) || !is_open(dst_entry)) {
		LogEvent(COMPONENT_CACHE_INODE,
			 "Cannot copy between files that are not open");
		status = NFS4ERR_OPENMODE;
		goto out;
	}

	if (count == UINT64_MAX) {
		count = src_entry->obj_handle->attrs->filesize - src_offset;
		LogDebug(COMPONENT_CACHE_INODE,
			 "0-count has an effective value of %zu", count);
	}

	fsal_status = src_entry->obj_handle->obj_ops.copy(src_entry->obj_handle,
							  src_offset,
							  dst_entry->obj_handle,
							  dst_offset,
							  count,
							  copied);

	if (FSAL_IS_ERROR(fsal_status)) {
		*copied = 0;
		status = cache_inode_error_convert(fsal_status);
		LogEvent(COMPONENT_CACHE_INODE,
			 "File copy failed: major = %d, minor = %d",
			 fsal_status.major, fsal_status.minor);
		goto out;
	}

	/* Update dest file after coping to it. */
	PTHREAD_RWLOCK_wrlock(&dst_entry->attr_lock);
	status = cache_inode_refresh_attrs(dst_entry);
	PTHREAD_RWLOCK_unlock(&dst_entry->attr_lock);

out:
	if ((size_t)src_entry < (size_t)dst_entry) {
		PTHREAD_RWLOCK_unlock(&dst_entry->content_lock);
		PTHREAD_RWLOCK_unlock(&src_entry->content_lock);
	} else {
		PTHREAD_RWLOCK_unlock(&src_entry->content_lock);
		PTHREAD_RWLOCK_unlock(&dst_entry->content_lock);
	}

	return status;
}

/** @} */
