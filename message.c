/*
 * Copyright (C) 2013 Kay Sievers
 * Copyright (C) 2013 Greg Kroah-Hartman <gregkh@linuxfoundation.org>
 * Copyright (C) 2013 Daniel Mack <daniel@zonque.org>
 * Copyright (C) 2013 Linux Foundation
 *
 * kdbus is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the
 * Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */

#define pr_fmt(fmt)	KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/device.h>
#include <linux/idr.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/mutex.h>
#include <linux/init.h>
#include <linux/poll.h>
#include <linux/file.h>
#include <linux/cgroup.h>
#include <linux/cred.h>
#include <linux/capability.h>
#include <linux/sizes.h>

#include "message.h"
#include "connection.h"
#include "bus.h"
#include "ep.h"
#include "policy.h"
#include "names.h"
#include "match.h"

#define KDBUS_MSG_HEADER_SIZE offsetof(struct kdbus_msg, items)
#define KDBUS_KMSG_HEADER_SIZE offsetof(struct kdbus_kmsg, msg)

static void kdbus_msg_dump(const struct kdbus_msg *msg);

static void kdbus_kmsg_free(struct kdbus_kmsg *kmsg)
{
	size_t size = 0;

	if (kmsg->fds_fp) {
		unsigned int i;

		for (i = 0; i < kmsg->fds_count; i++)
			fput(kmsg->fds_fp[i]);
		size += kmsg->fds_count * sizeof(struct file *);
		kfree(kmsg->fds_fp);
	}

	if (kmsg->fds) {
		size += KDBUS_ITEM_HEADER_SIZE + (kmsg->fds_count * sizeof(int));
		kfree(kmsg->fds);
	}

	if (kmsg->meta) {
		size += kmsg->meta_allocated_size;
		kfree(kmsg->meta);
	}

	if (kmsg->vecs) {
		size += kmsg->vecs_size;
		kfree(kmsg->vecs);
	}

	size += kmsg->msg.size + KDBUS_KMSG_HEADER_SIZE;
	kdbus_conn_sub_size_allocation(kmsg->conn_src, size);

	kfree(kmsg);
}

static void __kdbus_kmsg_free(struct kref *kref)
{
	struct kdbus_kmsg *kmsg = container_of(kref, struct kdbus_kmsg, kref);

	return kdbus_kmsg_free(kmsg);
}

void kdbus_kmsg_unref(struct kdbus_kmsg *kmsg)
{
	kref_put(&kmsg->kref, __kdbus_kmsg_free);
}

static struct kdbus_kmsg *kdbus_kmsg_ref(struct kdbus_kmsg *kmsg)
{
	kref_get(&kmsg->kref);
	return kmsg;
}

int kdbus_kmsg_new(size_t extra_size, struct kdbus_kmsg **m)
{
	size_t size = sizeof(struct kdbus_kmsg) + KDBUS_ITEM_SIZE(extra_size);
	struct kdbus_kmsg *kmsg;

	kmsg = kzalloc(size, GFP_KERNEL);
	if (!kmsg)
		return -ENOMEM;

	kref_init(&kmsg->kref);

	kmsg->msg.size = size - KDBUS_KMSG_HEADER_SIZE;
	kmsg->msg.items[0].size = KDBUS_ITEM_SIZE(extra_size);

	*m = kmsg;
	return 0;
}

