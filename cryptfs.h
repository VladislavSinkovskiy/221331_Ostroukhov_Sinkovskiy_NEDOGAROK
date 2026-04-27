/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _CRYPTFS_H
#define _CRYPTFS_H

#include <linux/fs.h>
#include <linux/path.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/dcache.h>
#include <linux/mount.h>
#include <linux/version.h>
#include <linux/fs_parser.h>

#include "cryptfs_uapi.h"

#define CRYPTFS_NAME         "cryptfs"
#define CRYPTFS_MAGIC        0x43525950  /* "CRYP" */
#define CRYPTFS_SECTOR_SIZE  512U
#define CRYPTFS_SECTOR_MASK  (CRYPTFS_SECTOR_SIZE - 1)

/* Per-mount data */
struct cryptfs_sb_info {
	struct path lower_path;   /* корень нижней ФС (mnt + dentry) */
};

/* Options, живущие в fs_context до конца монтирования */
struct cryptfs_fs_context {
	char *lowerdir;
};

/* Per-inode data: upper inode включает ссылку на lower inode */
struct cryptfs_inode_info {
	struct inode *lower_inode;
	struct inode  vfs_inode;
};

/* Per-dentry data: нижний path для каждого upper-dentry */
struct cryptfs_dentry_info {
	struct path lower_path;
};

/* Per-open-file data */
struct cryptfs_file_info {
	struct file *lower_file;
};

static inline struct cryptfs_sb_info *CRYPTFS_SB(struct super_block *sb)
{
	return sb->s_fs_info;
}

static inline struct cryptfs_inode_info *CRYPTFS_I(struct inode *inode)
{
	return container_of(inode, struct cryptfs_inode_info, vfs_inode);
}

static inline struct cryptfs_dentry_info *CRYPTFS_D(struct dentry *d)
{
	return (struct cryptfs_dentry_info *)d->d_fsdata;
}

static inline struct file *cryptfs_lower_file(struct file *f)
{
	struct cryptfs_file_info *fi = f->private_data;
	return fi ? fi->lower_file : NULL;
}

static inline struct dentry *cryptfs_lower_dentry(struct dentry *d)
{
	return CRYPTFS_D(d)->lower_path.dentry;
}

static inline struct path *cryptfs_lower_path(struct dentry *d)
{
	return &CRYPTFS_D(d)->lower_path;
}

extern struct kmem_cache *cryptfs_inode_cachep;
extern const struct fs_parameter_spec cryptfs_fs_parameters[];

/* cryptfs_super.c */
int cryptfs_init_fs_context(struct fs_context *fc);
int cryptfs_fill_super(struct super_block *sb, struct fs_context *fc);
extern const struct super_operations cryptfs_sops;
extern const struct dentry_operations cryptfs_dops;

/* cryptfs_inode.c */
struct inode *cryptfs_iget(struct super_block *sb, struct inode *lower);
void cryptfs_copy_attr(struct inode *dest, struct inode *src);
extern const struct inode_operations cryptfs_dir_iops;
extern const struct inode_operations cryptfs_file_iops;
extern const struct inode_operations cryptfs_symlink_iops;

/* cryptfs_file.c */
extern const struct file_operations cryptfs_file_fops;
extern const struct file_operations cryptfs_dir_fops;

/* cryptfs_crypto.c */
int  cryptfs_crypto_init(void);
void cryptfs_crypto_destroy(void);
int  cryptfs_set_key(const u8 *key, size_t len);
bool cryptfs_has_key(void);
int  cryptfs_crypt_range(bool encrypt, void *data, size_t len, loff_t offset);

/* cryptfs_ctl.c */
int  cryptfs_ctl_register(void);
void cryptfs_ctl_unregister(void);

#endif /* _CRYPTFS_H */
