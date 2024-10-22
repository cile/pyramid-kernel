/* Copyright (c) 2011, Code Aurora Forum. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/interrupt.h>
#include <linux/fb.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/list.h>
#include <linux/file.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/wait.h>
#include <linux/uaccess.h>
#include <linux/anon_inodes.h>
#include <linux/miscdevice.h>
#include <linux/genlock.h>

/* Lock states - can either be unlocked, held as an exclusive write lock or a
 * shared read lock
 */

#define _UNLOCKED 0
#define _RDLOCK  GENLOCK_RDLOCK
#define _WRLOCK GENLOCK_WRLOCK

struct genlock {
	struct list_head active;  /* List of handles holding lock */
	spinlock_t lock;          /* Spinlock to protect the lock internals */
	wait_queue_head_t queue;  /* Holding pen for processes pending lock */
	struct file *file;        /* File structure for exported lock */
	int state;                /* Current state of the lock */
};

struct genlock_handle {
	struct genlock *lock;     /* Lock currently attached to the handle */
	struct list_head entry;   /* List node for attaching to a lock */
	struct file *file;        /* File structure associated with handle */
	int active;		  /* Number of times the active lock has been
				     taken */
};

/*
 * Release the genlock object. Called when all the references to
 * the genlock file descriptor are released
 */

static int genlock_release(struct inode *inodep, struct file *file)
{
	kfree(file->private_data);
	return 0;
}

static const struct file_operations genlock_fops = {
	.release = genlock_release,
};

/**
 * genlock_create_lock - Create a new lock
 * @handle - genlock handle to attach the lock to
 *
 * Returns: a pointer to the genlock
 */

struct genlock *genlock_create_lock(struct genlock_handle *handle)
{
	struct genlock *lock;

	if (handle->lock != NULL)
		return ERR_PTR(-EINVAL);

	lock = kzalloc(sizeof(*lock), GFP_KERNEL);
	if (lock == NULL)
		return ERR_PTR(-ENOMEM);

	INIT_LIST_HEAD(&lock->active);
	init_waitqueue_head(&lock->queue);
	spin_lock_init(&lock->lock);

	lock->state = _UNLOCKED;

	/*
	 * Create an anonyonmous inode for the object that can exported to
	 * other processes
	 */

	lock->file = anon_inode_getfile("genlock", &genlock_fops,
		lock, O_RDWR);

	/* Attach the new lock to the handle */
	handle->lock = lock;

	return lock;
}
EXPORT_SYMBOL(genlock_create_lock);

/*
 * Get a file descriptor reference to a lock suitable for sharing with
 * other processes
 */

static int genlock_get_fd(struct genlock *lock)
{
	int ret;

	if (!lock->file)
		return -EINVAL;

	ret = get_unused_fd_flags(0);
	if (ret < 0)
		return ret;
	fd_install(ret, lock->file);
	return ret;
}

/**
 * genlock_attach_lock - Attach an existing lock to a handle
 * @handle - Pointer to a genlock handle to attach the lock to
 * @fd - file descriptor for the exported lock
 *
 * Returns: A pointer to the attached lock structure
 */

struct genlock *genlock_attach_lock(struct genlock_handle *handle, int fd)
{
	struct file *file;

	if (handle->lock != NULL)
		return ERR_PTR(-EINVAL);

	file = fget(fd);
	if (file == NULL)
		return ERR_PTR(-EBADF);

	handle->lock = file->private_data;

	return handle->lock;
}
EXPORT_SYMBOL(genlock_attach_lock);

/* Helper function that returns 1 if the specified handle holds the lock */

static int handle_has_lock(struct genlock *lock, struct genlock_handle *handle)
{
	struct genlock_handle *h;

	list_for_each_entry(h, &lock->active, entry) {
		if (h == handle)
			return 1;
	}

	return 0;
}

/* If the lock just became available, signal the next entity waiting for it */