static int kdbus_msg_scan_items(struct kdbus_conn *conn, struct kdbus_kmsg *kmsg)
{
	const struct kdbus_msg *msg = &kmsg->msg;
	const struct kdbus_item *item;
	unsigned int num_items = 0;
	unsigned int num_vecs = 0;
	unsigned int num_fds = 0;
	size_t vecs_size = 0;
	bool needs_vec = false;
	bool has_fds = false;
	bool has_name = false;
	bool has_bloom = false;

	KDBUS_ITEM_FOREACH_VALIDATE(item, msg) {
		/* empty data records are invalid */
		if (item->size <= KDBUS_ITEM_HEADER_SIZE)
			return -EINVAL;

		if (++num_items > KDBUS_MSG_MAX_ITEMS)
			return -E2BIG;

		switch (item->type) {
		case KDBUS_MSG_PAYLOAD:
			break;

		case KDBUS_MSG_PAYLOAD_VEC:
			if (item->size != KDBUS_ITEM_HEADER_SIZE + sizeof(struct kdbus_vec))
				return -EINVAL;

			if (++num_vecs > KDBUS_MSG_MAX_PAYLOAD_VECS)
				return -E2BIG;

			if (item->vec.flags & KDBUS_VEC_ALIGNED) {
				/* enforce page alignment and page granularity */
				if (!KDBUS_IS_ALIGNED_PAGE(item->vec.address) ||
				    !KDBUS_IS_ALIGNED_PAGE(item->vec.size))
					return -EFAULT;

				/* we always deliver aligned data as PAYLOAD_VEC */
				needs_vec = true;
			}

			vecs_size += KDBUS_ALIGN8(item->vec.size);
			if (vecs_size > KDBUS_MSG_MAX_PAYLOAD_SIZE)
				return -EMSGSIZE;
			break;

		case KDBUS_MSG_UNIX_FDS:
			/* do not allow multiple fd arrays */
			if (has_fds)
				return -EEXIST;
			has_fds = true;

			/* do not allow to broadcast file descriptors */
			if (msg->dst_id == KDBUS_DST_ID_BROADCAST)
				return -ENOTUNIQ;

			num_fds = (item->size - KDBUS_ITEM_HEADER_SIZE) / sizeof(int);
			if (num_fds > KDBUS_MSG_MAX_FDS)
				return -EMFILE;
			break;

		case KDBUS_MSG_BLOOM:
			/* do not allow multiple bloom filters */
			if (has_bloom)
				return -EEXIST;
			has_bloom = true;

			/* bloom filters are only for broadcast messages */
			if (msg->dst_id != KDBUS_DST_ID_BROADCAST)
				return -EBADMSG;

			/* allow only bloom sizes of a multiple of 64bit */
			if (!KDBUS_IS_ALIGNED8(item->size - KDBUS_ITEM_HEADER_SIZE))
				return -EFAULT;

			/* do not allow mismatching bloom filter sizes */
			if (item->size - KDBUS_ITEM_HEADER_SIZE != conn->ep->bus->bloom_size)
				return -EDOM;
			break;

		case KDBUS_MSG_DST_NAME:
			/* do not allow multiple names */
			if (has_name)
				return -EEXIST;
			has_name = true;

			/* enforce NUL-terminated strings */
			if (!kdbus_validate_nul(item->str, item->size - KDBUS_ITEM_HEADER_SIZE))
				return -EINVAL;

			if (!kdbus_name_is_valid(item->str))
				return -EINVAL;
			break;

		default:
			return -ENOTSUPP;
		}
	}

	/* validate correct padding and size values to match the overall size */
	if ((char *)item - ((char *)msg + msg->size) >= 8)
		return -EINVAL;

	/* name is needed for broadcast */
	if (msg->dst_id == KDBUS_DST_ID_WELL_KNOWN_NAME && !has_name)
		return -EDESTADDRREQ;

	/* name and ID should not be given at the same time */
	if (msg->dst_id > KDBUS_DST_ID_WELL_KNOWN_NAME &&
	    msg->dst_id < KDBUS_DST_ID_BROADCAST && has_name)
		return -EBADMSG;

	/* broadcast messages require a bloom filter */
	if (msg->dst_id == KDBUS_DST_ID_BROADCAST && !has_bloom)
		return -EBADMSG;

	/* bloom filters are for undirected messages only */
	if (has_name && has_bloom)
		return -EBADMSG;

	/* allocate array for file descriptors */
	if (has_fds) {
		size_t size;
		unsigned int i;
		int ret;

		size = num_fds * sizeof(struct file *);
		ret = kdbus_conn_add_size_allocation(conn, size);
		if (ret < 0)
			return ret;

		kmsg->fds_fp = kzalloc(size, GFP_KERNEL);
		if (!kmsg->fds_fp)
			return -ENOMEM;

		size = KDBUS_ITEM_HEADER_SIZE + (num_fds * sizeof(int));
		ret = kdbus_conn_add_size_allocation(conn, size);
		if (ret < 0)
			return ret;

		kmsg->fds = kmalloc(size, GFP_KERNEL);
		if (!kmsg->fds)
			return -ENOMEM;
		for (i = 0; i < num_fds; i++)
			kmsg->fds->fds[i] = -1;
	}

	/* if we have only very small PAYLOAD_VECs, they get inlined */
	if (num_vecs > 0 && !needs_vec &&
	    msg->size + vecs_size < KDBUS_MSG_MAX_INLINE_SIZE) {
		size_t size;
		int ret;

		size = (num_vecs * KDBUS_ITEM_HEADER_SIZE) + vecs_size;
		ret = kdbus_conn_add_size_allocation(conn, size);
		if (ret < 0)
			return ret;

		kmsg->vecs = kzalloc(size, GFP_KERNEL);
		if (!kmsg->vecs)
			return -ENOMEM;
		kmsg->vecs_size = size;
	}

	return 0;
}

