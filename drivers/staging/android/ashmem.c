/* mm/ashmem.c
 *
 * Anonymous Shared Memory Subsystem, ashmem
 *
 * Copyright (C) 2008 Google, Inc.
 *
 * Robert Love <rlove@google.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define pr_fmt(fmt) "ashmem: " fmt

#include <linux/init.h>
#include <linux/export.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/falloc.h>
#include <linux/miscdevice.h>
#include <linux/security.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/uaccess.h>
#include <linux/personality.h>
#include <linux/bitops.h>
#include <linux/mutex.h>
#include <linux/shmem_fs.h>
#include "ashmem.h"

#define ASHMEM_NAME_PREFIX "dev/ashmem/"
#define ASHMEM_NAME_PREFIX_LEN (sizeof(ASHMEM_NAME_PREFIX) - 1)
#define ASHMEM_FULL_NAME_LEN (ASHMEM_NAME_LEN + ASHMEM_NAME_PREFIX_LEN)

/**
 * struct ashmem_area - The anonymous shared memory area
 * @name:		The optional name in /proc/pid/maps
 * @unpinned_list:	The list of all ashmem areas
 * @file:		The shmem-based backing file
 * @size:		The size of the mapping, in bytes
 * @prot_mask:		The allowed protection bits, as vm_flags
 *
 * The lifecycle of this structure is from our parent file's open() until
 * its release(). It is also protected by 'ashmem_mutex'
 *
 * Warning: Mappings do NOT pin this structure; It dies on close()
 */
struct ashmem_area {
	struct mutex mmap_lock;
	struct file *file;
	size_t size;
	unsigned long prot_mask;
};

/**
 * struct ashmem_range - A range of unpinned/evictable pages
 * @lru:	         The entry in the LRU list
 * @unpinned:	         The entry in its area's unpinned list
 * @asma:	         The associated anonymous shared memory area.
 * @pgstart:	         The starting page (inclusive)
 * @pgend:	         The ending page (inclusive)
 * @purged:	         The purge status (ASHMEM_NOT or ASHMEM_WAS_PURGED)
 *
 * The lifecycle of this structure is from unpin to pin.
 * It is protected by 'ashmem_mutex'
 */
struct ashmem_range {
	struct list_head lru;
	struct list_head unpinned;
	struct ashmem_area *asma;
	size_t pgstart;
	size_t pgend;
	unsigned int purged;
};

/* LRU list of unpinned pages, protected by ashmem_mutex */
static LIST_HEAD(ashmem_lru_list);

/*
 * long lru_count - The count of pages on our LRU list.
 *
 * This is protected by ashmem_mutex.
 */
static unsigned long lru_count;

/*
 * ashmem_mutex - protects the list of and each individual ashmem_area
 *
 * Lock Ordering: ashmex_mutex -> i_mutex -> i_alloc_sem
 */
static DEFINE_MUTEX(ashmem_mutex);

static struct kmem_cache *ashmem_area_cachep __read_mostly;
static struct kmem_cache *ashmem_range_cachep __read_mostly;

/*
 * A separate lockdep class for the backing shmem inodes to resolve the lockdep
 * warning about the race between kswapd taking fs_reclaim before inode_lock
 * and write syscall taking inode_lock and then fs_reclaim.
 * Note that such race is impossible because ashmem does not support write
 * syscalls operating on the backing shmem.
 */
static struct lock_class_key backing_shmem_inode_class;

static inline unsigned long range_size(struct ashmem_range *range)
{
	return range->pgend - range->pgstart + 1;
}

static inline bool range_on_lru(struct ashmem_range *range)
{
	return range->purged == ASHMEM_NOT_PURGED;
}

static inline bool page_range_subsumes_range(struct ashmem_range *range,
					     size_t start, size_t end)
{
	return (range->pgstart >= start) && (range->pgend <= end);
}

static inline bool page_range_subsumed_by_range(struct ashmem_range *range,
						size_t start, size_t end)
{
	return (range->pgstart <= start) && (range->pgend >= end);
}

static inline bool page_in_range(struct ashmem_range *range, size_t page)
{
	return (range->pgstart <= page) && (range->pgend >= page);
}

static inline bool page_range_in_range(struct ashmem_range *range,
				       size_t start, size_t end)
{
	return page_in_range(range, start) || page_in_range(range, end) ||
		page_range_subsumes_range(range, start, end);
}

static inline bool range_before_page(struct ashmem_range *range, size_t page)
{
	return range->pgend < page;
}

