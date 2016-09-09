/*
 * vim:noexpandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright (C) Panasas Inc., 2011
 * Author: Jim Lieb jlieb@panasas.com
 *
 * contributeur : Philippe DENIEL   philippe.deniel@cea.fr
 *                Thomas LEIBOVICI  thomas.leibovici@cea.fr
 *
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
 * -------------
 */

/* file.c
 * File I/O methods for VFS module
 */

#include "config.h"

#include <assert.h>
#include "fsal.h"
#include "FSAL/access_check.h"
#include "fsal_convert.h"
#include <unistd.h>
#include <fcntl.h>
#include "FSAL/fsal_commonlib.h"
#include "vfs_methods.h"
#include "splice_copy.h"

/** vfs_open
 * called with appropriate locks taken at the cache inode level
 */

fsal_status_t vfs_open(struct fsal_obj_handle *obj_hdl,
		       fsal_openflags_t openflags)
{
	struct vfs_fsal_obj_handle *myself;
	int fd;
	fsal_errors_t fsal_error = ERR_FSAL_NO_ERROR;
	int posix_flags = 0;
	int retval = 0;

	myself = container_of(obj_hdl, struct vfs_fsal_obj_handle, obj_handle);

	if (obj_hdl->fsal != obj_hdl->fs->fsal) {
		LogDebug(COMPONENT_FSAL,
			 "FSAL %s operation for handle belonging to FSAL %s, return EXDEV",
			 obj_hdl->fsal->name, obj_hdl->fs->fsal->name);
		retval = EXDEV;
		fsal_error = posix2fsal_error(retval);
		return fsalstat(fsal_error, retval);
	}

	/* Take write lock on object to protect file descriptor. */
	PTHREAD_RWLOCK_wrlock(&obj_hdl->lock);

	assert(myself->u.file.fd == -1
	       && myself->u.file.openflags == FSAL_O_CLOSED && openflags != 0);

	fsal2posix_openflags(openflags, &posix_flags);
	LogFullDebug(COMPONENT_FSAL, "open_by_handle_at flags from %x to %x",
		     openflags, posix_flags);
	fd = vfs_fsal_open(myself, posix_flags, &fsal_error);
	if (fd < 0) {
		retval = -fd;
	} else {
		myself->u.file.fd = fd;
		myself->u.file.openflags = openflags;
	}

	PTHREAD_RWLOCK_unlock(&obj_hdl->lock);

	return fsalstat(fsal_error, retval);
}

/* vfs_status
 * Let the caller peek into the file's open/close state.
 */

fsal_openflags_t vfs_status(struct fsal_obj_handle *obj_hdl)
{
	struct vfs_fsal_obj_handle *myself;

	myself = container_of(obj_hdl, struct vfs_fsal_obj_handle, obj_handle);

	return myself->u.file.openflags;
}

/* vfs_read
 * concurrency (locks) is managed in cache_inode_*
 */

fsal_status_t vfs_read(struct fsal_obj_handle *obj_hdl,
		       uint64_t offset,
		       size_t buffer_size, void *buffer, size_t *read_amount,
		       bool *end_of_file)
{
	struct vfs_fsal_obj_handle *myself;
	ssize_t nb_read;
	fsal_errors_t fsal_error = ERR_FSAL_NO_ERROR;
	int retval = 0;

	myself = container_of(obj_hdl, struct vfs_fsal_obj_handle, obj_handle);

	if (obj_hdl->fsal != obj_hdl->fs->fsal) {
		LogDebug(COMPONENT_FSAL,
			 "FSAL %s operation for handle belonging to FSAL %s, return EXDEV",
			 obj_hdl->fsal->name, obj_hdl->fs->fsal->name);
		retval = EXDEV;
		fsal_error = posix2fsal_error(retval);
		return fsalstat(fsal_error, retval);
	}

	/* Take read lock on object to protect file descriptor. */
	PTHREAD_RWLOCK_rdlock(&obj_hdl->lock);

	assert(myself->u.file.fd >= 0
	       && myself->u.file.openflags != FSAL_O_CLOSED);

	nb_read = pread(myself->u.file.fd, buffer, buffer_size, offset);

	if (offset == -1 || nb_read == -1) {
		retval = errno;
		fsal_error = posix2fsal_error(retval);
		goto out;
	}

	*read_amount = nb_read;

	/* dual eof condition */
	*end_of_file = ((nb_read == 0) /* most clients */ ||	/* ESXi */
			(((offset + nb_read) >= myself->attributes.filesize)))
	    ? true : false;

 out:

	PTHREAD_RWLOCK_unlock(&obj_hdl->lock);

	return fsalstat(fsal_error, retval);
}

/* vfs_write
 * concurrency (locks) is managed in cache_inode_*
 */

