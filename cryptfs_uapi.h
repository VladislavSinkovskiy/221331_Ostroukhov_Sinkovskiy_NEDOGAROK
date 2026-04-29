#ifndef _CRYPTFS_UAPI_H
#define _CRYPTFS_UAPI_H

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/ioctl.h>
#else
#include <stdint.h>
#include <sys/ioctl.h>
typedef uint8_t __u8;
#endif

/* AES-256-XTS требует два 256-битных ключа => 64 байта. */
#define CRYPTFS_KEY_SIZE 64

struct cryptfs_key {
	__u8 key[CRYPTFS_KEY_SIZE];
};

#define CRYPTFS_IOC_MAGIC     'C'
#define CRYPTFS_IOC_SETKEY    _IOW(CRYPTFS_IOC_MAGIC, 1, struct cryptfs_key)
#define CRYPTFS_IOC_CLEARKEY  _IO(CRYPTFS_IOC_MAGIC, 2)

#endif