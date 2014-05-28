#include <linux/ioctl.h>
#include <linux/cdev.h>
#include <linux/semaphore.h>

/* Debugging */
#undef PDEBUG
#ifdef SCULLVM_DEBUG
#  ifdef __KERNEL__
#    define PDEBUG(fmt, args...) printk(KERN_DEBUG "scullvm: "fmt, ## args)
#  else
     /* and in user-space */
#    define PDEBUG(fmt, args...)
#  endif
#else
#  define PDEBUG(fmt, args...)
#endif

#undef PDEBUGG
#define PDEBUGG(fmt, args...)

#define SCULLVM_MAJOR	0
#define SCULLVM_DEVS	4

/*
 * The bare device is a variable-length region of memory. Use a linked
 * list of indirect blocks.
 *
 * "scullvm_dev->data" points to an array of pointers, each pointer refers
 * to a memory page.
 *
 * The array (quantum-set) is SCULLVM_QSET long.
 */
#define SCULLVM_ORDER	4	/* 16 pages at a time */
#define SCULLVM_QSET	500

struct scullvm_dev {
	void **data;
	struct scullvm_dev *next;	/* next list-item */
	int vmas;			/* active mappings */
	int order;			/* current allocation order */
	int qset;			/* current array size */
	size_t size;			/* 32-bit will suffice */
	struct semaphore sem;
	struct cdev cdev;
};

extern struct scullvm_dev *scullvm_devices;
extern struct file_operations scullvm_fops;

/* configurable parameters */
extern int scullvm_major;
extern int scullvm_devs;
extern int scullvm_order;
extern int scullvm_qset;

int scullvm_trim(struct scullvm_dev *dev);
struct scullvm_dev *scullvm_follow(struct scullvm_dev *dev, int n);

#ifdef SCULLVM_DEBUG
#  define SCULLVM_USE_PROC
#endif

/* ioctl */

/* Use 0x81 as magic number */
#define SCULLVM_IOC_MAGIC	0x81		/* 00-0C */

#define SCULLVM_IOCRESET	_IO(SCULLVM_IOC_MAGIC, 0)

/*
 * S means "Set" through a pointer,
 * T means "Tell" directly
 * G means "Get" (to a "pointed to" var)
 * Q means "Query", response is on return value
 * X means "eXchange": G and S atomically
 * H means "sHift": T and Q atomically
 */
#define SCULLVM_IOCSORDER	_IOW(SCULLVM_IOC_MAGIC,  1, int)
#define SCULLVM_IOCTORDER	_IO(SCULLVM_IOC_MAGIC,   2)
#define SCULLVM_IOCGORDER	_IOR(SCULLVM_IOC_MAGIC,  3, int)
#define SCULLVM_IOCQORDER	_IO(SCULLVM_IOC_MAGIC,   4)
#define SCULLVM_IOCXORDER	_IOWR(SCULLVM_IOC_MAGIC, 5, int)
#define SCULLVM_IOCHORDER	_IO(SCULLVM_IOC_MAGIC,   6)
#define SCULLVM_IOCSQSET	_IOW(SCULLVM_IOC_MAGIC,  7, int)
#define SCULLVM_IOCTQSET	_IO(SCULLVM_IOC_MAGIC,   8)
#define SCULLVM_IOCGQSET	_IOR(SCULLVM_IOC_MAGIC,  9, int)
#define SCULLVM_IOCQQSET	_IO(SCULLVM_IOC_MAGIC,  10)
#define SCULLVM_IOCXQSET	_IOWR(SCULLVM_IOC_MAGIC, 11, int)
#define SCULLVM_IOCHQSET	_IO(SCULLVM_IOC_MAGIC,   12)

#define SCULLVM_IOC_MAXNR	12
