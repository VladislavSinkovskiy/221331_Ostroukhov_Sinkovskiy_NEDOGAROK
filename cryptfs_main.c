// SPDX-License-Identifier: GPL-2.0
/*
 * cryptfs: stackable filesystem with transparent AES-XTS encryption.
 *
 * Модуль регистрирует файловую систему "cryptfs", которая накладывается
 * поверх произвольного каталога нижней ФС (lowerdir=...), прозрачно
 * шифруя содержимое файлов при записи и расшифровывая при чтении.
 *
 * Целевое ядро: Linux 5.15 LTS (Ubuntu 22.04).
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/slab.h>

#include "cryptfs.h"

struct kmem_cache *cryptfs_inode_cachep;

static void cryptfs_kill_sb(struct super_block *sb)
{
	struct cryptfs_sb_info *sbi = CRYPTFS_SB(sb);

	kill_anon_super(sb);

	if (sbi) {
		if (sbi->lower_path.dentry)
			path_put(&sbi->lower_path);
		kfree(sbi);
	}
}

static struct file_system_type cryptfs_fs_type = {
	.owner           = THIS_MODULE,
	.name            = CRYPTFS_NAME,
	.init_fs_context = cryptfs_init_fs_context,
	.parameters      = cryptfs_fs_parameters,
	.kill_sb         = cryptfs_kill_sb,
	.fs_flags        = 0,  /* stackable: нет собственного backing device */
};

static void cryptfs_inode_init_once(void *obj)
{
	struct cryptfs_inode_info *ci = obj;
	inode_init_once(&ci->vfs_inode);
}

static int __init cryptfs_init(void)
{
	int err;

	cryptfs_inode_cachep = kmem_cache_create("cryptfs_inode_cache",
			sizeof(struct cryptfs_inode_info), 0,
			SLAB_RECLAIM_ACCOUNT | SLAB_ACCOUNT,
			cryptfs_inode_init_once);
	if (!cryptfs_inode_cachep)
		return -ENOMEM;

	err = cryptfs_crypto_init();
	if (err)
		goto err_cache;

	err = cryptfs_ctl_register();
	if (err)
		goto err_crypto;

	err = register_filesystem(&cryptfs_fs_type);
	if (err)
		goto err_ctl;

	pr_info("cryptfs: loaded\n");
	return 0;

err_ctl:
	cryptfs_ctl_unregister();
err_crypto:
	cryptfs_crypto_destroy();
err_cache:
	kmem_cache_destroy(cryptfs_inode_cachep);
	cryptfs_inode_cachep = NULL;
	return err;
}

static void __exit cryptfs_exit(void)
{
	unregister_filesystem(&cryptfs_fs_type);
	cryptfs_ctl_unregister();
	cryptfs_crypto_destroy();
	rcu_barrier();  /* дождаться отложенного освобождения inode */
	kmem_cache_destroy(cryptfs_inode_cachep);
	pr_info("cryptfs: unloaded\n");
}

module_init(cryptfs_init);
module_exit(cryptfs_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("cryptfs team");
MODULE_DESCRIPTION("Stackable filesystem with transparent AES-XTS encryption");
MODULE_VERSION("0.1");
