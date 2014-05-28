#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/proc_fs.h>
#include <linux/fcntl.h>
#include <linux/aio.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <linux/semaphore.h>
#include <linux/cdev.h>
#include <linux/kdev_t.h>
#include <linux/device.h>

#include "scullvm.h"

int scullvm_major	= SCULLVM_MAJOR;
int scullvm_devs	= SCULLVM_DEVS;
int scullvm_qset	= SCULLVM_QSET;
int scullvm_order	= SCULLVM_ORDER;

module_param(scullvm_major, int, 0);
module_param(scullvm_devs, int, 0);
module_param(scullvm_qset, int, 0);
module_param(scullvm_order, int, 0);

MODULE_AUTHOR("Salym Senyonga");
MODULE_LICENSE("GPL");

struct scullvm_dev *scullvm_devices;
int scullvm_trim(struct scullvm_dev *dev);
void scullvm_cleanup(void);
static struct class *sc;	/* for the device class in `/sys` */

/* open and close */
int scullvm_open(struct inode *inode, struct file *filp)
{
	struct scullvm_dev *dev;

	dev = container_of(inode->i_cdev, struct scullvm_dev, cdev);

	/* trim to 0 if device was opened in write-only mode */
	if ((filp->f_flags & O_ACCMODE) == O_WRONLY) {
		if (down_interruptible(&dev->sem))
				return -ERESTARTSYS;
		scullvm_trim(dev);
		up(&dev->sem);
	}

	filp->private_data	= dev;

	return 0;
}

int scullvm_release(struct inode *inode, struct file *filp)
{
	return 0;
}

struct scullvm_dev *scullvm_follow(struct scullvm_dev *dev, int n)
{
	while (n--) {
		if (!dev->next) {
			dev->next = kmalloc(sizeof(struct scullvm_dev), GFP_KERNEL);
			memset(dev->next, 0, sizeof(struct scullvm_dev));
		}
		dev = dev->next;
		continue;
	}
	return dev;
}

/* read and write */
ssize_t scullvm_read(struct file *filp, char __user *buf, size_t count,
		     loff_t *f_pos)
{
	int item, s_pos, q_pos, rest;
	struct scullvm_dev *dptr;
	struct scullvm_dev *dev	= filp->private_data;	/* the first list-item */
	int quantum	= PAGE_SIZE << dev->order;
	int qset	= dev->qset;
	int itemsize	= quantum * qset;	/* how many bytes in list-item */
	size_t retval	= 0;

	if (down_interruptible(&dev->sem))
		return -ERESTARTSYS;
	if (*f_pos > dev->size)
		goto nothing;
	if (*f_pos + count > dev->size)
		count = dev->size - *f_pos;

	/* find list-item, qset index and offset in the quantum */
	item	= ((long) *f_pos) / itemsize;
	rest	= ((long) *f_pos) % itemsize;
	s_pos	= rest / quantum;
	q_pos	= rest % quantum;

	/* follow the list to the right position */
	dptr	= scullvm_follow(dev, item);
	if (!dptr->data)
		goto nothing;	/* don't fill holes */
	if (!dptr->data[s_pos])
		goto nothing;
	if (count > quantum - q_pos)
		count	= quantum - q_pos; /* read only up to end of this quantum */

	if (copy_to_user(buf, dptr->data[s_pos] + q_pos, count)) {
		retval	= -EFAULT;
		goto nothing;
	}
	up(&dev->sem);

	*f_pos	+= count;
	return count;

nothing:
	up(&dev->sem);
	return retval;
}

ssize_t scullvm_write(struct file *filp, const char __user *buf, size_t count,
		      loff_t *f_pos)
{
	struct scullvm_dev *dptr;
	int item, s_pos , q_pos, rest;
	struct scullvm_dev *dev	= filp->private_data;
	int quantum	= PAGE_SIZE << dev->order;
	int qset	= dev->qset;
	int itemsize	= quantum * qset;
	ssize_t retval	= -ENOMEM;	/* our most likely error */

	if (down_interruptible(&dev->sem))
		return -ERESTARTSYS;

	item	= ((long) *f_pos) / itemsize;
	rest	= ((long) *f_pos) % itemsize;
	s_pos	= rest / quantum;
	q_pos	= rest % quantum;

	dptr	= scullvm_follow(dev, item);
	if (!dptr->data) {
		dptr->data = kmalloc(qset * sizeof(void *), GFP_KERNEL);
		if (!dptr->data)
			goto nomem;
		memset(dptr->data, 0, qset * sizeof(char *));
	}

	/* Allocate a quantum using virtual addresses */
	if (!dptr->data[s_pos]) {
		dptr->data[s_pos] = (void *) vmalloc(PAGE_SIZE << dptr->order);
		if (!dptr->data[s_pos])
			goto nomem;
		memset(dptr->data[s_pos], 0, PAGE_SIZE << dptr->order);
	}
	if (count > quantum - q_pos)
		count = quantum - q_pos;
	if (copy_from_user(dptr->data[s_pos] + q_pos, buf, count)) {
		retval = -EFAULT;
		goto nomem;
	}
	*f_pos += count;

	/* update the size */
	if (dev->size < *f_pos)
		dev->size = *f_pos;
	up(&dev->sem);
	return count;

nomem:
	up(&dev->sem);
	return retval;
}

