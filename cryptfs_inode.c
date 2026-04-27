// SPDX-License-Identifier: GPL-2.0
/*
 * cryptfs: inode_operations для каталогов, файлов и символических ссылок.
 *
 * Целевое ядро — Linux 5.15: в inode_operations живёт
 * struct user_namespace *mnt_userns, unlink/rmdir ещё без userns,
 * generic_fillattr принимает (userns, inode, stat) без request_mask,
 * время в inode читается/пишется прямым обращением к полям i_[acm]time.
 */

#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/mount.h>
#include <linux/uidgid.h>
#include <linux/user_namespace.h>
#include <linux/xattr.h>
#include <linux/slab.h>

#include "cryptfs.h"

/* ---------- вспомогательные функции ---------- */

void cryptfs_copy_attr(struct inode *dest, struct inode *src)
{
	dest->i_mode    = src->i_mode;
	dest->i_uid     = src->i_uid;
	dest->i_gid     = src->i_gid;
	dest->i_rdev    = src->i_rdev;
	dest->i_blkbits = src->i_blkbits;
	dest->i_flags   = src->i_flags;
	dest->i_blocks  = src->i_blocks;
	i_size_write(dest, i_size_read(src));
	set_nlink(dest, src->i_nlink);

	dest->i_atime = src->i_atime;
	dest->i_mtime = src->i_mtime;
	dest->i_ctime = src->i_ctime;
}

static struct cryptfs_dentry_info *
cryptfs_alloc_dentry_info(struct dentry *upper, struct dentry *lower_dentry,
			  struct vfsmount *lower_mnt)
{
	struct cryptfs_dentry_info *di;

	di = kzalloc(sizeof(*di), GFP_KERNEL);
	if (!di)
		return NULL;

	di->lower_path.mnt    = mntget(lower_mnt);
	di->lower_path.dentry = lower_dentry; /* поглощает переданный ref */
	upper->d_fsdata = di;
	return di;
}

/* ---------- inode lifecycle ---------- */

static int cryptfs_inode_test(struct inode *inode, void *data)
{
	struct inode *lower = data;
	return CRYPTFS_I(inode)->lower_inode == lower;
}

static int cryptfs_inode_set(struct inode *inode, void *data)
{
	struct inode *lower = data;

	/* держим ссылку на lower inode, пока жив upper */
	CRYPTFS_I(inode)->lower_inode = igrab(lower);
	inode->i_ino        = lower->i_ino;
	inode->i_generation = lower->i_generation;
	cryptfs_copy_attr(inode, lower);
	return 0;
}

struct inode *cryptfs_iget(struct super_block *sb, struct inode *lower)
{
	struct inode *inode;

	inode = iget5_locked(sb, (unsigned long)lower,
			     cryptfs_inode_test, cryptfs_inode_set, lower);
	if (!inode)
		return ERR_PTR(-ENOMEM);

	if (inode->i_state & I_NEW) {
		if (S_ISDIR(lower->i_mode)) {
			inode->i_op  = &cryptfs_dir_iops;
			inode->i_fop = &cryptfs_dir_fops;
		} else if (S_ISLNK(lower->i_mode)) {
			inode->i_op  = &cryptfs_symlink_iops;
		} else if (S_ISREG(lower->i_mode)) {
			inode->i_op  = &cryptfs_file_iops;
			inode->i_fop = &cryptfs_file_fops;
		} else {
			init_special_inode(inode, lower->i_mode, lower->i_rdev);
		}
		unlock_new_inode(inode);
	} else {
		cryptfs_copy_attr(inode, lower);
	}

	return inode;
}

/* ---------- directory inode_operations ---------- */

static struct dentry *cryptfs_lookup(struct inode *dir, struct dentry *dentry,
				     unsigned int flags)
{
	struct dentry *parent = dentry->d_parent;
	struct path *lower_parent = cryptfs_lower_path(parent);
	struct dentry *lower_dentry;
	struct inode *inode;

	if (!lower_parent || !lower_parent->dentry)
		return ERR_PTR(-EINVAL);

	lower_dentry = lookup_one_len_unlocked(dentry->d_name.name,
					       lower_parent->dentry,
					       dentry->d_name.len);
	if (IS_ERR(lower_dentry))
		return ERR_CAST(lower_dentry);

	if (!cryptfs_alloc_dentry_info(dentry, lower_dentry,
				       lower_parent->mnt)) {
		dput(lower_dentry);
		return ERR_PTR(-ENOMEM);
	}

	if (d_really_is_negative(lower_dentry)) {
		d_add(dentry, NULL);
		return NULL;
	}

	inode = cryptfs_iget(dir->i_sb, d_inode(lower_dentry));
	if (IS_ERR(inode))
		return ERR_CAST(inode);

	d_add(dentry, inode);
	return NULL;
}

static int cryptfs_create(struct user_namespace *mnt_userns, struct inode *dir,
			  struct dentry *dentry, umode_t mode, bool excl)
{
	struct inode *lower_dir  = CRYPTFS_I(dir)->lower_inode;
	struct dentry *lower_dentry = cryptfs_lower_dentry(dentry);
	struct inode *inode;
	int err;

	if (!lower_dir || !lower_dentry)
		return -EINVAL;

	inode_lock_nested(lower_dir, I_MUTEX_PARENT);
	err = vfs_create(&init_user_ns, lower_dir, lower_dentry, mode, excl);
	inode_unlock(lower_dir);
	if (err)
		return err;

