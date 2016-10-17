/*
 * vim:noexpandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright 2015-2016 Red Hat, Inc. and/or its affiliates.
 * Author: Daniel Gryniewicz <dang@redhat.com>
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
 * Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 */

/**
 * @addtogroup FSAL_MDCACHE
 * @{
 */

/**
 * @file  main.c
 * @brief FSAL export functions
 */

#include "config.h"

#include "fsal.h"
#include <libgen.h>		/* used for 'dirname' */
#include <pthread.h>
#include <string.h>
#include <sys/types.h>
#include <os/mntent.h>
#include <os/quota.h>
#include <dlfcn.h>
#include "gsh_list.h"
#include "config_parsing.h"
#include "fsal_convert.h"
#include "FSAL/fsal_commonlib.h"
#include "FSAL/fsal_config.h"
#include "mdcache_lru.h"
#include "nfs_exports.h"
#include "export_mgr.h"

/*
 * helpers to/from other NULL objects
 */

struct fsal_staticfsinfo_t *mdcache_staticinfo(struct fsal_module *hdl);

/*
 * export object methods
 */

/**
 * @brief Return the name of the sub-FSAL
 *
 * For MDCACHE, we append "/MDC" onto the name.
 *
 * @param[in] exp_hdl	Our export handle
 * @return Name of sub-FSAL
 */
static const char *mdcache_get_name(struct fsal_export *exp_hdl)
{
	struct mdcache_fsal_export *exp = mdc_cur_export();

	return exp->name;
}

/**
 * @brief Un-export an MDCACHE export
 *
 * Clean up all the cache entries on this export.
 *
 * @param[in] exp_hdl	Export to unexport
 */
static void mdcache_unexport(struct fsal_export *exp_hdl)
{
	struct mdcache_fsal_export *exp = mdc_export(exp_hdl);
	struct fsal_export *sub_export = exp->export.sub_export;
	mdcache_entry_t *entry;
	struct entry_export_map *expmap;
	fsal_status_t status;

	/* First unexport for the sub-FSAL */
	subcall_raw(exp,
		sub_export->exp_ops.unexport(sub_export)
	);

	/* Next, clean up our cache entries on the export */
	while (true) {
		PTHREAD_RWLOCK_rdlock(&exp->mdc_exp_lock);
		expmap = glist_first_entry(&exp->entry_list,
					   struct entry_export_map,
					   entry_per_export);

		if (unlikely(expmap == NULL)) {
			PTHREAD_RWLOCK_unlock(&exp->mdc_exp_lock);
			break;
		}

		entry = expmap->entry;
		/* Get a ref across cleanup */
		status = mdcache_get(entry);
		PTHREAD_RWLOCK_unlock(&exp->mdc_exp_lock);

		if (FSAL_IS_ERROR(status)) {
			/* Entry was stale; skip it */
			continue;
		}


		/* Must get attr_lock before mdc_exp_lock */
		PTHREAD_RWLOCK_wrlock(&entry->attr_lock);
		PTHREAD_RWLOCK_wrlock(&exp->mdc_exp_lock);

		mdc_remove_export_map(expmap);

		expmap = glist_first_entry(&entry->export_list,
					   struct entry_export_map,
					   export_per_entry);
		if (expmap == NULL) {
			/* Clear out first export pointer */
			atomic_store_voidptr(&entry->first_export, NULL);
			/* We must not hold entry->attr_lock across
			 * try_cleanup_push (LRU lane lock order) */
			PTHREAD_RWLOCK_unlock(&exp->mdc_exp_lock);
			PTHREAD_RWLOCK_unlock(&entry->attr_lock);

			/* There are no exports referencing this entry, attempt
			 * to push it to cleanup queue.  */
			mdcache_lru_cleanup_try_push(entry);
		} else {
			/* Make sure first export pointer is still valid */
			atomic_store_voidptr(&entry->first_export,
					     expmap->export);

			PTHREAD_RWLOCK_unlock(&exp->mdc_exp_lock);
			PTHREAD_RWLOCK_unlock(&entry->attr_lock);
		}

		/* Release above ref */
		mdcache_put(entry);
	};
}

/**
 * @brief Release an MDCACHE export
 *
 * @param[in] exp_hdl	Export to release
 */