static int kdbus_inline_user_vec(struct kdbus_kmsg *kmsg,
				 struct kdbus_item *next,
				 const struct kdbus_item *item)
{
	void __user *user_addr;

	user_addr = (void __user *)(uintptr_t)item->vec.address;
	if (copy_from_user(next->data, user_addr, item->vec.size))
		return -EFAULT;

	next->type = KDBUS_MSG_PAYLOAD;
	next->size = KDBUS_ITEM_HEADER_SIZE + item->vec.size;

	return 0;
}

/*
 * Grab and keep references to passed files descriptors, to install
 * them in the receiving process at message delivery.
 */
static int kdbus_copy_user_fds(struct kdbus_kmsg *kmsg,
			       const struct kdbus_item *item)
{
	unsigned int i;
	unsigned int count;

	count = (item->size - KDBUS_ITEM_HEADER_SIZE) / sizeof(int);
	for (i = 0; i < count; i++) {
		struct file *fp;

		fp = fget(item->fds[i]);
		if (!fp)
			goto unwind;

		kmsg->fds_fp[kmsg->fds_count++] = fp;
	}

	return 0;

unwind:
	while (i >= 0) {
		fput(kmsg->fds_fp[i]);
		kmsg->fds_fp[i] = NULL;
		i--;
	}

	kmsg->fds_count = 0;
	return -EBADF;
}

/*
 * Check the validity of a message. The general layout of the received message
 * is not altered before it is delivered.
 */
int kdbus_kmsg_new_from_user(struct kdbus_conn *conn, void __user *buf,
			     struct kdbus_kmsg **m)
{
	struct kdbus_kmsg *kmsg;
	const struct kdbus_item *item;
	struct kdbus_item *vecs_next;
	u64 size, alloc_size;
	int ret;

	if (!KDBUS_IS_ALIGNED8((unsigned long)buf))
		return -EFAULT;

	if (kdbus_size_get_user(size, buf, struct kdbus_msg))
		return -EFAULT;

	if (size < sizeof(struct kdbus_msg) || size > KDBUS_MSG_MAX_SIZE)
		return -EMSGSIZE;

	alloc_size = size + KDBUS_KMSG_HEADER_SIZE;

	kmsg = kmalloc(alloc_size, GFP_KERNEL);
	if (!kmsg)
		return -ENOMEM;
	memset(kmsg, 0, KDBUS_KMSG_HEADER_SIZE);

	ret = kdbus_conn_add_size_allocation(conn, alloc_size);
	if (ret < 0) {
		kfree(kmsg);
		return ret;
	}

	if (copy_from_user(&kmsg->msg, buf, size)) {
		ret = -EFAULT;
		goto exit_free;
	}

	/* check validity and prepare handling of data records */
	ret = kdbus_msg_scan_items(conn, kmsg);
	if (ret < 0)
		goto exit_free;

	/* fill in sender ID */
	kmsg->msg.src_id = conn->id;

	/* keep a reference to the source connection, for accounting */
	kmsg->conn_src = conn;

	/* Iterate over the items, resolve external references to data
	 * we need to pass to the receiver; ignore all items used by
	 * the sender only. */
	vecs_next = kmsg->vecs;
	KDBUS_ITEM_FOREACH(item, &kmsg->msg) {
		switch (item->type) {
		case KDBUS_MSG_PAYLOAD_VEC:
			if (!kmsg->vecs) {
				ret = -ENOSYS;
				goto exit_free;
			}

			/* convert PAYLOAD_VEC to PAYLOAD */
			ret = kdbus_inline_user_vec(kmsg, vecs_next, item);
			if (ret < 0)
				goto exit_free;
			vecs_next = KDBUS_ITEM_NEXT(vecs_next);
			break;

		case KDBUS_MSG_UNIX_FDS:
			ret = kdbus_copy_user_fds(kmsg, item);
			if (ret < 0)
				goto exit_free;
			break;

		case KDBUS_MSG_BLOOM:
			//FIXME: store in kmsg
			break;
		}
	}

	kref_init(&kmsg->kref);

	*m = kmsg;
	return 0;

exit_free:
	kdbus_kmsg_free(kmsg);
	return ret;
}