#define PROT_MASK		(PROT_EXEC | PROT_READ | PROT_WRITE)

/**
 * lru_add() - Adds a range of memory to the LRU list
 * @range:     The memory range being added.
 *
 * The range is first added to the end (tail) of the LRU list.
 * After this, the size of the range is added to @lru_count
 */
static inline void lru_add(struct ashmem_range *range)
{
	list_add_tail(&range->lru, &ashmem_lru_list);
	lru_count += range_size(range);
}

/**
 * lru_del() - Removes a range of memory from the LRU list
 * @range:     The memory range being removed
 *
 * The range is first deleted from the LRU list.
 * After this, the size of the range is removed from @lru_count
 */
static inline void lru_del(struct ashmem_range *range)
{
	list_del(&range->lru);
	lru_count -= range_size(range);
}

/**
 * range_alloc() - Allocates and initializes a new ashmem_range structure
 * @asma:	   The associated ashmem_area
 * @prev_range:	   The previous ashmem_range in the sorted asma->unpinned list
 * @purged:	   Initial purge status (ASMEM_NOT_PURGED or ASHMEM_WAS_PURGED)
 * @start:	   The starting page (inclusive)
 * @end:	   The ending page (inclusive)
 *
 * This function is protected by ashmem_mutex.
 *
 * Return: 0 if successful, or -ENOMEM if there is an error
 */
static int range_alloc(struct ashmem_area *asma,
		       struct ashmem_range *prev_range, unsigned int purged,
		       size_t start, size_t end)
{
	struct ashmem_range *range;

	range = kmem_cache_zalloc(ashmem_range_cachep, GFP_KERNEL);
	if (!range)
		return -ENOMEM;

	range->asma = asma;
	range->pgstart = start;
	range->pgend = end;
	range->purged = purged;

	list_add_tail(&range->unpinned, &prev_range->unpinned);

	if (range_on_lru(range))
		lru_add(range);

	return 0;
}

/**
 * range_del() - Deletes and dealloctes an ashmem_range structure
 * @range:	 The associated ashmem_range that has previously been allocated
 */
static void range_del(struct ashmem_range *range)
{
	list_del(&range->unpinned);
	if (range_on_lru(range))
		lru_del(range);
	kmem_cache_free(ashmem_range_cachep, range);
}

/**
 * range_shrink() - Shrinks an ashmem_range
 * @range:	    The associated ashmem_range being shrunk
 * @start:	    The starting byte of the new range
 * @end:	    The ending byte of the new range
 *
 * This does not modify the data inside the existing range in any way - It
 * simply shrinks the boundaries of the range.
 *
 * Theoretically, with a little tweaking, this could eventually be changed
 * to range_resize, and expand the lru_count if the new range is larger.
 */
static inline void range_shrink(struct ashmem_range *range,
				size_t start, size_t end)
{
	size_t pre = range_size(range);

	range->pgstart = start;
	range->pgend = end;

	if (range_on_lru(range))
		lru_count -= pre - range_size(range);
}

/**
 * ashmem_open() - Opens an Anonymous Shared Memory structure
 * @inode:	   The backing file's index node(?)
 * @file:	   The backing file
 *
 * Please note that the ashmem_area is not returned by this function - It is
 * instead written to "file->private_data".
 *
 * Return: 0 if successful, or another code if unsuccessful.
 */
static int ashmem_open(struct inode *inode, struct file *file)
{
	struct ashmem_area *asma;
	int ret;

	ret = generic_file_open(inode, file);
	if (unlikely(ret))
		return ret;

	asma = kmem_cache_alloc(ashmem_area_cachep, GFP_KERNEL);
	if (unlikely(!asma))
		return -ENOMEM;

	*asma = (typeof(*asma)){
		.mmap_lock = __MUTEX_INITIALIZER(asma->mmap_lock),
		.prot_mask = PROT_MASK
	};

	file->private_data = asma;

	return 0;
}

/**
 * ashmem_release() - Releases an Anonymous Shared Memory structure
 * @ignored:	      The backing file's Index Node(?) - It is ignored here.
 * @file:	      The backing file
 *
 * Return: 0 if successful. If it is anything else, go have a coffee and
 * try again.
 */
static int ashmem_release(struct inode *ignored, struct file *file)
{
	struct ashmem_area *asma = file->private_data;
	struct ashmem_range *range, *next;

	mutex_lock(&ashmem_mutex);
	list_for_each_entry_safe(range, next, &asma->unpinned_list, unpinned)
		range_del(range);
	mutex_unlock(&ashmem_mutex);

	if (asma->file)
		fput(asma->file);
	kmem_cache_free(ashmem_area_cachep, asma);

	return 0;
}