	inode = cryptfs_iget(dir->i_sb, d_inode(lower_dentry));
	if (IS_ERR(inode))
		return PTR_ERR(inode);

	d_instantiate(dentry, inode);
	cryptfs_copy_attr(dir, lower_dir);
	return 0;
}

static int cryptfs_mkdir(struct user_namespace *mnt_userns, struct inode *dir,
			 struct dentry *dentry, umode_t mode)
{
	struct inode *lower_dir  = CRYPTFS_I(dir)->lower_inode;
	struct dentry *lower_dentry = cryptfs_lower_dentry(dentry);
	struct inode *inode;
	int err;

	if (!lower_dir || !lower_dentry)
		return -EINVAL;

	inode_lock_nested(lower_dir, I_MUTEX_PARENT);
	err = vfs_mkdir(&init_user_ns, lower_dir, lower_dentry, mode);
	inode_unlock(lower_dir);
	if (err)
		return err;

	inode = cryptfs_iget(dir->i_sb, d_inode(lower_dentry));
	if (IS_ERR(inode))
		return PTR_ERR(inode);

	d_instantiate(dentry, inode);
	cryptfs_copy_attr(dir, lower_dir);
	return 0;
}

static int cryptfs_unlink(struct inode *dir, struct dentry *dentry)
{
	struct inode *lower_dir  = CRYPTFS_I(dir)->lower_inode;
	struct dentry *lower_dentry = cryptfs_lower_dentry(dentry);
	int err;

	if (!lower_dir || !lower_dentry)
		return -EINVAL;

	dget(lower_dentry);
	inode_lock_nested(lower_dir, I_MUTEX_PARENT);
	err = vfs_unlink(&init_user_ns, lower_dir, lower_dentry, NULL);
	inode_unlock(lower_dir);

	if (!err) {
		cryptfs_copy_attr(dir, lower_dir);
		if (d_inode(dentry))
			set_nlink(d_inode(dentry),
				  d_inode(lower_dentry)->i_nlink);
	}
	dput(lower_dentry);
	return err;
}

static int cryptfs_rmdir(struct inode *dir, struct dentry *dentry)
{
	struct inode *lower_dir  = CRYPTFS_I(dir)->lower_inode;
	struct dentry *lower_dentry = cryptfs_lower_dentry(dentry);
	int err;

	if (!lower_dir || !lower_dentry)
		return -EINVAL;

	dget(lower_dentry);
	inode_lock_nested(lower_dir, I_MUTEX_PARENT);
	err = vfs_rmdir(&init_user_ns, lower_dir, lower_dentry);
	inode_unlock(lower_dir);

	if (!err)
		cryptfs_copy_attr(dir, lower_dir);
	dput(lower_dentry);
	return err;
}

static int cryptfs_setattr(struct user_namespace *mnt_userns,
			   struct dentry *dentry, struct iattr *ia)
{
	struct dentry *lower_dentry = cryptfs_lower_dentry(dentry);
	struct inode *inode = d_inode(dentry);
	struct inode *lower;
	struct iattr lower_ia;
	int err;

	if (!inode || !lower_dentry)
		return -EINVAL;

	lower = CRYPTFS_I(inode)->lower_inode;
	if (!lower)
		return -EINVAL;

	err = setattr_prepare(&init_user_ns, dentry, ia);
	if (err)
		return err;

	lower_ia = *ia;
	if (ia->ia_valid & ATTR_FILE)
		lower_ia.ia_file = cryptfs_lower_file(ia->ia_file);

	inode_lock(lower);
	err = notify_change(&init_user_ns, lower_dentry, &lower_ia, NULL);
	inode_unlock(lower);

	if (!err)
		cryptfs_copy_attr(inode, lower);
	return err;
}

static int cryptfs_getattr(struct user_namespace *mnt_userns,
			   const struct path *path, struct kstat *stat,
			   u32 request_mask, unsigned int flags)
{
	struct inode *inode = d_inode(path->dentry);
	struct inode *lower = CRYPTFS_I(inode)->lower_inode;

	if (lower)
		cryptfs_copy_attr(inode, lower);
	generic_fillattr(&init_user_ns, inode, stat);
	return 0;
}

static int cryptfs_permission(struct user_namespace *mnt_userns,
			      struct inode *inode, int mask)
{
	struct inode *lower = CRYPTFS_I(inode)->lower_inode;

	if (!lower)
		return generic_permission(&init_user_ns, inode, mask);
	return inode_permission(&init_user_ns, lower, mask);
}

const struct inode_operations cryptfs_dir_iops = {
	.lookup     = cryptfs_lookup,
	.create     = cryptfs_create,
	.mkdir      = cryptfs_mkdir,
	.unlink     = cryptfs_unlink,
	.rmdir      = cryptfs_rmdir,
	.setattr    = cryptfs_setattr,
	.getattr    = cryptfs_getattr,
	.permission = cryptfs_permission,
};

const struct inode_operations cryptfs_file_iops = {
	.setattr    = cryptfs_setattr,
	.getattr    = cryptfs_getattr,
	.permission = cryptfs_permission,
};

const struct inode_operations cryptfs_symlink_iops = {
	.setattr    = cryptfs_setattr,
	.getattr    = cryptfs_getattr,
	.permission = cryptfs_permission,
};
