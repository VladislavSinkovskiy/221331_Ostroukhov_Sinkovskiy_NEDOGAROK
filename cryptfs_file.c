// SPDX-License-Identifier: GPL-2.0
/*
 * cryptfs: file_operations для файлов и каталогов.
 *
 * Шифрование — AES-XTS по секторам CRYPTFS_SECTOR_SIZE (512 байт).
 * read_iter/write_iter используют read-modify-write, чтобы корректно
 * обрабатывать частично перекрывающиеся секторы на границах запроса.
 */

#include <linux/fs.h>
#include <linux/file.h>
#include <linux/uio.h>
#include <linux/slab.h>
#include <linux/namei.h>
#include <linux/mount.h>
#include <linux/uaccess.h>
#include <linux/math.h>

#include "cryptfs.h"

/* ---------- общие: open/release/flush/fsync/llseek ---------- */

static int cryptfs_open(struct inode *inode, struct file *file)
{
	struct cryptfs_file_info *fi;
	struct path *lower_path;
	struct file *lower_file;

	fi = kzalloc(sizeof(*fi), GFP_KERNEL);
	if (!fi)
		return -ENOMEM;

	lower_path = cryptfs_lower_path(file->f_path.dentry);
	if (!lower_path || !lower_path->dentry) {
		kfree(fi);
		return -ENOENT;
	}

	path_get(lower_path);                                                             
        {                                                                                                  
                int lower_flags = file->f_flags;                                                           
                if (lower_flags & O_WRONLY) {                                                              
                        lower_flags &= ~O_WRONLY;                                                          
                        lower_flags |= O_RDWR;                                                             
                }
                lower_file = dentry_open(lower_path, lower_flags, current_cred());                         
        }         
    path_put(lower_path);
	if (IS_ERR(lower_file)) {
		kfree(fi);
		return PTR_ERR(lower_file);
	}

	fi->lower_file = lower_file;
	file->private_data = fi;
	return 0;
}

static int cryptfs_release(struct inode *inode, struct file *file)
{
	struct cryptfs_file_info *fi = file->private_data;

	if (fi) {
		if (fi->lower_file)
			fput(fi->lower_file);
		kfree(fi);
		file->private_data = NULL;
	}
	return 0;
}

static int cryptfs_flush(struct file *file, fl_owner_t id)
{
	struct file *lower = cryptfs_lower_file(file);

	if (!lower || !lower->f_op || !lower->f_op->flush)
		return 0;
	return lower->f_op->flush(lower, id);
}

static int cryptfs_fsync(struct file *file, loff_t start, loff_t end,
			 int datasync)
{
	struct file *lower = cryptfs_lower_file(file);

	if (!lower)
		return -EBADF;
	return vfs_fsync_range(lower, start, end, datasync);
}

static loff_t cryptfs_llseek(struct file *file, loff_t offset, int whence)
{
	struct file *lower = cryptfs_lower_file(file);
	loff_t rv;

	if (!lower)
		return -EBADF;
	rv = vfs_llseek(lower, offset, whence);
	if (rv >= 0)
		file->f_pos = rv;
	return rv;
}

/* ---------- каталог: iterate_shared ---------- */

static int cryptfs_iterate_shared(struct file *file, struct dir_context *ctx)
{
	struct file *lower = cryptfs_lower_file(file);
	int err;

	if (!lower)
		return -EBADF;

	lower->f_pos = file->f_pos;
	err = iterate_dir(lower, ctx);
	file->f_pos = lower->f_pos;
	return err;
}

/* ---------- read_iter с расшифровкой ---------- */