const struct kdbus_item *
kdbus_msg_get_item(const struct kdbus_msg *msg, u64 type, unsigned int index)
{
	const struct kdbus_item *item;

	KDBUS_ITEM_FOREACH(item, msg)
		if (item->type == type && index-- == 0)
			return item;

	return NULL;
}

static void __maybe_unused kdbus_msg_dump(const struct kdbus_msg *msg)
{
	const struct kdbus_item *item;

	pr_debug("msg size=%llu, flags=0x%llx, dst_id=%llu, src_id=%llu, "
		 "cookie=0x%llx payload_type=0x%llx, timeout=%llu\n",
		 (unsigned long long) msg->size,
		 (unsigned long long) msg->flags,
		 (unsigned long long) msg->dst_id,
		 (unsigned long long) msg->src_id,
		 (unsigned long long) msg->cookie,
		 (unsigned long long) msg->payload_type,
		 (unsigned long long) msg->timeout_ns);

	KDBUS_ITEM_FOREACH(item, msg)
		pr_debug("`- msg_item size=%llu, type=0x%llx\n",
			 item->size, item->type);
}

static struct kdbus_item *
kdbus_kmsg_append(struct kdbus_kmsg *kmsg, size_t extra_size)
{
	struct kdbus_item *item;
	size_t size;
	int ret;

	/* get new metadata buffer, pre-allocate at least 512 bytes */
	if (!kmsg->meta) {
		size = roundup_pow_of_two(256 + KDBUS_ALIGN8(extra_size));
		ret = kdbus_conn_add_size_allocation(kmsg->conn_src, size);
		if (ret < 0)
			return ERR_PTR(ret);

		kmsg->meta = kzalloc(size, GFP_KERNEL);
		if (!kmsg->meta)
			return ERR_PTR(-ENOMEM);

		kmsg->meta_allocated_size = size;
	}

	/* double the pre-allocated buffer size if needed */
	size = kmsg->meta_size + KDBUS_ALIGN8(extra_size);
	if (size > kmsg->meta_allocated_size) {
		size_t size_diff;
		struct kdbus_item *meta;

		size = roundup_pow_of_two(size);
		size_diff = size - kmsg->meta_allocated_size;

		ret = kdbus_conn_add_size_allocation(kmsg->conn_src, size_diff);
		if (ret < 0)
			return ERR_PTR(ret);

		pr_debug("%s: grow to size=%zu\n", __func__, size);
		meta = kmalloc(size, GFP_KERNEL);
		if (!meta)
			return ERR_PTR(-ENOMEM);

		memcpy(meta, kmsg->meta, kmsg->meta_size);
		memset((u8 *)meta + kmsg->meta_allocated_size, 0, size_diff);

		kfree(kmsg->meta);
		kmsg->meta = meta;
		kmsg->meta_allocated_size = size;

	}

	/* insert new record */
	item = (struct kdbus_item *)((u8 *)kmsg->meta + kmsg->meta_size);
	kmsg->meta_size += KDBUS_ALIGN8(extra_size);

	return item;
}

static int kdbus_kmsg_append_timestamp(struct kdbus_kmsg *kmsg, u64 *now_ns)
{
	struct kdbus_item *item;
	u64 size = KDBUS_ITEM_SIZE(sizeof(struct kdbus_timestamp));
	struct timespec ts;

	item = kdbus_kmsg_append(kmsg, size);
	if (IS_ERR(item))
		return PTR_ERR(item);

	item->type = KDBUS_MSG_TIMESTAMP;
	item->size = size;

	ktime_get_ts(&ts);
	item->timestamp.monotonic_ns = timespec_to_ns(&ts);

	ktime_get_real_ts(&ts);
	item->timestamp.realtime_ns = timespec_to_ns(&ts);

	if (now_ns)
		*now_ns = item->timestamp.monotonic_ns;

	return 0;
}

static int kdbus_kmsg_append_data(struct kdbus_kmsg *kmsg, u64 type,
				  const void *buf, size_t len)
{
	struct kdbus_item *item;
	u64 size;

	if (len == 0)
		return 0;

	size = KDBUS_ITEM_SIZE(len);
	item = kdbus_kmsg_append(kmsg, size);
	if (IS_ERR(item))
		return PTR_ERR(item);

	item->type = type;
	item->size = KDBUS_ITEM_HEADER_SIZE + len;
	memcpy(item->str, buf, len);

	return 0;
}

static int kdbus_kmsg_append_str(struct kdbus_kmsg *kmsg, u64 type,
				 const char *str)
{
	return kdbus_kmsg_append_data(kmsg, type, str, strlen(str) + 1);
}