static void mdcache_exp_release(struct fsal_export *exp_hdl)
{
	struct mdcache_fsal_export *exp = mdc_export(exp_hdl);
	struct fsal_export *sub_export = exp->export.sub_export;
	struct fsal_module *fsal_hdl;

	fsal_hdl = sub_export->fsal;

	/* Release the sub_export */
	subcall_shutdown_raw(exp,
		sub_export->exp_ops.release(sub_export)
	);

	fsal_put(fsal_hdl);

	fsal_detach_export(exp_hdl->fsal, &exp_hdl->exports);
	free_export_ops(exp_hdl);

	gsh_free(exp->name);

	gsh_free(exp);	/* elvis has left the building */
}

/**
 * @brief Get FS information
 *
 * Pass through to underlying FSAL.
 *
 * Note dang: Should this gather info about MDCACHE?
 *
 * @param[in] exp_hdl	Export to operate on
 * @param[in] obj_hdl	Object to operate on
 * @param[out] infop	Output information on success
 * @return FSAL status
 */
static fsal_status_t mdcache_get_dynamic_info(struct fsal_export *exp_hdl,
					      struct fsal_obj_handle *obj_hdl,
					      fsal_dynamicfsinfo_t *infop)
{
	struct mdcache_fsal_export *exp = mdc_export(exp_hdl);
	struct fsal_export *sub_export = exp->export.sub_export;
	mdcache_entry_t *entry =
		container_of(obj_hdl, mdcache_entry_t, obj_handle);
	fsal_status_t status;

	subcall_raw(exp,
		status = sub_export->exp_ops.get_fs_dynamic_info(
			sub_export, entry->sub_handle, infop)
	       );

	return status;
}

/**
 * @brief See if a feature is supported
 *
 * For the moment, MDCACHE supports no features, so just pass through to the
 * base FSAL.
 *
 * @param[in] exp_hdl	Export to check
 * @param[in] option	Option to check for support
 * @return true if supported, false otherwise
 */
static bool mdcache_fs_supports(struct fsal_export *exp_hdl,
				fsal_fsinfo_options_t option)
{
	struct mdcache_fsal_export *exp = mdc_export(exp_hdl);
	struct fsal_export *sub_export = exp->export.sub_export;
	bool result;

	subcall_raw(exp,
		result = sub_export->exp_ops.fs_supports(sub_export, option)
	       );

	return result;
}

/**
 * @brief Find the maximum supported file size
 *
 * MDCACHE only caches metadata, so it imposes no restrictions itself.
 *
 * @param[in] exp_hdl	Export to query
 * @return Max size in bytes
 */
static uint64_t mdcache_fs_maxfilesize(struct fsal_export *exp_hdl)
{
	struct mdcache_fsal_export *exp = mdc_export(exp_hdl);
	struct fsal_export *sub_export = exp->export.sub_export;
	uint64_t result;

	subcall_raw(exp,
		result = sub_export->exp_ops.fs_maxfilesize(sub_export)
	       );

	return result;
}

/**
 * @brief Get the maximum supported read size
 *
 * MDCACHE only caches metadata, so it imposes no restrictions itself.
 *
 * @param[in] exp_hdl	Export to query
 * @return Max size in bytes
 */
static uint32_t mdcache_fs_maxread(struct fsal_export *exp_hdl)
{
	struct mdcache_fsal_export *exp = mdc_export(exp_hdl);
	struct fsal_export *sub_export = exp->export.sub_export;
	uint32_t result;

	subcall_raw(exp,
		result = sub_export->exp_ops.fs_maxread(sub_export)
	       );

	return result;
}

/**
 * @brief Get the maximum supported write size
 *
 * MDCACHE only caches metadata, so it imposes no restrictions itself.
 *
 * @param[in] exp_hdl	Export to query
 * @return Max size in bytes
 */
static uint32_t mdcache_fs_maxwrite(struct fsal_export *exp_hdl)
{
	struct mdcache_fsal_export *exp = mdc_export(exp_hdl);
	struct fsal_export *sub_export = exp->export.sub_export;
	uint32_t result;

	subcall_raw(exp,
		result = sub_export->exp_ops.fs_maxwrite(sub_export)
	       );

	return result;
}

