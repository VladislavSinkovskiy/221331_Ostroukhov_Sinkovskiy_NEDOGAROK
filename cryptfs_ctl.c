/*
 * cryptfs: misc-устройство /dev/cryptfs_ctl для управления ключом.
 */

#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/module.h>
#include <linux/string.h>

#include "cryptfs.h"

static long cryptfs_ctl_ioctl(struct file *file, unsigned int cmd,
			      unsigned long arg)
{
	struct cryptfs_key k;
	int err;

	switch (cmd) {
	case CRYPTFS_IOC_SETKEY:
		if (copy_from_user(&k, (void __user *)arg, sizeof(k)))
			return -EFAULT;
		err = cryptfs_set_key(k.key, CRYPTFS_KEY_SIZE);
		memzero_explicit(&k, sizeof(k));
		return err;

	case CRYPTFS_IOC_CLEARKEY:
		/* Полноценный «забыть ключ» требует разрушить tfm и
		 * отказывать последующим I/O. В MVP мы не поддерживаем
		 * это честно, чтобы не провоцировать use-after-free. */
		return -EOPNOTSUPP;

	default:
		return -ENOTTY;
	}
}

static const struct file_operations cryptfs_ctl_fops = {
	.owner          = THIS_MODULE,
	.unlocked_ioctl = cryptfs_ctl_ioctl,
	.compat_ioctl   = compat_ptr_ioctl,
	.open           = simple_open,
	.llseek         = no_llseek,
};

static struct miscdevice cryptfs_ctl_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name  = "cryptfs_ctl",
	.fops  = &cryptfs_ctl_fops,
	.mode  = 0600,
};

int cryptfs_ctl_register(void)
{
	int err = misc_register(&cryptfs_ctl_misc);

	if (err)
		pr_err("cryptfs: failed to register /dev/cryptfs_ctl: %d\n",
		       err);
	else
		pr_info("cryptfs: /dev/cryptfs_ctl registered\n");
	return err;
}

void cryptfs_ctl_unregister(void)
{
	misc_deregister(&cryptfs_ctl_misc);
}