static int kdbus_kmsg_append_src_names(struct kdbus_kmsg *kmsg,
				       struct kdbus_conn *conn)
{
	struct kdbus_name_entry *name_entry;
	struct kdbus_item *item;
	u64 pos = 0, size, strsize = 0;
	int ret = 0;

	mutex_lock(&conn->names_lock);
	list_for_each_entry(name_entry, &conn->names_list, conn_entry)
		strsize += strlen(name_entry->name) + 1;

	/* no names? then don't do anything */
	if (strsize == 0)
		goto exit_unlock;

	size = KDBUS_ITEM_SIZE(strsize);
	item = kdbus_kmsg_append(kmsg, size);
	if (IS_ERR(item)) {
		ret = PTR_ERR(item);
		goto exit_unlock;
	}

	item->type = KDBUS_MSG_SRC_NAMES;
	item->size = KDBUS_ITEM_HEADER_SIZE + strsize;

	list_for_each_entry(name_entry, &conn->names_list, conn_entry) {
		strcpy(item->data + pos, name_entry->name);
		pos += strlen(name_entry->name) + 1;
	}

exit_unlock:
	mutex_unlock(&conn->names_lock);

	return ret;
}

static int kdbus_kmsg_append_cred(struct kdbus_kmsg *kmsg,
				  const struct kdbus_creds *creds)
{
	struct kdbus_item *item;
	u64 size = KDBUS_ITEM_SIZE(sizeof(struct kdbus_creds));

	item = kdbus_kmsg_append(kmsg, size);
	if (IS_ERR(item))
		return PTR_ERR(item);

	item->type = KDBUS_MSG_SRC_CREDS;
	item->size = size;
	memcpy(&item->creds, creds, sizeof(*creds));

	return 0;
}

static int kdbus_conn_enqueue_kmsg(struct kdbus_conn *conn,
				   struct kdbus_kmsg *kmsg)
{
	struct kdbus_msg_list_entry *entry;
	int ret = 0;

	if (!conn->active)
		return -ENOTCONN;

	if (kmsg->fds && !(conn->flags & KDBUS_HELLO_ACCEPT_FD))
		return -ECOMM;

	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry)
		return -ENOMEM;

	entry->kmsg = kdbus_kmsg_ref(kmsg);
	INIT_LIST_HEAD(&entry->entry);

	mutex_lock(&conn->msg_lock);
	if (conn->msg_count > KDBUS_CONN_MAX_MSGS) {
		ret = -EXFULL;
	} else {
		list_add_tail(&entry->entry, &conn->msg_list);
		conn->msg_count++;
	}
	mutex_unlock(&conn->msg_lock);

	if (ret == 0)
		wake_up_interruptible(&conn->ep->wait);

	return ret;
}

/*
 * FIXME: dirty and unsafe version of:
 *   http://git.kernel.org/cgit/linux/kernel/git/tj/cgroup.git/commit/?h=review-task_cgroup_path_from_hierarchy
 * remove it when the above is upstream.
 */
int task_cgroup_path_from_hierarchy(struct task_struct *task, int hierarchy_id,
				    char *buf, size_t buflen)
{
	struct cg_cgroup_link {
		struct list_head cgrp_link_list;
		struct cgroup *cgrp;
		struct list_head cg_link_list;
		struct css_set *cg;
	};

	struct cgroupfs_root {
		struct super_block *sb;
		unsigned long subsys_mask;
		int hierarchy_id;
	};

	struct cg_cgroup_link *link;
	int ret = -ENOENT;

	cgroup_lock();
	list_for_each_entry(link, &current->cgroups->cg_links, cg_link_list) {
		struct cgroup* cg = link->cgrp;
		struct cgroupfs_root *root = (struct cgroupfs_root *)cg->root;

		if (root->hierarchy_id != hierarchy_id)
			continue;

		ret = cgroup_path(cg, buf, buflen);
		break;
	}
	cgroup_unlock();

	return ret;
}

static int kdbus_msg_append_for_dst(struct kdbus_kmsg *kmsg,
				    struct kdbus_conn *conn_src,
				    struct kdbus_conn *conn_dst)
{
	struct kdbus_bus *bus = conn_dst->ep->bus;
	int ret = 0;