/**
 * @brief Get the maximum supported link count
 *
 * MDCACHE only caches metadata, so it imposes no restrictions itself.
 *
 * @param[in] exp_hdl	Export to query
 * @return Max number of links to a file
 */
static uint32_t mdcache_fs_maxlink(struct fsal_export *exp_hdl)
{
	struct mdcache_fsal_export *exp = mdc_export(exp_hdl);
	struct fsal_export *sub_export = exp->export.sub_export;
	uint32_t result;

	subcall_raw(exp,
		result = sub_export->exp_ops.fs_maxlink(sub_export)
	       );

	return result;
}

/**
 * @brief Get the maximum supported name length
 *
 * MDCACHE only caches metadata, so it imposes no restrictions itself.
 *
 * @param[in] exp_hdl	Export to query
 * @return Max length of name in bytes
 */
static uint32_t mdcache_fs_maxnamelen(struct fsal_export *exp_hdl)
{
	struct mdcache_fsal_export *exp = mdc_export(exp_hdl);
	struct fsal_export *sub_export = exp->export.sub_export;
	uint32_t result;

	subcall_raw(exp,
		result = sub_export->exp_ops.fs_maxnamelen(sub_export)
	       );

	return result;
}

/**
 * @brief Get the maximum supported name length
 *
 * MDCACHE only caches metadata, so it imposes no restrictions itself.
 *
 * @param[in] exp_hdl	Export to query
 * @return Max length of name in bytes
 */
static uint32_t mdcache_fs_maxpathlen(struct fsal_export *exp_hdl)
{
	struct mdcache_fsal_export *exp = mdc_export(exp_hdl);
	struct fsal_export *sub_export = exp->export.sub_export;
	uint32_t result;

	subcall_raw(exp,
		result = sub_export->exp_ops.fs_maxpathlen(sub_export)
	       );

	return result;
}

/**
 * @brief Get the FS lease time
 *
 * MDCACHE only caches metadata, so it imposes no restrictions itself.
 *
 * @param[in] exp_hdl	Export to query
 * @return Lease time
 */
static struct timespec mdcache_fs_lease_time(struct fsal_export *exp_hdl)
{
	struct mdcache_fsal_export *exp = mdc_export(exp_hdl);
	struct fsal_export *sub_export = exp->export.sub_export;
	struct timespec result;

	subcall_raw(exp,
		result = sub_export->exp_ops.fs_lease_time(sub_export)
	       );

	return result;
}

/**
 * @brief Get the NFSv4 ACLSUPPORT attribute
 *
 * MDCACHE does not provide or restrict ACLs
 *
 * @param[in] exp_hdl	Export to query
 * @return ACLSUPPORT
 */
static fsal_aclsupp_t mdcache_fs_acl_support(struct fsal_export *exp_hdl)
{
	struct mdcache_fsal_export *exp = mdc_export(exp_hdl);
	struct fsal_export *sub_export = exp->export.sub_export;
	fsal_aclsupp_t result;

	subcall_raw(exp,
		result = sub_export->exp_ops.fs_acl_support(sub_export)
	       );

	return result;
}

/**
 * @brief Get the list of supported attributes
 *
 * MDCACHE does not provide or restrict attributes
 *
 * @param[in] exp_hdl	Export to query
 * @return Mask of supported attributes
 */
static attrmask_t mdcache_fs_supported_attrs(struct fsal_export *exp_hdl)
{
	struct mdcache_fsal_export *exp = mdc_export(exp_hdl);
	struct fsal_export *sub_export = exp->export.sub_export;
	attrmask_t result;

	subcall_raw(exp,
		result = sub_export->exp_ops.fs_supported_attrs(sub_export)
	       );

	return result;
}

/**
 * @brief Get the configured umask on the export
 *
 * MDCACHE only caches metadata, so it imposes no restrictions itself.
 *
 * @param[in] exp_hdl	Export to query
 * @return umask value
 */
static uint32_t mdcache_fs_umask(struct fsal_export *exp_hdl)
{
	struct mdcache_fsal_export *exp = mdc_export(exp_hdl);
	struct fsal_export *sub_export = exp->export.sub_export;
	uint32_t result;

	subcall_raw(exp,
		result = sub_export->exp_ops.fs_umask(sub_export)
	       );

	return result;
}

