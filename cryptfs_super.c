/*
 * cryptfs: fs_context API, разбор опций, fill_super,
 * super_operations, dentry_operations.
 */

#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/fs_parser.h>
#include <linux/slab.h>
#include <linux/namei.h>
#include <linux/mount.h>
#include <linux/statfs.h>
#include <linux/seq_file.h>
#include <linux/string.h>

#include "cryptfs.h"

enum cryptfs_param {
	Opt_lowerdir,
};

const struct fs_parameter_spec cryptfs_fs_parameters[] = {
	fsparam_string("lowerdir", Opt_lowerdir),
	{}
};

/* ---------- super_operations ---------- */

static struct inode *cryptfs_alloc_inode(struct super_block *sb)
{
	struct cryptfs_inode_info *ci;

	ci = kmem_cache_alloc(cryptfs_inode_cachep, GFP_KERNEL);
	if (!ci)
		return NULL;
	ci->lower_inode = NULL;
	return &ci->vfs_inode;
}

static void cryptfs_free_inode(struct inode *inode)
{
	kmem_cache_free(cryptfs_inode_cachep, CRYPTFS_I(inode));
}

static void cryptfs_evict_inode(struct inode *inode)
{
	struct inode *lower = CRYPTFS_I(inode)->lower_inode;

	truncate_inode_pages_final(&inode->i_data);
	clear_inode(inode);
	if (lower) {
		iput(lower);
		CRYPTFS_I(inode)->lower_inode = NULL;
	}
}

static int cryptfs_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	struct path *lower = cryptfs_lower_path(dentry);
	int err;

	if (!lower || !lower->dentry)
		return -EINVAL;

	err = vfs_statfs(lower, buf);
	if (!err)
		buf->f_type = CRYPTFS_MAGIC;
	return err;
}

static int cryptfs_show_options(struct seq_file *m, struct dentry *root)
{
	struct cryptfs_sb_info *sbi = CRYPTFS_SB(root->d_sb);

	if (sbi && sbi->lower_path.dentry)
		seq_printf(m, ",lowerdir=%pd4", sbi->lower_path.dentry);
	return 0;
}

const struct super_operations cryptfs_sops = {
	.alloc_inode  = cryptfs_alloc_inode,
	.free_inode   = cryptfs_free_inode,
	.evict_inode  = cryptfs_evict_inode,
	.statfs       = cryptfs_statfs,
	.show_options = cryptfs_show_options,
	.drop_inode   = generic_delete_inode,
};

/* ---------- dentry_operations ---------- */

static void cryptfs_d_release(struct dentry *dentry)
{
	struct cryptfs_dentry_info *di = dentry->d_fsdata;

	if (di) {
		if (di->lower_path.dentry)
			path_put(&di->lower_path);
		kfree(di);
		dentry->d_fsdata = NULL;
	}
}

const struct dentry_operations cryptfs_dops = {
	.d_release = cryptfs_d_release,
};

/* ---------- fill_super ---------- */

int cryptfs_fill_super(struct super_block *sb, struct fs_context *fc)
{
	struct cryptfs_fs_context *ctx = fc->fs_private;
	struct cryptfs_sb_info *sbi = NULL;
	struct cryptfs_dentry_info *root_di = NULL;
	struct path lower_path;
	struct inode *root_inode = NULL;
	int err;

	if (!ctx || !ctx->lowerdir) {
		pr_err("cryptfs: 'lowerdir=' is required\n");
		return -EINVAL;
	}

	err = kern_path(ctx->lowerdir, LOOKUP_FOLLOW | LOOKUP_DIRECTORY,
			&lower_path);
	if (err) {
		pr_err("cryptfs: cannot resolve lowerdir '%s' (err=%d)\n",
		       ctx->lowerdir, err);
		return err;
	}

	sbi = kzalloc(sizeof(*sbi), GFP_KERNEL);
	if (!sbi) {
		path_put(&lower_path);
		return -ENOMEM;
	}

	/* Владение lower_path передаётся в sbi; cryptfs_kill_sb освободит. */
	sbi->lower_path = lower_path;
	sb->s_fs_info   = sbi;

	sb->s_op        = &cryptfs_sops;
	sb->s_d_op      = &cryptfs_dops;
	sb->s_magic     = CRYPTFS_MAGIC;
	sb->s_blocksize = PAGE_SIZE;
	sb->s_blocksize_bits = PAGE_SHIFT;
	sb->s_maxbytes  = MAX_LFS_FILESIZE;
	sb->s_time_gran = 1;

	sb->s_stack_depth = lower_path.dentry->d_sb->s_stack_depth + 1;
	if (sb->s_stack_depth > FILESYSTEM_MAX_STACK_DEPTH) {
		pr_err("cryptfs: maximum stack depth exceeded\n");
		return -EINVAL;
	}

	root_inode = cryptfs_iget(sb, d_inode(lower_path.dentry));
	if (IS_ERR(root_inode))
		return PTR_ERR(root_inode);

	sb->s_root = d_make_root(root_inode);
	if (!sb->s_root)
		return -ENOMEM;

	root_di = kzalloc(sizeof(*root_di), GFP_KERNEL);
	if (!root_di)
		return -ENOMEM;

	root_di->lower_path.mnt    = mntget(lower_path.mnt);
	root_di->lower_path.dentry = dget(lower_path.dentry);
	sb->s_root->d_fsdata = root_di;

	pr_info("cryptfs: mounted over '%pd'\n", lower_path.dentry);
	return 0;
}

/* ---------- fs_context operations ---------- */

static int cryptfs_fc_parse_param(struct fs_context *fc,
				  struct fs_parameter *param)
{
	struct cryptfs_fs_context *ctx = fc->fs_private;
	struct fs_parse_result result;
	int opt;

	opt = fs_parse(fc, cryptfs_fs_parameters, param, &result);
	if (opt < 0)
		return opt;

	switch (opt) {
	case Opt_lowerdir:
		kfree(ctx->lowerdir);
		ctx->lowerdir = kstrdup(param->string, GFP_KERNEL);
		if (!ctx->lowerdir)
			return -ENOMEM;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static int cryptfs_fc_get_tree(struct fs_context *fc)
{
	return get_tree_nodev(fc, cryptfs_fill_super);
}

static int cryptfs_fc_reconfigure(struct fs_context *fc)
{
	/* remount не поддерживаем в MVP */
	return -EINVAL;
}

static void cryptfs_fc_free(struct fs_context *fc)
{
	struct cryptfs_fs_context *ctx = fc->fs_private;

	if (ctx) {
		kfree(ctx->lowerdir);
		kfree(ctx);
		fc->fs_private = NULL;
	}
}

static const struct fs_context_operations cryptfs_context_ops = {
	.parse_param = cryptfs_fc_parse_param,
	.get_tree    = cryptfs_fc_get_tree,
	.reconfigure = cryptfs_fc_reconfigure,
	.free        = cryptfs_fc_free,
};

int cryptfs_init_fs_context(struct fs_context *fc)
{
	struct cryptfs_fs_context *ctx;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	fc->fs_private = ctx;
	fc->ops        = &cryptfs_context_ops;
	return 0;
}