	if (conn_dst->flags & KDBUS_HELLO_ATTACH_COMM) {
		char comm[TASK_COMM_LEN];

		get_task_comm(comm, current->group_leader);
		ret = kdbus_kmsg_append_str(kmsg, KDBUS_MSG_SRC_TID_COMM, comm);
		if (ret < 0)
			return ret;

		get_task_comm(comm, current);
		ret = kdbus_kmsg_append_str(kmsg, KDBUS_MSG_SRC_PID_COMM, comm);
		if (ret < 0)
			return ret;
	}

	if (conn_dst->flags & KDBUS_HELLO_ATTACH_EXE) {
		struct mm_struct *mm = get_task_mm(current);
		struct path *exe_path = NULL;

		if (mm) {
			down_read(&mm->mmap_sem);
			if (mm->exe_file) {
				path_get(&mm->exe_file->f_path);
				exe_path = &mm->exe_file->f_path;
			}
			up_read(&mm->mmap_sem);
			mmput(mm);
		}

		if (exe_path) {
			char *tmp;
			char *pathname;
			size_t len;

			tmp = (char *) __get_free_page(GFP_TEMPORARY | __GFP_ZERO);
			if (!tmp) {
				path_put(exe_path);
				return -ENOMEM;
			}

			pathname = d_path(exe_path, tmp, PAGE_SIZE);
			if (!IS_ERR(pathname)) {
				len = tmp + PAGE_SIZE - pathname;
				ret = kdbus_kmsg_append_data(kmsg, KDBUS_MSG_SRC_EXE,
							     pathname, len);
			}

			free_page((unsigned long) tmp);
			path_put(exe_path);

			if (ret < 0)
				return ret;
		}
	}

	if (conn_dst->flags & KDBUS_HELLO_ATTACH_CMDLINE) {
		struct mm_struct *mm = current->mm;
		char *tmp;

		tmp = (char *) __get_free_page(GFP_TEMPORARY | __GFP_ZERO);
		if (!tmp)
			return -ENOMEM;

		if (mm && mm->arg_end) {
			size_t len = mm->arg_end - mm->arg_start;

			if (len > PAGE_SIZE)
				len = PAGE_SIZE;

			ret = copy_from_user(tmp, (const char __user *) mm->arg_start, len);
			if (ret == 0)
				ret = kdbus_kmsg_append_data(kmsg, KDBUS_MSG_SRC_CMDLINE,
							     tmp, len);
		}

		free_page((unsigned long) tmp);

		if (ret < 0)
			return ret;
	}

	/* we always return a 4 elements, the element size is 1/4  */
	if (conn_dst->flags & KDBUS_HELLO_ATTACH_CAPS) {
		const struct cred *cred;
		struct caps {
			u32 cap[_KERNEL_CAPABILITY_U32S];
		} cap[4];
		unsigned int i;

		rcu_read_lock();
		cred = __task_cred(current);
		for (i = 0; i < _KERNEL_CAPABILITY_U32S; i++) {
			cap[0].cap[i] = cred->cap_inheritable.cap[i];
			cap[1].cap[i] = cred->cap_permitted.cap[i];
			cap[2].cap[i] = cred->cap_effective.cap[i];
			cap[3].cap[i] = cred->cap_bset.cap[i];
		}
		rcu_read_unlock();

		/* clear unused bits */
		for (i = 0; i < 4; i++)
			cap[i].cap[CAP_TO_INDEX(CAP_LAST_CAP)] &=
				CAP_TO_MASK(CAP_LAST_CAP + 1) - 1;

		ret = kdbus_kmsg_append_data(kmsg, KDBUS_MSG_SRC_CAPS,
					     cap, sizeof(cap));
		if (ret < 0)
			return ret;
	}

#ifdef CONFIG_CGROUPS
	/* attach the path of the one group hierarchy specified for the bus */
	if (conn_dst->flags & KDBUS_HELLO_ATTACH_CGROUP && bus->cgroup_id > 0) {
		char *tmp;

		tmp = (char *) __get_free_page(GFP_TEMPORARY | __GFP_ZERO);
		if (!tmp)
			return -ENOMEM;

		ret = task_cgroup_path_from_hierarchy(current, bus->cgroup_id, tmp, PAGE_SIZE);
		if (ret >= 0)
			ret = kdbus_kmsg_append_str(kmsg, KDBUS_MSG_SRC_CGROUP, tmp);

		free_page((unsigned long) tmp);

		if (ret < 0)
			return ret;
	}
#endif

#ifdef CONFIG_AUDITSYSCALL
	if (conn_dst->flags & KDBUS_HELLO_ATTACH_AUDIT) {
		ret = kdbus_kmsg_append_data(kmsg, KDBUS_MSG_SRC_AUDIT,
					     conn_src->audit_ids,
					     sizeof(conn_src->audit_ids));
		if (ret < 0)
			return ret;
	}
#endif

#ifdef CONFIG_SECURITY
	if (conn_dst->flags & KDBUS_HELLO_ATTACH_SECLABEL) {
		if (conn_src->sec_label_len > 0) {
			ret = kdbus_kmsg_append_data(kmsg,
						     KDBUS_MSG_SRC_SECLABEL,
						     conn_src->sec_label,
						     conn_src->sec_label_len);
			if (ret < 0)
				return ret;
		}
	}
#endif