static ssize_t cryptfs_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
	struct file *upper = iocb->ki_filp;
	struct file *lower = cryptfs_lower_file(upper);
	struct inode *upper_inode = file_inode(upper);
	struct inode *lower_inode;
	loff_t pos   = iocb->ki_pos;
	size_t count = iov_iter_count(to);
	loff_t aligned_start, aligned_end;
	size_t aligned_len, skip, avail, want, dec_len;
	void *buf = NULL;
	loff_t lpos;
	ssize_t n;
	int err;

	if (!count)
		return 0;
	if (!lower)
		return -EBADF;

	lower_inode = CRYPTFS_I(upper_inode)->lower_inode;

	aligned_start = round_down(pos, CRYPTFS_SECTOR_SIZE);
	aligned_end   = round_up((u64)pos + count, CRYPTFS_SECTOR_SIZE);
	aligned_len   = aligned_end - aligned_start;

	buf = kvzalloc(aligned_len, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	lpos = aligned_start;
	n = kernel_read(lower, buf, aligned_len, &lpos);
	if (n < 0) {
		err = n;
		goto out_err;
	}

	if (n > 0) {
		dec_len = round_up((size_t)n, CRYPTFS_SECTOR_SIZE);
		if (dec_len > aligned_len)
			dec_len = aligned_len;
		err = cryptfs_crypt_range(false, buf, dec_len, aligned_start);
		if (err)
			goto out_err;
	}

	skip  = pos - aligned_start;
	avail = ((size_t)n > skip) ? (size_t)n - skip : 0;
	want  = min(count, avail);

	if (want > 0) {
		if (copy_to_iter(buf + skip, want, to) != want) {
			err = -EFAULT;
			goto out_err;
		}
	}

	iocb->ki_pos = pos + want;
	if (lower_inode)
		cryptfs_copy_attr(upper_inode, lower_inode);

	kvfree(buf);
	return want;

out_err:
	kvfree(buf);
	return err;
}

/* ---------- write_iter с шифрованием (read-modify-write) ---------- */

static ssize_t cryptfs_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	struct file *upper = iocb->ki_filp;
	struct file *lower = cryptfs_lower_file(upper);
	struct inode *upper_inode = file_inode(upper);
	struct inode *lower_inode;
	loff_t pos   = iocb->ki_pos;
	size_t count = iov_iter_count(from);
	loff_t aligned_start, aligned_end;
	size_t aligned_len, skip, dec_len;
	void *buf = NULL;
	loff_t lpos;
	ssize_t n;
	int err;

	if (!count)
		return 0;
	if (!lower)
		return -EBADF;

	lower_inode = CRYPTFS_I(upper_inode)->lower_inode;

	if (upper->f_flags & O_APPEND) {
		pos = i_size_read(upper_inode);
		iocb->ki_pos = pos;
	}

	aligned_start = round_down(pos, CRYPTFS_SECTOR_SIZE);
	aligned_end   = round_up((u64)pos + count, CRYPTFS_SECTOR_SIZE);
	aligned_len   = aligned_end - aligned_start;

	buf = kvzalloc(aligned_len, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	/* Read-modify-write: читаем уже записанные секторы (если есть). */
	lpos = aligned_start;
	n = kernel_read(lower, buf, aligned_len, &lpos);
	if (n < 0) {
		err = n;
		goto out_err;
	}

	if (n > 0) {
		dec_len = round_up((size_t)n, CRYPTFS_SECTOR_SIZE);
		if (dec_len > aligned_len)
			dec_len = aligned_len;
		err = cryptfs_crypt_range(false, buf, dec_len, aligned_start);
		if (err)
			goto out_err;
	}

	/* Накладываем новые байты из userspace. */
	skip = pos - aligned_start;
	if (copy_from_iter(buf + skip, count, from) != count) {
		err = -EFAULT;
		goto out_err;
	}

	/* Шифруем весь подготовленный aligned-буфер. */
	err = cryptfs_crypt_range(true, buf, aligned_len, aligned_start);
	if (err)
		goto out_err;

	/* Пишем шифртекст обратно в lower. */
	lpos = aligned_start;
	n = kernel_write(lower, buf, aligned_len, &lpos);
	if (n < 0) {
		err = n;
		goto out_err;
	}

	iocb->ki_pos = pos + count;

	if (lower_inode)
		cryptfs_copy_attr(upper_inode, lower_inode);

	kvfree(buf);
	return count;

out_err:
	kvfree(buf);
	return err;
}

/* mmap пока не поддержан — честно возвращаем ENODEV, чем выдавать
 * шифртекст напрямую из page cache нижней ФС. */
static int cryptfs_mmap(struct file *file, struct vm_area_struct *vma)
{
	return -ENODEV;
}

const struct file_operations cryptfs_file_fops = {
	.owner      = THIS_MODULE,
	.open       = cryptfs_open,
	.release    = cryptfs_release,
	.flush      = cryptfs_flush,
	.fsync      = cryptfs_fsync,
	.llseek     = cryptfs_llseek,
	.read_iter  = cryptfs_read_iter,
	.write_iter = cryptfs_write_iter,
	.mmap       = cryptfs_mmap,
};

const struct file_operations cryptfs_dir_fops = {
	.owner          = THIS_MODULE,
	.open           = cryptfs_open,
	.release        = cryptfs_release,
	.iterate_shared = cryptfs_iterate_shared,
	.llseek         = cryptfs_llseek,
	.fsync          = cryptfs_fsync,
};