/**
 * @brief Get the configured xattr access mask
 *
 * MDCACHE only caches metadata, so it imposes no restrictions itself.
 *
 * @param[in] exp_hdl	Export to query
 * @return POSIX access bits for xattrs
 */
static uint32_t mdcache_fs_xattr_access_rights(struct fsal_export *exp_hdl)
{
	struct mdcache_fsal_export *exp = mdc_export(exp_hdl);
	struct fsal_export *sub_export = exp->export.sub_export;
	uint32_t result;

	subcall_raw(exp,
		result = sub_export->exp_ops.fs_xattr_access_rights(sub_export)
	       );

	return result;
}

/**
 * @brief Check quota on a file
 *
 * MDCACHE only caches metadata, so it imposes no restrictions itself.
 *
 * @param[in] exp_hdl	Export to query
 * @param[in] filepath	Path to file to query
 * @param[in] quota_type	Type of quota (user or group)
 * @return FSAL status
 */
static fsal_status_t mdcache_check_quota(struct fsal_export *exp_hdl,
					 const char *filepath, int quota_type)
{
	struct mdcache_fsal_export *exp = mdc_export(exp_hdl);
	struct fsal_export *sub_export = exp->export.sub_export;
	fsal_status_t status;

	subcall_raw(exp,
		status = sub_export->exp_ops.check_quota(sub_export, filepath,
							 quota_type)
	       );

	return status;
}

/**
 * @brief Get quota information for a file
 *
 * MDCACHE only caches metadata, so it imposes no restrictions itself.
 *
 * @param[in] exp_hdl	Export to query
 * @param[in] filepath	Path to file to query
 * @param[in] quota_type	Type of quota (user or group)
 * @param[in] quota_id  Id for getting quota information
 * @param[out] pquota	Resulting quota information
 * @return FSAL status
 */
static fsal_status_t mdcache_get_quota(struct fsal_export *exp_hdl,
				       const char *filepath, int quota_type,
				       int quota_id,
				       fsal_quota_t *pquota)
{
	struct mdcache_fsal_export *exp = mdc_export(exp_hdl);
	struct fsal_export *sub_export = exp->export.sub_export;
	fsal_status_t status;

	subcall_raw(exp,
		status = sub_export->exp_ops.get_quota(sub_export, filepath,
						       quota_type, quota_id,
						       pquota));

	return status;
}

/**
 * @brief Set a quota for a file
 *
 * MDCACHE only caches metadata, so it imposes no restrictions itself.
 *
 * @param[in] exp_hdl	Export to query
 * @param[in] filepath	Path to file to query
 * @param[in] quota_type	Type of quota (user or group)
 * @param[in] quota_id  Id for which quota is set
 * @param[in] pquota	Quota information to set
 * @param[out] presquota	Quota after set
 * @return FSAL status
 */
static fsal_status_t mdcache_set_quota(struct fsal_export *exp_hdl,
				       const char *filepath, int quota_type,
				       int quota_id,
				       fsal_quota_t *pquota,
				       fsal_quota_t *presquota)
{
	struct mdcache_fsal_export *exp = mdc_export(exp_hdl);
	struct fsal_export *sub_export = exp->export.sub_export;
	fsal_status_t status;

	subcall_raw(exp,
		status = sub_export->exp_ops.set_quota(sub_export,
			filepath, quota_type, quota_id, pquota, presquota)
	       );

	return status;
}

/**
 * @brief List pNFS devices
 *
 * MDCACHE only caches metadata, pass it through
 *
 * @param[in] exp_hdl	Export to query
 * @param[in] type	Layout type for query
 * @param[in] cb	Callback for devices
 * @param[in] res	Devicelist result
 * @return NFSv4 Status
 */
static nfsstat4 mdcache_getdevicelist(struct fsal_export *exp_hdl,
					   layouttype4 type, void *opaque,
					   bool (*cb)(void *opaque,
						      const uint64_t id),
					   struct fsal_getdevicelist_res *res)
{
	struct mdcache_fsal_export *exp = mdc_export(exp_hdl);
	struct fsal_export *sub_export = exp->export.sub_export;
	nfsstat4 status;