static void _genlock_signal(struct genlock *lock)
{
	if (list_empty(&lock->active)) {
		/* If the list is empty, then the lock is free */
		lock->state = _UNLOCKED;
		/* Wake up the first process sitting in the queue */
		wake_up(&lock->queue);
	}
}

/* Attempt to release the handle's ownership of the lock */

static int _genlock_unlock(struct genlock *lock, struct genlock_handle *handle)
{
	int ret = -EINVAL;
	unsigned long irqflags;

	spin_lock_irqsave(&lock->lock, irqflags);

	if (lock->state == _UNLOCKED)
		goto done;

	/* Make sure this handle is an owner of the lock */
	if (!handle_has_lock(lock, handle))
		goto done;

	/* If the handle holds no more references to the lock then
	   release it (maybe) */

	if (--handle->active == 0) {
		list_del(&handle->entry);
		_genlock_signal(lock);
	}

	ret = 0;

done:
	spin_unlock_irqrestore(&lock->lock, irqflags);
	return ret;
}

/* Attempt to acquire the lock for the handle */

static int _genlock_lock(struct genlock *lock, struct genlock_handle *handle,
	int op, int flags, uint32_t timeout)
{
	unsigned long irqflags;
	int ret = 0;
	unsigned int ticks = msecs_to_jiffies(timeout);

	spin_lock_irqsave(&lock->lock, irqflags);

	/* Sanity check - no blocking locks in a debug context. Even if it
	 * succeed to not block, the mere idea is too dangerous to continue
	 */

	if (in_interrupt() && !(flags & GENLOCK_NOBLOCK))
		BUG();

	/* Fast path - the lock is unlocked, so go do the needful */

	if (lock->state == _UNLOCKED)
		goto dolock;

	if (handle_has_lock(lock, handle)) {

		/*
		 * If the handle already holds the lock and the type matches,
		 * then just increment the active pointer. This allows the
		 * handle to do recursive locks
		 */

		if (lock->state == op) {
			handle->active++;
			goto done;
		}

		/*
		 * If the handle holds a write lock then the owner can switch
		 * to a read lock if they want. Do the transition atomically
		 * then wake up any pending waiters in case they want a read
		 * lock too.
		 */

		if (op == _RDLOCK && handle->active == 1) {
			lock->state = _RDLOCK;
			wake_up(&lock->queue);
			goto done;
		}

		/*
		 * Otherwise the user tried to turn a read into a write, and we
		 * don't allow that.
		 */

		ret = -EINVAL;
		goto done;
	}

	/*
	 * If we request a read and the lock is held by a read, then go
	 * ahead and share the lock
	 */

	if (op == GENLOCK_RDLOCK && lock->state == _RDLOCK)
		goto dolock;

	/* Treat timeout 0 just like a NOBLOCK flag and return if the
	   lock cannot be aquired without blocking */

	if (flags & GENLOCK_NOBLOCK || timeout == 0) {
		ret = -EAGAIN;
		goto done;
	}

	/* Wait while the lock remains in an incompatible state */

	while (lock->state != _UNLOCKED) {
		unsigned int elapsed;

		spin_unlock_irqrestore(&lock->lock, irqflags);

		elapsed = wait_event_interruptible_timeout(lock->queue,
			lock->state == _UNLOCKED, ticks);

		spin_lock_irqsave(&lock->lock, irqflags);

		if (elapsed <= 0) {
			ret = (elapsed < 0) ? elapsed : -ETIMEDOUT;
			goto done;
		}

		ticks = elapsed;
	}

dolock:
	/* We can now get the lock, add ourselves to the list of owners */

	list_add_tail(&handle->entry, &lock->active);
	lock->state = op;
	handle->active = 1;

done:
	spin_unlock_irqrestore(&lock->lock, irqflags);
	return ret;

}