/* The ioctl implementation */
long scullvm_ioctl(struct inode *inode, struct file *filp, unsigned int cmd,
		   unsigned long arg)
{
	int tmp;
	int err	= 0;
	int ret	= 0;

	if (_IOC_TYPE(cmd) != SCULLVM_IOC_MAGIC)
		return -ENOTTY;
	if (_IOC_NR(cmd) > SCULLVM_IOC_MAXNR)
		return -ENOTTY;

	/*
	 * the type is a bitmask, and VERIFY_WRITE catches R/W transfers. Note
	 * that the type is user-oriented, while verify_area is kernel-oriented,
	 * so the concept of "read" and "write" is reversed
	 */
	if (_IOC_DIR(cmd) & _IOC_READ)
		err = !access_ok(VERIFY_WRITE, (void __user *)arg, _IOC_SIZE(cmd));
	else if (_IOC_DIR(cmd) & _IOC_WRITE)
		err = !access_ok(VERIFY_READ, (void __user *)arg, _IOC_SIZE(cmd));
	if (err)
		return -EFAULT;

	switch(cmd) {
		case SCULLVM_IOCRESET:
			scullvm_qset	= SCULLVM_QSET;
			scullvm_order	= SCULLVM_ORDER;
			break;

		case SCULLVM_IOCSORDER:
			ret = __get_user(scullvm_order, (int __user *) arg);
			break;

		case SCULLVM_IOCTORDER:
			scullvm_order = arg;
			break;

		case SCULLVM_IOCGORDER:
			ret = __put_user(scullvm_order, (int __user *)arg);
			break;

		case SCULLVM_IOCQORDER:
			return scullvm_order;

		case SCULLVM_IOCXORDER:
			tmp = scullvm_order;
			ret = __get_user(scullvm_order, (int __user *)arg);
			if (ret == 0)
				ret = __put_user(tmp, (int __user *)arg);
			break;

		case SCULLVM_IOCHORDER:
			tmp = scullvm_order;
			scullvm_order = arg;
			return tmp;

		case SCULLVM_IOCSQSET:
			ret = __get_user(scullvm_qset, (int __user *)arg);
			break;

		case SCULLVM_IOCTQSET:
			scullvm_qset = arg;
			break;

		case SCULLVM_IOCGQSET:
			ret = __put_user(scullvm_qset, (int __user *)arg);
			break;

		case SCULLVM_IOCQQSET:
			return scullvm_qset;

		case SCULLVM_IOCXQSET:
			tmp = scullvm_qset;
			ret = __get_user(scullvm_qset, (int __user *)arg);
			if (ret == 0)
				ret = __put_user(tmp, (int __user *)arg);
			break;

		case SCULLVM_IOCHQSET:
			tmp = scullvm_qset;
			scullvm_qset = arg;
			return tmp;

		default:	/* redundant as cmd was checked against MAXNR */
			return -ENOTTY;
	}

	return ret;
}

/*
 * The extended operations
 */
loff_t scullvm_llseek(struct file *filp, loff_t off, int whence)
{
	long newpos;
	struct scullvm_dev *dev	= filp->private_data;

	switch(whence) {
		case 0:	/* SEEK_SET */
			newpos	= off;
			break;

		case 1:	/* SEEK_CUR */
			newpos	= filp->f_pos + off;
			break;

		case 2:	/* SEEK_END */
			newpos	= dev->size + off;
			break;

		default:	/* can't happen??? */
			return -EINVAL;
	}
	if (newpos < 0)
		return -EINVAL;
	filp->f_pos	= newpos;

	return newpos;
}

/*
 * a simple asynchronous I/O implementation.
 */
struct async_work {
	struct kiocb *iocb;
	int result;
	struct work_struct work;
};

struct delayed_work *dwork;	/* for schedule delayed_work */

/* Complete an asynchronous operation */
static void scullvm_do_deferred_op(struct work_struct *work)
{
	struct async_work *p = container_of(work, struct async_work, work);
	aio_complete(p->iocb, p->result, 0);
	kfree(p);
}

static ssize_t scullvm_defer_op(int write, struct kiocb *iocb, char __user *buf,
				size_t count, loff_t pos)
{
	int result;
	struct async_work *stuff;

	/* Copy now while we can access the buffer */
	if (write)
		result = scullvm_write(iocb->ki_filp, buf, count, &pos);
	else
		result = scullvm_read(iocb->ki_filp, buf, count, &pos);

	/* If this is an asynchronous IOCB, we return our status now */
	if (is_sync_kiocb(iocb))
		return result;

	/* otherwise defer the completion for a few milliseconds */
	stuff = kmalloc(sizeof(*stuff), GFP_KERNEL);
	if (stuff == NULL)
		return result;	/* No memory, just complete now */
	stuff->iocb	= iocb;
	stuff->result	= result;
	dwork->work	= stuff->work;
	INIT_WORK(&stuff->work, scullvm_do_deferred_op);
	schedule_delayed_work(dwork, HZ/100);

	return -EIOCBQUEUED;
}