fsal_status_t vfs_write(struct fsal_obj_handle *obj_hdl,
			uint64_t offset,
			size_t buffer_size, void *buffer, size_t *write_amount,
			bool *fsal_stable)
{
	struct vfs_fsal_obj_handle *myself;
	ssize_t nb_written;
	fsal_errors_t fsal_error = ERR_FSAL_NO_ERROR;
	int retval = 0;

	myself = container_of(obj_hdl, struct vfs_fsal_obj_handle, obj_handle);

	if (obj_hdl->fsal != obj_hdl->fs->fsal) {
		LogDebug(COMPONENT_FSAL,
			 "FSAL %s operation for handle belonging to FSAL %s, return EXDEV",
			 obj_hdl->fsal->name, obj_hdl->fs->fsal->name);
		retval = EXDEV;
		fsal_error = posix2fsal_error(retval);
		return fsalstat(fsal_error, retval);
	}

	/* Take read lock on object to protect file descriptor. */
	PTHREAD_RWLOCK_rdlock(&obj_hdl->lock);

	assert(myself->u.file.fd >= 0
	       && myself->u.file.openflags != FSAL_O_CLOSED);

	if (offset == SIZE_MAX) {
		offset = myself->attributes.filesize;
		LogDebug(COMPONENT_FSAL, "Appending file from %zu", offset);
	}

	fsal_set_credentials(op_ctx->creds);
	nb_written = pwrite(myself->u.file.fd, buffer, buffer_size, offset);

	if (offset == -1 || nb_written == -1) {
		retval = errno;
		fsal_error = posix2fsal_error(retval);
		goto out;
	}

	*write_amount = nb_written;

	/* attempt stability */
	if (fsal_stable != NULL && *fsal_stable) {
		retval = fsync(myself->u.file.fd);
		if (retval == -1) {
			retval = errno;
			fsal_error = posix2fsal_error(retval);
		}
		*fsal_stable = true;
	}

 out:

	PTHREAD_RWLOCK_unlock(&obj_hdl->lock);

	fsal_restore_ganesha_credentials();
	return fsalstat(fsal_error, retval);
}

fsal_status_t vfs_copy(struct fsal_obj_handle *src_hdl, uint64_t src_offset,
		       struct fsal_obj_handle *dst_hdl, uint64_t dst_offset,
		       uint64_t count, uint64_t *copied)
{
	struct vfs_fsal_obj_handle *src_vfs;
	struct vfs_fsal_obj_handle *dst_vfs;
	fsal_status_t st = {0, 0};
	ssize_t ret;

	LogDebug(COMPONENT_FSAL, "vfs_copy: server-side copying");
	src_vfs = container_of(src_hdl, struct vfs_fsal_obj_handle, obj_handle);
	dst_vfs = container_of(dst_hdl, struct vfs_fsal_obj_handle, obj_handle);
	if ((size_t)src_vfs < (size_t)dst_vfs) {
		PTHREAD_RWLOCK_rdlock(&src_hdl->lock);
		PTHREAD_RWLOCK_rdlock(&dst_hdl->lock);
	} else {
		PTHREAD_RWLOCK_rdlock(&dst_hdl->lock);
		PTHREAD_RWLOCK_rdlock(&src_hdl->lock);
	}

	ret = splice_fcopy(src_vfs->u.file.fd, src_offset, dst_vfs->u.file.fd,
			   dst_offset, count);
	if (ret < 0) {
		LogMajor(COMPONENT_FSAL, "Failed to copy file: (%s)",
			 strerror(-ret));
		st = fsalstat(-ret, 0);
		*copied = 0;
	}
	*copied = ret;
	LogDebug(COMPONENT_FSAL, "vfs_copy: %zu of %zu bytes copied", *copied,
		 count);

	if ((size_t)src_vfs < (size_t)dst_vfs) {
		PTHREAD_RWLOCK_unlock(&dst_hdl->lock);
		PTHREAD_RWLOCK_unlock(&src_hdl->lock);
	} else {
		PTHREAD_RWLOCK_unlock(&src_hdl->lock);
		PTHREAD_RWLOCK_unlock(&dst_hdl->lock);
	}

	return st;
}

/* vfs_commit
 * Commit a file range to storage.
 * for right now, fsync will have to do.
 */