	return 0;
}

int kdbus_kmsg_send(struct kdbus_ep *ep,
		    struct kdbus_conn *conn_src,
		    struct kdbus_kmsg *kmsg)
{
	struct kdbus_conn *conn_dst = NULL;
	const struct kdbus_msg *msg;
	u64 now_ns = 0;
	int ret;

	/* augment incoming message */
	ret = kdbus_kmsg_append_timestamp(kmsg, &now_ns);
	if (ret < 0)
		return ret;

	if (conn_src) {
		ret = kdbus_kmsg_append_src_names(kmsg, conn_src);
		if (ret < 0)
			return ret;

		ret = kdbus_kmsg_append_cred(kmsg, &conn_src->creds);
		if (ret < 0)
			return ret;
	}

	msg = &kmsg->msg;
//	kdbus_msg_dump(msg);

	if (msg->dst_id == KDBUS_DST_ID_WELL_KNOWN_NAME) {
		const struct kdbus_item *name_item;
		const struct kdbus_name_entry *name_entry;

		name_item = kdbus_msg_get_item(msg, KDBUS_MSG_DST_NAME, 0);
		if (!name_item)
			return -EDESTADDRREQ;

		/* lookup and determine conn_dst ... */
		name_entry = kdbus_name_lookup(ep->bus->name_registry,
					       name_item->data);
		if (!name_entry)
			return -ESRCH;

		conn_dst = name_entry->conn;

		if ((msg->flags & KDBUS_MSG_FLAGS_NO_AUTO_START) &&
		    (conn_dst->flags & KDBUS_HELLO_STARTER))
			return -EADDRNOTAVAIL;

	} else if (msg->dst_id != KDBUS_DST_ID_BROADCAST) {
		/* direct message */
		conn_dst = kdbus_bus_find_conn_by_id(ep->bus, msg->dst_id);
		if (!conn_dst)
			return -ENXIO;
	}

	if (conn_dst) {
		/* direct message */

		if (msg->timeout_ns)
			kmsg->deadline_ns = now_ns + msg->timeout_ns;

		/* check policy */
		if (ep->policy_db && conn_src) {
			ret = kdbus_policy_db_check_send_access(ep->policy_db,
								conn_src,
								conn_dst,
								kmsg->deadline_ns);
			if (ret < 0)
				return ret;
		}

		/* direct message */
		if (conn_src) {
			ret = kdbus_msg_append_for_dst(kmsg, conn_src, conn_dst);
			if (ret < 0)
				return ret;
		}

		ret = kdbus_conn_enqueue_kmsg(conn_dst, kmsg);

		if (msg->timeout_ns)
			kdbus_conn_schedule_timeout_scan(conn_dst);
	} else {
		/* broadcast */
		/* timeouts are not allowed for broadcasts */
		if (msg->timeout_ns)
			return -ENOTUNIQ;

		ret = 0;

		list_for_each_entry(conn_dst, &ep->connection_list,
				    connection_entry) {
			if (conn_dst->type != KDBUS_CONN_EP)
				continue;

			if (conn_dst->id == msg->src_id)
				continue;

			if (!conn_dst->active)
				continue;

			if (!conn_dst->monitor &&
			    !kdbus_match_db_match_kmsg(conn_dst->match_db,
						       conn_src, conn_dst,
						       kmsg))
				continue;

			ret = kdbus_conn_enqueue_kmsg(conn_dst, kmsg);
			if (ret < 0)
				break;
		}
	}

	return ret;
}