static ssize_t scullvm_aio_read(struct kiocb *iocb, char __user *buf,
				size_t count, loff_t pos)
{
	return scullvm_defer_op(0, iocb, buf, count, pos);
}

static ssize_t scullvm_aio_write(struct kiocb *iocb, const char __user *buf,
				 size_t count, loff_t pos)
{
	return scullvm_defer_op(1, iocb, (char __user *) buf, count, pos);
}

extern int scullvm_mmap(struct file *filp, struct vm_area_struct *vma);

struct file_operations scullvm_fops = {
	.owner		= THIS_MODULE,
	.llseek		= scullvm_llseek,
	.read		= scullvm_read,
	.write		= scullvm_write,
	.unlocked_ioctl	= scullvm_ioctl,
	.mmap		= scullvm_mmap,
	.open		= scullvm_open,
	.release	= scullvm_release,
	.aio_read	= scullvm_aio_read,
	.aio_write	= scullvm_aio_write,
};

int scullvm_trim(struct scullvm_dev *dev)
{
	int i;
	struct scullvm_dev *next, *dptr;
	int qset	= dev->qset;	/* assume `dev` not null */

	if (dev->vmas)	/* don't trim: there are active mappings */
		return -EBUSY;

	for (dptr = dev; dptr; dptr = next) {	/* all the list items */
		if (dptr->data) {
			/* Release the quantum-set */
			for (i = 0; i < qset; i++)
				if (dptr->data[i])
					vfree(dptr->data[i]);
			kfree(dptr->data);
			dptr->data	= NULL;
		}
		next	 = dptr->next;
		if (dptr != dev)
			kfree(dptr);	/* all but the last */
	}
	dev->size	= 0;
	dev->qset	= scullvm_qset;
	dev->order	= scullvm_order;
	dev->next	= NULL;

	return 0;
}

static void scullvm_setup_cdev(struct scullvm_dev *dev, int index)
{
	int err;
	int devno	= MKDEV(scullvm_major, index);

	if (dev	== NULL) {
		pr_info("dev is null");
		return;
	}
	cdev_init(&dev->cdev, &scullvm_fops);
	dev->cdev.owner	= THIS_MODULE;
	dev->cdev.ops	= &scullvm_fops;

	err = cdev_add(&dev->cdev, devno, 1);
	if (err)
		pr_info("Error %d adding scull%d", err, index);
}

int scullvm_init(void)
{
	int result, i;
	dev_t dev	= MKDEV(scullvm_major, 0);

	/* Register your major and accept a dynamic number */
	if (scullvm_major)
		result = register_chrdev_region(dev, scullvm_devs, "scullvm");
	else {
		result = alloc_chrdev_region(&dev, 0, scullvm_devs, "scullvm");
		scullvm_major = MAJOR(dev);
	}

	if (result < 0)
		return result;
	if ((sc = class_create(THIS_MODULE, "scullvm")) == NULL) {
		goto fail_init;
	}

	if (device_create(sc, NULL, dev, NULL, "scullvm") == NULL) {
		class_destroy(sc);
		goto fail_init;
	}

	/*
	 * Allocate the devices -- we can't have them static, as the number can
	 * be specified at load time
	 */
	scullvm_devices	= kmalloc(scullvm_devs * sizeof(struct scullvm_dev),
				  GFP_KERNEL);
	if (!scullvm_devices) {
		result = -ENOMEM;
		goto fail_init;
	}

	memset(scullvm_devices, 0, scullvm_devs * sizeof(struct scullvm_dev));
	for (i = 0; i < scullvm_devs; i++) {
		scullvm_devices[i].order	= scullvm_order;
		scullvm_devices[i].qset		= scullvm_qset;
		sema_init(&scullvm_devices[i].sem, 1);
		scullvm_setup_cdev(scullvm_devices + i, i);
	}

	return 0;

fail_init:
	unregister_chrdev_region(dev, scullvm_devs);
	return result;
}

void scullvm_cleanup(void)
{
	int i;
	dev_t dev = MKDEV(scullvm_major, 0);

	for (i = 0; i < scullvm_devs; i++) {
		cdev_del(&scullvm_devices[i].cdev);
		scullvm_trim(scullvm_devices + i);
	}
	kfree(scullvm_devices);
	device_destroy(sc, dev);
	class_destroy(sc);
	unregister_chrdev_region(MKDEV(scullvm_major, 0), scullvm_devs);
}

module_init(scullvm_init);
module_exit(scullvm_cleanup);