fsal_status_t vfs_commit(struct fsal_obj_handle *obj_hdl,	/* sync */
			 off_t offset, size_t len)
{
	struct vfs_fsal_obj_handle *myself;
	fsal_errors_t fsal_error = ERR_FSAL_NO_ERROR;
	int retval = 0;

	myself = container_of(obj_hdl, struct vfs_fsal_obj_handle, obj_handle);

	if (obj_hdl->fsal != obj_hdl->fs->fsal) {
		LogDebug(COMPONENT_FSAL,
			 "FSAL %s operation for handle belonging to FSAL %s, return EXDEV",
			 obj_hdl->fsal->name, obj_hdl->fs->fsal->name);
		retval = EXDEV;
		fsal_error = posix2fsal_error(retval);
		return fsalstat(fsal_error, retval);
	}

	/* Take read lock on object to protect file descriptor. */
	PTHREAD_RWLOCK_rdlock(&obj_hdl->lock);

	assert(myself->u.file.fd >= 0
	       && myself->u.file.openflags != FSAL_O_CLOSED);

	retval = fsync(myself->u.file.fd);
	if (retval == -1) {
		retval = errno;
		fsal_error = posix2fsal_error(retval);
	}

	PTHREAD_RWLOCK_unlock(&obj_hdl->lock);

	return fsalstat(fsal_error, retval);
}

/* vfs_lock_op
 * lock a region of the file
 * throw an error if the fd is not open.  The old fsal didn't
 * check this.
 */

fsal_status_t vfs_lock_op(struct fsal_obj_handle *obj_hdl,
			  void *p_owner,
			  fsal_lock_op_t lock_op,
			  fsal_lock_param_t *request_lock,
			  fsal_lock_param_t *conflicting_lock)
{
	struct vfs_fsal_obj_handle *myself;
	struct flock lock_args;
	int fcntl_comm;
	fsal_errors_t fsal_error = ERR_FSAL_NO_ERROR;
	int retval = 0;

	myself = container_of(obj_hdl, struct vfs_fsal_obj_handle, obj_handle);

	if (obj_hdl->fsal != obj_hdl->fs->fsal) {
		LogDebug(COMPONENT_FSAL,
			 "FSAL %s operation for handle belonging to FSAL %s, return EXDEV",
			 obj_hdl->fsal->name, obj_hdl->fs->fsal->name);
		retval = EXDEV;
		fsal_error = posix2fsal_error(retval);
		return fsalstat(fsal_error, retval);
	}

	/* Take read lock on object to protect file descriptor. */
	PTHREAD_RWLOCK_rdlock(&obj_hdl->lock);

	if (myself->u.file.fd < 0 ||
	    myself->u.file.openflags == FSAL_O_CLOSED) {
		LogDebug(COMPONENT_FSAL,
			 "Attempting to lock with no file descriptor open");
		fsal_error = ERR_FSAL_FAULT;
		goto out;
	}
	if (p_owner != NULL) {
		fsal_error = ERR_FSAL_NOTSUPP;
		goto out;
	}
	LogFullDebug(COMPONENT_FSAL,
		     "Locking: op:%d type:%d start:%" PRIu64 " length:%" PRIu64,
		     lock_op, request_lock->lock_type, request_lock->lock_start,
		     request_lock->lock_length);
	if (lock_op == FSAL_OP_LOCKT) {
		fcntl_comm = F_GETLK;
	} else if (lock_op == FSAL_OP_LOCK || lock_op == FSAL_OP_UNLOCK) {
		fcntl_comm = F_SETLK;
	} else {
		LogDebug(COMPONENT_FSAL,
			 "ERROR: Lock operation requested was not TEST, READ, or WRITE.");
		fsal_error = ERR_FSAL_NOTSUPP;
		goto out;
	}

	if (request_lock->lock_type == FSAL_LOCK_R) {
		lock_args.l_type = F_RDLCK;
	} else if (request_lock->lock_type == FSAL_LOCK_W) {
		lock_args.l_type = F_WRLCK;
	} else {
		LogDebug(COMPONENT_FSAL,
			 "ERROR: The requested lock type was not read or write.");
		fsal_error = ERR_FSAL_NOTSUPP;
		goto out;
	}

	if (lock_op == FSAL_OP_UNLOCK)
		lock_args.l_type = F_UNLCK;

	lock_args.l_len = request_lock->lock_length;
	lock_args.l_start = request_lock->lock_start;
	lock_args.l_whence = SEEK_SET;

	/* flock.l_len being signed long integer, larger lock ranges may
	 * get mapped to negative values. As per 'man 3 fcntl', posix
	 * locks can accept negative l_len values which may lead to
	 * unlocking an unintended range. Better bail out to prevent that.
	 */
	if (lock_args.l_len < 0) {
		LogCrit(COMPONENT_FSAL,
			"The requested lock length is out of range- lock_args.l_len(%ld), request_lock_length(%"
			PRIu64 ")",
			lock_args.l_len, request_lock->lock_length);
		fsal_error = ERR_FSAL_BAD_RANGE;
		goto out;
	}

	errno = 0;
	retval = fcntl(myself->u.file.fd, fcntl_comm, &lock_args);
	if (retval && lock_op == FSAL_OP_LOCK) {
		retval = errno;
		if (conflicting_lock != NULL) {
			fcntl_comm = F_GETLK;
			if (fcntl(myself->u.file.fd, fcntl_comm, &lock_args)) {
				retval = errno;	/* we lose the inital error */
				LogCrit(COMPONENT_FSAL,
					"After failing a lock request, I couldn't even get the details of who owns the lock.");
				fsal_error = posix2fsal_error(retval);
				goto out;
			}
			if (conflicting_lock != NULL) {
				conflicting_lock->lock_length = lock_args.l_len;
				conflicting_lock->lock_start =
				    lock_args.l_start;
				conflicting_lock->lock_type = lock_args.l_type;
			}
		}
		fsal_error = posix2fsal_error(retval);
		goto out;
	}

	/* F_UNLCK is returned then the tested operation would be possible. */
	if (conflicting_lock != NULL) {
		if (lock_op == FSAL_OP_LOCKT && lock_args.l_type != F_UNLCK) {
			conflicting_lock->lock_length = lock_args.l_len;
			conflicting_lock->lock_start = lock_args.l_start;
			conflicting_lock->lock_type = lock_args.l_type;
		} else {
			conflicting_lock->lock_length = 0;
			conflicting_lock->lock_start = 0;
			conflicting_lock->lock_type = FSAL_NO_LOCK;
		}
	}
 out:

	PTHREAD_RWLOCK_unlock(&obj_hdl->lock);

	return fsalstat(fsal_error, retval);
}