/**
 * genlock_lock - Acquire or release a lock
 * @handle - pointer to the genlock handle that is requesting the lock
 * @op - the operation to perform (RDLOCK, WRLOCK, UNLOCK)
 * @flags - flags to control the operation
 * @timeout - optional timeout to wait for the lock to come free
 *
 * Returns: 0 on success or error code on failure
 */

int genlock_lock(struct genlock_handle *handle, int op, int flags,
	uint32_t timeout)
{
	struct genlock *lock = handle->lock;
	int ret = 0;

	if (lock == NULL)
		return -EINVAL;

	switch (op) {
	case GENLOCK_UNLOCK:
		ret = _genlock_unlock(lock, handle);
		break;
	case GENLOCK_RDLOCK:
	case GENLOCK_WRLOCK:
		ret = _genlock_lock(lock, handle, op, flags, timeout);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}
EXPORT_SYMBOL(genlock_lock);

/**
 * genlock_wait - Wait for the lock to be released
 * @handle - pointer to the genlock handle that is waiting for the lock
 * @timeout - optional timeout to wait for the lock to get released
 */

int genlock_wait(struct genlock_handle *handle, uint32_t timeout)
{
	struct genlock *lock = handle->lock;
	unsigned long irqflags;
	int ret = 0;
	unsigned int ticks = msecs_to_jiffies(timeout);

	if (lock == NULL)
		return -EINVAL;

	spin_lock_irqsave(&lock->lock, irqflags);

	/*
	 * if timeout is 0 and the lock is already unlocked, then success
	 * otherwise return -EAGAIN
	 */

	if (timeout == 0) {
		ret = (lock->state == _UNLOCKED) ? 0 : -EAGAIN;
		goto done;
	}

	while (lock->state != _UNLOCKED) {
		unsigned int elapsed;

		spin_unlock_irqrestore(&lock->lock, irqflags);

		elapsed = wait_event_interruptible_timeout(lock->queue,
			lock->state == _UNLOCKED, ticks);

		spin_lock_irqsave(&lock->lock, irqflags);

		if (elapsed <= 0) {
			ret = (elapsed < 0) ? elapsed : -ETIMEDOUT;
			break;
		}

		ticks = elapsed;
	}

done:
	spin_unlock_irqrestore(&lock->lock, irqflags);
	return ret;
}

/**
 * genlock_release_lock - Release a lock attached to a handle
 * @handle - Pointer to the handle holding the lock
 */

void genlock_release_lock(struct genlock_handle *handle)
{
	unsigned long flags;

	if (handle == NULL || handle->lock == NULL)
		return;

	spin_lock_irqsave(&handle->lock->lock, flags);

	/* If the handle is holding the lock, then force it closed */

	if (handle_has_lock(handle->lock, handle)) {
		list_del(&handle->entry);
		_genlock_signal(handle->lock);
	}
	spin_unlock_irqrestore(&handle->lock->lock, flags);

	fput(handle->lock->file);
	handle->lock = NULL;
	handle->active = 0;
}
EXPORT_SYMBOL(genlock_release_lock);

/*
 * Release function called when all references to a handle are released
 */

static int genlock_handle_release(struct inode *inodep, struct file *file)
{
	struct genlock_handle *handle = file->private_data;

	genlock_release_lock(handle);
	kfree(handle);

	return 0;
}

static const struct file_operations genlock_handle_fops = {
	.release = genlock_handle_release
};

/*
 * Allocate a new genlock handle
 */

static struct genlock_handle *_genlock_get_handle(void)
{
	struct genlock_handle *handle = kzalloc(sizeof(*handle), GFP_KERNEL);
	if (handle == NULL)
		return ERR_PTR(-ENOMEM);

	return handle;
}

/**
 * genlock_get_handle - Create a new genlock handle
 *
 * Returns: A pointer to a new genlock handle
 */

struct genlock_handle *genlock_get_handle(void)
{
	struct genlock_handle *handle = _genlock_get_handle();
	if (IS_ERR(handle))
		return handle;

	handle->file = anon_inode_getfile("genlock-handle",
		&genlock_handle_fops, handle, O_RDWR);

	return handle;
}
EXPORT_SYMBOL(genlock_get_handle);

/**
 * genlock_put_handle - release a reference to a genlock handle
 * @handle - A pointer to the handle to release
 */

void genlock_put_handle(struct genlock_handle *handle)
{
	if (handle)
		fput(handle->file);
}
EXPORT_SYMBOL(genlock_put_handle);

/**
 * genlock_get_handle_fd - Get a handle reference from a file descriptor
 * @fd - The file descriptor for a genlock handle
 */

struct genlock_handle *genlock_get_handle_fd(int fd)
{
	struct file *file = fget(fd);

	if (file == NULL)
		return ERR_PTR(-EINVAL);

	return file->private_data;
}
EXPORT_SYMBOL(genlock_get_handle_fd);

#ifdef CONFIG_GENLOCK_MISCDEVICE

static long genlock_dev_ioctl(struct file *filep, unsigned int cmd,
	unsigned long arg)
{
	struct genlock_lock param;
	struct genlock_handle *handle = filep->private_data;
	struct genlock *lock;
	int ret;

	switch (cmd) {
	case GENLOCK_IOC_NEW: {
		lock = genlock_create_lock(handle);
		if (IS_ERR(lock))
			return PTR_ERR(lock);

		return 0;
	}
	case GENLOCK_IOC_EXPORT: {
		if (handle->lock == NULL)
			return -EINVAL;

		ret = genlock_get_fd(handle->lock);
		if (ret < 0)
			return ret;

		param.fd = ret;

		if (copy_to_user((void __user *) arg, &param,
			sizeof(param)))
			return -EFAULT;

		return 0;
		}
	case GENLOCK_IOC_ATTACH: {
		if (copy_from_user(&param, (void __user *) arg,
			sizeof(param)))
			return -EFAULT;

		lock = genlock_attach_lock(handle, param.fd);
		if (IS_ERR(lock))
			return PTR_ERR(lock);

		return 0;
	}
	case GENLOCK_IOC_LOCK: {
		if (copy_from_user(&param, (void __user *) arg,
		sizeof(param)))
			return -EFAULT;

		return genlock_lock(handle, param.op, param.flags,
			param.timeout);
	}
	case GENLOCK_IOC_WAIT: {
		if (copy_from_user(&param, (void __user *) arg,
		sizeof(param)))
			return -EFAULT;

		return genlock_wait(handle, param.timeout);
	}
	case GENLOCK_IOC_RELEASE: {
		genlock_release_lock(handle);
		return 0;
	}
	default:
		return -EINVAL;
	}
}

static int genlock_dev_release(struct inode *inodep, struct file *file)
{
	struct genlock_handle *handle = file->private_data;

	genlock_put_handle(handle);

	return 0;
}

static int genlock_dev_open(struct inode *inodep, struct file *file)
{
	struct genlock_handle *handle = _genlock_get_handle();
	if (IS_ERR(handle))
		return PTR_ERR(handle);

	handle->file = file;
	file->private_data = handle;
	return 0;
}

static const struct file_operations genlock_dev_fops = {
	.open = genlock_dev_open,
	.release = genlock_dev_release,
	.unlocked_ioctl = genlock_dev_ioctl,
};

static struct miscdevice genlock_dev;

static int genlock_dev_init(void)
{
	genlock_dev.minor = MISC_DYNAMIC_MINOR;
	genlock_dev.name = "genlock";
	genlock_dev.fops = &genlock_dev_fops;
	genlock_dev.parent = NULL;

	return misc_register(&genlock_dev);
}

static void genlock_dev_close(void)
{
	misc_deregister(&genlock_dev);
}

module_init(genlock_dev_init);
module_exit(genlock_dev_close);

#endif