	subcall_raw(exp,
		status = sub_export->exp_ops.getdevicelist(sub_export, type,
							   opaque, cb, res)
	       );

	return status;
}

/**
 * @brief List supported pNFS layout types
 *
 * MDCACHE only caches metadata, pass it through
 *
 * @param[in] exp_hdl	Export to query
 * @param[out] count	Number of types returned
 * @param[out] types	Layout types supported
 */
static void mdcache_fs_layouttypes(struct fsal_export *exp_hdl,
					    int32_t *count,
					    const layouttype4 **types)
{
	struct mdcache_fsal_export *exp = mdc_export(exp_hdl);
	struct fsal_export *sub_export = exp->export.sub_export;

	subcall_raw(exp,
		sub_export->exp_ops.fs_layouttypes(sub_export, count, types)
	       );
}

/**
 * @brief Get pNFS layout block size
 *
 * MDCACHE only caches metadata, pass it through
 *
 * @param[in] exp_hdl	Export to query
 * @return Number of bytes in block
 */
static uint32_t mdcache_fs_layout_blocksize(struct fsal_export *exp_hdl)
{
	struct mdcache_fsal_export *exp = mdc_export(exp_hdl);
	struct fsal_export *sub_export = exp->export.sub_export;
	uint32_t status;

	subcall_raw(exp,
		status = sub_export->exp_ops.fs_layout_blocksize(sub_export)
	       );

	return status;
}

/**
 * @brief Get pNFS maximum number of segments
 *
 * MDCACHE only caches metadata, pass it through
 *
 * @param[in] exp_hdl	Export to query
 * @return Number of segments
 */
static uint32_t mdcache_fs_maximum_segments(struct fsal_export *exp_hdl)
{
	struct mdcache_fsal_export *exp = mdc_export(exp_hdl);
	struct fsal_export *sub_export = exp->export.sub_export;
	uint32_t status;

	subcall_raw(exp,
		status = sub_export->exp_ops.fs_maximum_segments(sub_export)
	       );

	return status;
}

/**
 * @brief Get size of pNFS loc_body
 *
 * MDCACHE only caches metadata, pass it through
 *
 * @param[in] exp_hdl	Export to query
 * @return Size of loc_body
 */
static size_t mdcache_fs_loc_body_size(struct fsal_export *exp_hdl)
{
	struct mdcache_fsal_export *exp = mdc_export(exp_hdl);
	struct fsal_export *sub_export = exp->export.sub_export;
	size_t status;

	subcall_raw(exp,
		status = sub_export->exp_ops.fs_loc_body_size(sub_export)
	       );

	return status;
}

/**
 * @brief Get write verifier
 *
 * MDCACHE only caches metadata, pass it through
 *
 * @param[in] exp_hdl	Export to query
 * @param[in,out] verf_desc Address and length of verifier
 */
static void mdcache_get_write_verifier(struct fsal_export *exp_hdl,
				       struct gsh_buffdesc *verf_desc)
{
	struct mdcache_fsal_export *exp = mdc_export(exp_hdl);
	struct fsal_export *sub_export = exp->export.sub_export;

	subcall_raw(exp,
		sub_export->exp_ops.get_write_verifier(sub_export, verf_desc)
	       );
}

/**
 * @brief Decode the wire handle into something the FSAL can understand
 *
 * Wire formats are delegated to the underlying FSAL.
 *
 * @param[in] exp_hdl	Export to operate on
 * @param[in] in_type	Type of handle to extract
 * @param[in,out] fh_desc	Source/dest for extracted digest
 * @param[in] flags	Related flages (currently endian)
 * @return FSAL status
 */
static fsal_status_t mdcache_extract_handle(struct fsal_export *exp_hdl,
					    fsal_digesttype_t in_type,
					    struct gsh_buffdesc *fh_desc,
					    int flags)
{
	struct mdcache_fsal_export *exp = mdc_export(exp_hdl);
	struct fsal_export *sub_export = exp->export.sub_export;
	fsal_status_t status;

	subcall_raw(exp,
		status = sub_export->exp_ops.extract_handle(sub_export, in_type,
							    fh_desc, flags)
	       );

	return status;
}

/**
 * @brief Allocate state_t structure
 *
 * @param[in] exp_hdl	Export to operate on
 * @param[in] state_type Type of state to allocate
 * @param[in] related_state Related state if appropriate
 * @return New state structure
 */
