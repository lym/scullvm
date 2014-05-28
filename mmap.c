/*
 * mmap.c -- memory mapping for the scullvm character module
 */

#include <linux/module.h>
#include <linux/mm.h>
#include <linux/errno.h>
#include <asm/pgtable.h>
#include <linux/fs.h>

#include "scullvm.h"

void scullvm_vma_open(struct vm_area_struct *vma)
{
	struct scullvm_dev *dev	= vma->vm_private_data;
	dev->vmas++;
}

void scullvm_vma_close(struct vm_area_struct *vma)
{
	struct scullvm_dev *dev	= vma->vm_private_data;
	dev->vmas--;
}

static int scullvm_vma_fault(struct vm_area_struct *vma, struct vm_fault *vmf)
{
	unsigned long offset;
	struct page *page;

	struct scullvm_dev *ptr;
	struct scullvm_dev *dev	= vma->vm_private_data;
	void *pageptr		= NULL;

	down(&dev->sem);
	offset = (unsigned long)vmf->virtual_address - vma->vm_start;
	if (offset >= dev->size)
		goto out;

	offset >>= PAGE_SHIFT;
	for (ptr = dev; ptr && offset >= dev->qset;) {
		ptr	= ptr->next;
		offset -= dev->qset;
	}

	if (ptr && ptr->data)
		pageptr = ptr->data[offset];
	if (!pageptr)
		goto out;
	page	= virt_to_page(pageptr);

	get_page(page);
	vmf->page	= page;

	up(&dev->sem);
	return 0;

out:
	up(&dev->sem);
	return (int)page;
}

struct vm_operations_struct scullvm_vm_ops = {
	.open	= scullvm_vma_open,
	.close	= scullvm_vma_close,
	.fault	= scullvm_vma_fault,
};

int scullvm_mmap(struct file *filp, struct vm_area_struct *vma)
{

	vma->vm_ops	= &scullvm_vm_ops;
	vma->vm_flags  |= (VM_DONTEXPAND | VM_DONTDUMP);
	vma->vm_private_data	= filp->private_data;
	scullvm_vma_open(vma);

	return 0;
}