static ssize_t ashmem_read_iter(struct kiocb *iocb, struct iov_iter *iter)
{
	struct ashmem_area *asma = iocb->ki_filp->private_data;
	struct file *vmfile;
	ssize_t ret;

	/* If size is not set, or set to 0, always return EOF. */
	if (!READ_ONCE(asma->size))
		return 0;

	vmfile = READ_ONCE(asma->file);
	if (!vmfile)
		return -EBADF;

	/*
	 * asma and asma->file are used outside the lock here.  We assume
	 * once asma->file is set it will never be changed, and will not
	 * be destroyed until all references to the file are dropped and
	 * ashmem_release is called.
	 */
	ret = vfs_iter_read(vmfile, iter, &iocb->ki_pos, 0);
	if (ret > 0)
		vmfile->f_pos = iocb->ki_pos;
	return ret;
}

static loff_t ashmem_llseek(struct file *file, loff_t offset, int origin)
{
	struct ashmem_area *asma = file->private_data;
	struct file *vmfile;
	loff_t ret;

	if (!READ_ONCE(asma->size))
		return -EINVAL;
	}

	vmfile = READ_ONCE(asma->file);
	if (!vmfile)
		return -EBADF;
	}

	ret = vfs_llseek(vmfile, offset, origin);
	if (ret < 0)
		return ret;

	/** Copy f_pos from backing file, since f_ops->llseek() sets it */
	file->f_pos = vmfile->f_pos;
	return ret;
}

static inline vm_flags_t calc_vm_may_flags(unsigned long prot)
{
	return _calc_vm_trans(prot, PROT_READ,  VM_MAYREAD) |
	       _calc_vm_trans(prot, PROT_WRITE, VM_MAYWRITE) |
	       _calc_vm_trans(prot, PROT_EXEC,  VM_MAYEXEC);
}

static int ashmem_vmfile_mmap(struct file *file, struct vm_area_struct *vma)
{
	/* do not allow to mmap ashmem backing shmem file directly */
	return -EPERM;
}

static unsigned long
ashmem_vmfile_get_unmapped_area(struct file *file, unsigned long addr,
				unsigned long len, unsigned long pgoff,
				unsigned long flags)
{
	return current->mm->get_unmapped_area(file, addr, len, pgoff, flags);
}

static int ashmem_file_setup(struct ashmem_area *asma, size_t size,
			     struct vm_area_struct *vma)
{
	static struct file_operations vmfile_fops;
	static DEFINE_SPINLOCK(vmfile_fops_lock);
	struct file *vmfile;

	vmfile = shmem_file_setup(ASHMEM_NAME_DEF, size, vma->vm_flags);
	if (IS_ERR(vmfile))
		return PTR_ERR(vmfile);

	/*
	 * override mmap operation of the vmfile so that it can't be
	 * remapped which would lead to creation of a new vma with no
	 * asma permission checks. Have to override get_unmapped_area
	 * as well to prevent VM_BUG_ON check for f_ops modification.
	 */
	if (!READ_ONCE(vmfile_fops.mmap)) {
		spin_lock(&vmfile_fops_lock);
		if (!vmfile_fops.mmap) {
			vmfile_fops = *vmfile->f_op;
			vmfile_fops.mmap = ashmem_vmfile_mmap;
			vmfile_fops.get_unmapped_area =
				ashmem_vmfile_get_unmapped_area;
			WRITE_ONCE(vmfile_fops.mmap, ashmem_vmfile_mmap);
		}
		spin_unlock(&vmfile_fops_lock);
	}
	vmfile->f_op = &vmfile_fops;
	vmfile->f_mode |= FMODE_LSEEK;

	WRITE_ONCE(asma->file, vmfile);
	return 0;
}