static struct state_t *mdcache_alloc_state(struct fsal_export *exp_hdl,
					   enum state_type state_type,
					   struct state_t *related_state)
{
	struct mdcache_fsal_export *exp = mdc_export(exp_hdl);
	struct fsal_export *sub_export = exp->export.sub_export;
	struct state_t *state;

	subcall_raw(exp,
		state = sub_export->exp_ops.alloc_state(sub_export, state_type,
							related_state)
	       );

	return state;
}

/**
 * @brief Free state_t structure
 *
 * @param[in] state	State to free
 */
static void mdcache_free_state(struct state_t *state)
{
	struct mdcache_fsal_export *exp = mdc_export(state->state_exp);
	struct fsal_export *sub_export = exp->export.sub_export;

	subcall_raw(exp,
		sub_export->exp_ops.free_state(state)
	       );
}

/* mdcache_export_ops_init
 * overwrite vector entries with the methods that we support
 */

void mdcache_export_ops_init(struct export_ops *ops)
{
	ops->get_name = mdcache_get_name;
	ops->unexport = mdcache_unexport;
	ops->release = mdcache_exp_release;
	ops->lookup_path = mdcache_lookup_path;
	/* lookup_junction unimplemented because deprecated */
	ops->extract_handle = mdcache_extract_handle;
	ops->create_handle = mdcache_create_handle;
	ops->get_fs_dynamic_info = mdcache_get_dynamic_info;
	ops->fs_supports = mdcache_fs_supports;
	ops->fs_maxfilesize = mdcache_fs_maxfilesize;
	ops->fs_maxread = mdcache_fs_maxread;
	ops->fs_maxwrite = mdcache_fs_maxwrite;
	ops->fs_maxlink = mdcache_fs_maxlink;
	ops->fs_maxnamelen = mdcache_fs_maxnamelen;
	ops->fs_maxpathlen = mdcache_fs_maxpathlen;
	ops->fs_lease_time = mdcache_fs_lease_time;
	ops->fs_acl_support = mdcache_fs_acl_support;
	ops->fs_supported_attrs = mdcache_fs_supported_attrs;
	ops->fs_umask = mdcache_fs_umask;
	ops->fs_xattr_access_rights = mdcache_fs_xattr_access_rights;
	ops->check_quota = mdcache_check_quota;
	ops->get_quota = mdcache_get_quota;
	ops->set_quota = mdcache_set_quota;
	ops->getdevicelist = mdcache_getdevicelist;
	ops->fs_layouttypes = mdcache_fs_layouttypes;
	ops->fs_layout_blocksize = mdcache_fs_layout_blocksize;
	ops->fs_maximum_segments = mdcache_fs_maximum_segments;
	ops->fs_loc_body_size = mdcache_fs_loc_body_size;
	ops->get_write_verifier = mdcache_get_write_verifier;
	ops->alloc_state = mdcache_alloc_state;
	ops->free_state = mdcache_free_state;
}

struct mdcache_fsal_args {
	struct subfsal_args subfsal;
};

static struct config_item sub_fsal_params[] = {
	CONF_ITEM_STR("name", 1, 10, NULL,
		      subfsal_args, name),
	CONFIG_EOL
};

static struct config_item export_params[] = {
	CONF_ITEM_NOOP("name"),
	CONF_RELAX_BLOCK("FSAL", sub_fsal_params,
			 noop_conf_init, subfsal_commit,
			 mdcache_fsal_args, subfsal),
	CONFIG_EOL
};

static struct config_block export_param = {
	.dbus_interface_name = "org.ganesha.nfsd.config.fsal.mdcache-export%d",
	.blk_desc.name = "FSAL",
	.blk_desc.type = CONFIG_BLOCK,
	.blk_desc.u.blk.init = noop_conf_init,
	.blk_desc.u.blk.params = export_params,
	.blk_desc.u.blk.commit = noop_conf_commit
};

/**
 * @brief Initialize a MDCACHE export
 *
 * Create a MDCACHE export, wrapping it around a sub-FSAL export.  The sub-FSAL
 * export must be initialized already, as must @a mdc_up_ops
 *
 * @param[in] fsal_hdl		MDCACHE FSAL
 * @param[in] mdc_up_ops	UP ops for MDCACHE
 * @return FSAL status
 */