int kdbus_kmsg_recv(struct kdbus_conn *conn, void __user *buf)
{
	struct kdbus_msg_list_entry *entry;
	const struct kdbus_kmsg *kmsg = NULL;
	const struct kdbus_msg *msg;
	const struct kdbus_item *item;
	const struct kdbus_item *vecs_next;
	u64 size, pos, max_size;
	int ret;

	if (!KDBUS_IS_ALIGNED8((unsigned long)buf))
		return -EFAULT;

	if (kdbus_size_get_user(size, buf, struct kdbus_msg))
		return -EFAULT;

	mutex_lock(&conn->msg_lock);
	if (conn->msg_count == 0) {
		ret = -EAGAIN;
		goto exit_unlock;
	}

	entry = list_first_entry(&conn->msg_list, struct kdbus_msg_list_entry, entry);
	kmsg = entry->kmsg;
	msg = &kmsg->msg;

	max_size = msg->size;

	if (kmsg->meta)
		max_size += kmsg->meta->size;

	if (kmsg->vecs)
		max_size += kmsg->vecs_size;

	/* reuturn needed buffer size to the receiver */
	if (size < max_size) {
		kdbus_size_set_user(max_size, buf, struct kdbus_msg);
		ret = -ENOBUFS;
		goto exit_unlock;
	}

	/* copy the message header */
	if (copy_to_user(buf, msg, KDBUS_MSG_HEADER_SIZE)) {
		ret = -EFAULT;
		goto exit_unlock;
	}

	pos = KDBUS_MSG_HEADER_SIZE;

	/* The order and sequence of PAYLOAD and PAYLOAD_VEC is always
	 * preserved, it might have meaning in the sender/receiver contract;
	 * one type is freely concerted to the other though, depending
	 * on the actual copying strategy */
	vecs_next = kmsg->vecs;
	KDBUS_ITEM_FOREACH(item, msg) {
		switch (item->type) {
		case KDBUS_MSG_PAYLOAD:
			if (copy_to_user(buf + pos, item, item->size)) {
				ret = -EFAULT;
				goto exit_unlock;
			}

			pos += KDBUS_ALIGN8(item->size);
			break;

		case KDBUS_MSG_PAYLOAD_VEC:
			if (!kmsg->vecs) {
				/* copy PAYLOAD_VEC from the sender to the receiver */
				ret = -ENOSYS;
				goto exit_unlock;
				break;
			}

			/* copy PAYLOAD_VEC we converted to PAYLOAD */
			if (copy_to_user(buf + pos, vecs_next, vecs_next->size)) {
				ret = -EFAULT;
				goto exit_unlock;
			}

			pos += KDBUS_ALIGN8(vecs_next->size);
			vecs_next = KDBUS_ITEM_NEXT(vecs_next);
			break;
		}
	}

	/* install file descriptors */
	if (kmsg->fds) {
		unsigned int i;
		size_t size;

		for (i = 0; i < kmsg->fds_count; i++) {
			int fd;

			fd = get_unused_fd();
			if (fd < 0) {
				ret = fd;
				goto exit_unlock_fds;
			}

			fd_install(fd, get_file(kmsg->fds_fp[i]));
			kmsg->fds->fds[i] = fd;
		}

		size = KDBUS_ITEM_HEADER_SIZE + (sizeof(int) * kmsg->fds_count);
		kmsg->fds->size = size;
		kmsg->fds->type = KDBUS_MSG_UNIX_FDS;

		if (copy_to_user(buf + pos, kmsg->fds, size)) {
			ret = -EFAULT;
			goto exit_unlock_fds;
		}

		pos += KDBUS_ALIGN8(size);
	}

	/* append metadata records */
	if (kmsg->meta) {
		if (copy_to_user(buf + pos, kmsg->meta, kmsg->meta_size)) {
			ret = -EFAULT;
			goto exit_unlock_fds;
		}

		pos += KDBUS_ALIGN8(kmsg->meta_size);
	}

	/* update the returned data size in the message header */
	ret = kdbus_size_set_user(pos, buf, struct kdbus_msg);
	if (ret)
		goto exit_unlock_fds;

	conn->msg_count--;
	list_del(&entry->entry);
	kdbus_kmsg_unref(entry->kmsg);
	kfree(entry);
	mutex_unlock(&conn->msg_lock);

	return 0;

exit_unlock_fds:
	/* cleanup installed file descriptors */
	if (kmsg->fds) {
		unsigned int i;

		for (i = 0; i < kmsg->fds_count; i++) {
			if (kmsg->fds->fds[i] < 0)
				continue;

			fput(kmsg->fds_fp[i]);
			put_unused_fd(kmsg->fds->fds[i]);
			kmsg->fds->fds[i] = -1;
		}
	}

exit_unlock:
	mutex_unlock(&conn->msg_lock);

	return ret;
}