/* vfs_close
 * Close the file if it is still open.
 * Yes, we ignor lock status.  Closing a file in POSIX
 * releases all locks but that is state and cache inode's problem.
 */

fsal_status_t vfs_close(struct fsal_obj_handle *obj_hdl)
{
	struct vfs_fsal_obj_handle *myself;
	fsal_errors_t fsal_error = ERR_FSAL_NO_ERROR;
	int retval = 0;

	assert(obj_hdl->type == REGULAR_FILE);
	myself = container_of(obj_hdl, struct vfs_fsal_obj_handle, obj_handle);

	if (obj_hdl->fsal != obj_hdl->fs->fsal) {
		LogDebug(COMPONENT_FSAL,
			 "FSAL %s operation for handle belonging to FSAL %s, return EXDEV",
			 obj_hdl->fsal->name, obj_hdl->fs->fsal->name);
		retval = EXDEV;
		fsal_error = posix2fsal_error(retval);
		return fsalstat(fsal_error, retval);
	}

	/* Take write lock on object to protect file descriptor. */
	PTHREAD_RWLOCK_wrlock(&obj_hdl->lock);

	if (myself->u.file.fd >= 0 &&
	    myself->u.file.openflags != FSAL_O_CLOSED) {
		retval = close(myself->u.file.fd);
		if (retval < 0) {
			retval = errno;
			fsal_error = posix2fsal_error(retval);
		}
		myself->u.file.fd = -1;
		myself->u.file.openflags = FSAL_O_CLOSED;
	}

	PTHREAD_RWLOCK_unlock(&obj_hdl->lock);

	return fsalstat(fsal_error, retval);
}

/* vfs_lru_cleanup
 * free non-essential resources at the request of cache inode's
 * LRU processing identifying this handle as stale enough for resource
 * trimming.
 */

fsal_status_t vfs_lru_cleanup(struct fsal_obj_handle *obj_hdl,
			      lru_actions_t requests)
{
	struct vfs_fsal_obj_handle *myself;
	fsal_errors_t fsal_error = ERR_FSAL_NO_ERROR;
	int retval = 0;

	myself = container_of(obj_hdl, struct vfs_fsal_obj_handle, obj_handle);

	if (obj_hdl->fsal != obj_hdl->fs->fsal) {
		LogDebug(COMPONENT_FSAL,
			 "FSAL %s operation for handle belonging to FSAL %s, return EXDEV",
			 obj_hdl->fsal->name, obj_hdl->fs->fsal->name);
		retval = EXDEV;
		fsal_error = posix2fsal_error(retval);
		return fsalstat(fsal_error, retval);
	}

	/* Take read lock on object to protect file descriptor. */
	PTHREAD_RWLOCK_rdlock(&obj_hdl->lock);

	if (obj_hdl->type == REGULAR_FILE && myself->u.file.fd >= 0) {
		retval = close(myself->u.file.fd);
		myself->u.file.fd = -1;
		myself->u.file.openflags = FSAL_O_CLOSED;
	}
	if (retval == -1) {
		retval = errno;
		fsal_error = posix2fsal_error(retval);
	}

	PTHREAD_RWLOCK_unlock(&obj_hdl->lock);

	return fsalstat(fsal_error, retval);
}