fsal_status_t
mdc_init_export(struct fsal_module *fsal_hdl,
		    const struct fsal_up_vector *mdc_up_ops,
		    const struct fsal_up_vector *super_up_ops)
{
	struct mdcache_fsal_export *myself;
	int namelen;
	pthread_rwlockattr_t attrs;

	myself = gsh_calloc(1, sizeof(struct mdcache_fsal_export));
	namelen = strlen(op_ctx->fsal_export->fsal->name) + 5;
	myself->name = gsh_calloc(1, namelen);

	fsal_get(op_ctx->fsal_export->fsal);
	snprintf(myself->name, namelen, "%s/MDC",
		 op_ctx->fsal_export->fsal->name);

	fsal_export_init(&myself->export);
	mdcache_export_ops_init(&myself->export.exp_ops);
	myself->super_up_ops = *super_up_ops; /* Struct copy */
	myself->up_ops = *mdc_up_ops; /* Struct copy */
	myself->up_ops.up_export = &myself->export;
	myself->export.up_ops = &myself->up_ops;
	myself->export.fsal = fsal_hdl;
	fsal_export_stack(op_ctx->fsal_export, &myself->export);

	glist_init(&myself->entry_list);
	pthread_rwlockattr_init(&attrs);
#ifdef GLIBC
	pthread_rwlockattr_setkind_np(&attrs,
		PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
#endif
	PTHREAD_RWLOCK_init(&myself->mdc_exp_lock, &attrs);

	op_ctx->fsal_export = &myself->export;
	op_ctx->fsal_module = fsal_hdl;
	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/**
 * @brief Create an export for MDCACHE
 *
 * Create the stacked export for MDCACHE to allow metadata caching on another
 * export.  Unlike other Stackable FSALs, this one is created @b after the FSAL
 * underneath.  It assumes the sub-FSAL's export is already created and
 * available via the @e fsal_export member of @link op_ctx @endlink, the same
 * way that this export is returned.
 *
 * There is currently no config; FSALs that want caching should call @ref
 * mdcache_export_init
 *
 * @param[in] fsal_hdl		FSAL module handle
 * @param[in] parse_node	Config node for export
 * @param[out] err_type		Parse errors
 * @param[in] up_ops		Upcall ops for export
 * @return FSAL status
 */
fsal_status_t
mdcache_fsal_create_export(struct fsal_module *fsal_hdl, void *parse_node,
			   struct config_error_type *err_type,
			   const struct fsal_up_vector *super_up_ops)
{
	fsal_status_t status = {0, 0};
	struct fsal_module *sub_fsal;
	struct mdcache_fsal_args mdcache_fsal;
	struct fsal_up_vector my_up_ops;
	int retval;

	/* process the sub-FSAL block to get the name of the fsal
	 * underneath us.
	 */
	retval = load_config_from_node(parse_node,
				       &export_param,
				       &mdcache_fsal,
				       true,
				       err_type);
	if (retval != 0)
		return fsalstat(ERR_FSAL_INVAL, 0);
	sub_fsal = lookup_fsal(mdcache_fsal.subfsal.name);
	if (sub_fsal == NULL) {
		LogMajor(COMPONENT_FSAL, "failed to lookup for FSAL %s",
			 mdcache_fsal.subfsal.name);
		return fsalstat(ERR_FSAL_INVAL, EINVAL);
	}

	mdcache_export_up_ops_init(&my_up_ops, super_up_ops);

	status = sub_fsal->m_ops.create_export(sub_fsal,
						 parse_node,
						 err_type,
						 &my_up_ops);
	if (FSAL_IS_ERROR(status)) {
		LogMajor(COMPONENT_FSAL,
			 "Failed to call create_export on underlying FSAL %s",
			 mdcache_fsal.subfsal.name);
		fsal_put(sub_fsal);
		return status;
	}

	/* Wrap sub export with MDCACHE export */
	status = mdc_init_export(fsal_hdl, &my_up_ops, super_up_ops);
	/* mdc_init_export took a ref on sub_fsal */
	fsal_put(sub_fsal);

	return status;
}

/** @} */