static int ashmem_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct ashmem_area *asma = file->private_data;
	unsigned long prot_mask;
	size_t size;

	/* user needs to SET_SIZE before mapping */
	size = READ_ONCE(asma->size);
	if (unlikely(!size))
		return -EINVAL;

	/* requested mapping size larger than object size */
	if (vma->vm_end - vma->vm_start > PAGE_ALIGN(size))
		return -EINVAL;

	/* requested protection bits must match our allowed protection mask */
	prot_mask = READ_ONCE(asma->prot_mask);
	if (unlikely((vma->vm_flags & ~calc_vm_prot_bits(prot_mask, 0)) &
		     calc_vm_prot_bits(PROT_MASK, 0)))
		return -EPERM;

	vma->vm_flags &= ~calc_vm_may_flags(~prot_mask);

	if (!READ_ONCE(asma->file)) {
		int ret = 0;

		mutex_lock(&asma->mmap_lock);
		if (!asma->file)
			ret = ashmem_file_setup(asma, size, vma);
		mutex_unlock(&asma->mmap_lock);

		if (ret)
			return ret;
	}

	get_file(asma->file);

	if (vma->vm_flags & VM_SHARED) {
		shmem_set_file(vma, asma->file);
	} else {
		if (vma->vm_file)
			fput(vma->vm_file);
		vma->vm_file = asma->file;
	}
	mutex_unlock(&ashmem_mutex);

	return 0;
}

static int set_prot_mask(struct ashmem_area *asma, unsigned long prot)
{
	/* the user can only remove, not add, protection bits */
	if (unlikely((READ_ONCE(asma->prot_mask) & prot) != prot))
		return -EINVAL;

	/* does the application expect PROT_READ to imply PROT_EXEC? */
	if ((prot & PROT_READ) && (current->personality & READ_IMPLIES_EXEC))
		prot |= PROT_EXEC;

	WRITE_ONCE(asma->prot_mask, prot);
	return 0;
}

static long ashmem_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct ashmem_area *asma = file->private_data;
	long ret = -ENOTTY;

	switch (cmd) {
	case ASHMEM_SET_NAME:
		return 0;
	case ASHMEM_GET_NAME:
		return 0;
	case ASHMEM_SET_SIZE:
		if (READ_ONCE(asma->file))
			return -EINVAL;

		WRITE_ONCE(asma->size, (size_t)arg);
		return 0;
	case ASHMEM_GET_SIZE:
		return READ_ONCE(asma->size);
	case ASHMEM_SET_PROT_MASK:
		return set_prot_mask(asma, arg);
	case ASHMEM_GET_PROT_MASK:
		return READ_ONCE(asma->prot_mask);
	case ASHMEM_PIN:
		return 0;
	case ASHMEM_UNPIN:
		return 0;
	case ASHMEM_GET_PIN_STATUS:
		return ASHMEM_IS_PINNED;
	case ASHMEM_PURGE_ALL_CACHES:
		return capable(CAP_SYS_ADMIN) ? 0 : -EPERM;
	}

	return -ENOTTY;
}

/* support of 32bit userspace on 64bit platforms */
#ifdef CONFIG_COMPAT
static long compat_ashmem_ioctl(struct file *file, unsigned int cmd,
				unsigned long arg)
{
	switch (cmd) {
	case COMPAT_ASHMEM_SET_SIZE:
		cmd = ASHMEM_SET_SIZE;
		break;
	case COMPAT_ASHMEM_SET_PROT_MASK:
		cmd = ASHMEM_SET_PROT_MASK;
		break;
	}
	return ashmem_ioctl(file, cmd, arg);
}
#endif

static const struct file_operations ashmem_fops = {
	.owner = THIS_MODULE,
	.open = ashmem_open,
	.release = ashmem_release,
	.read_iter = ashmem_read_iter,
	.llseek = ashmem_llseek,
	.mmap = ashmem_mmap,
	.unlocked_ioctl = ashmem_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = compat_ashmem_ioctl,
#endif
};

static struct miscdevice ashmem_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "ashmem",
	.fops = &ashmem_fops,
};

static int __init ashmem_init(void)
{
	int ret;

	ashmem_area_cachep = kmem_cache_create("ashmem_area_cache",
					       sizeof(struct ashmem_area),
					       0, 0, NULL);
	if (unlikely(!ashmem_area_cachep)) {
		pr_err("failed to create slab cache\n");
		ret = -ENOMEM;
		goto out;
	}

	ashmem_range_cachep = kmem_cache_create("ashmem_range_cache",
						sizeof(struct ashmem_range),
						0, SLAB_RECLAIM_ACCOUNT, NULL);
	if (!ashmem_range_cachep) {
		pr_err("failed to create slab cache\n");
		goto out_free1;
	}

	ret = misc_register(&ashmem_misc);
	if (unlikely(ret)) {
		pr_err("failed to register misc device!\n");
		goto out_free1;
	}

	register_shrinker(&ashmem_shrinker);

	pr_info("initialized\n");

	return 0;

out_free2:
	kmem_cache_destroy(ashmem_range_cachep);
out_free1:
	kmem_cache_destroy(ashmem_area_cachep);
out:
	return ret;
}
device_initcall(ashmem_init);
