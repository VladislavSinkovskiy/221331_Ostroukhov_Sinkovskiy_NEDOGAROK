// SPDX-License-Identifier: GPL-2.0
/*
 * cryptfs: обёртка над kernel crypto API (skcipher, xts(aes)).
 *
 * Используется AES-256-XTS, сектор 512 байт. В IV первые 8 байт —
 * номер сектора (little-endian), остальные — нули: классическая
 * схема XTS с sector tweak, как в dm-crypt/plain64.
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/scatterlist.h>
#include <linux/mutex.h>
#include <linux/err.h>
#include <linux/string.h>
#include <crypto/skcipher.h>

#include "cryptfs.h"

static struct crypto_skcipher *cryptfs_tfm;
static DEFINE_MUTEX(cryptfs_key_lock);
static bool cryptfs_key_valid;

/* Ключ по умолчанию: удобен для разработки, пока не выставлен через
 * ioctl. В продакшене, естественно, его нужно заменить сразу после
 * загрузки модуля: cryptfs-ctl setkey $(cryptfs-ctl genkey). */
static const u8 cryptfs_default_key[CRYPTFS_KEY_SIZE] = {
	0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
	0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
	0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
	0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
	0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
	0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f,
	0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
	0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f,
};

int cryptfs_crypto_init(void)
{
	int err;

	cryptfs_tfm = crypto_alloc_skcipher("xts(aes)", 0, 0);
	if (IS_ERR(cryptfs_tfm)) {
		err = PTR_ERR(cryptfs_tfm);
		cryptfs_tfm = NULL;
		pr_err("cryptfs: cannot allocate xts(aes): %d\n", err);
		return err;
	}

	err = crypto_skcipher_setkey(cryptfs_tfm, cryptfs_default_key,
				     CRYPTFS_KEY_SIZE);
	if (err) {
		pr_err("cryptfs: initial setkey failed: %d\n", err);
		crypto_free_skcipher(cryptfs_tfm);
		cryptfs_tfm = NULL;
		return err;
	}

	cryptfs_key_valid = true;
	pr_info("cryptfs: crypto initialized (xts(aes), default key)\n");
	return 0;
}

void cryptfs_crypto_destroy(void)
{
	if (cryptfs_tfm) {
		crypto_free_skcipher(cryptfs_tfm);
		cryptfs_tfm = NULL;
	}
	cryptfs_key_valid = false;
}

int cryptfs_set_key(const u8 *key, size_t len)
{
	int err;

	if (len != CRYPTFS_KEY_SIZE)
		return -EINVAL;
	if (!cryptfs_tfm)
		return -ENODEV;

	mutex_lock(&cryptfs_key_lock);
	err = crypto_skcipher_setkey(cryptfs_tfm, key, len);
	if (!err)
		cryptfs_key_valid = true;
	mutex_unlock(&cryptfs_key_lock);

	if (err)
		pr_err("cryptfs: setkey failed: %d\n", err);
	else
		pr_info("cryptfs: key updated\n");
	return err;
}

bool cryptfs_has_key(void)
{
	return cryptfs_key_valid;
}

int cryptfs_crypt_range(bool encrypt, void *data, size_t len, loff_t offset)
{
	struct skcipher_request *req;
	DECLARE_CRYPTO_WAIT(wait);
	size_t off;
	int err = 0;

	if (!cryptfs_tfm || !cryptfs_key_valid)
		return -ENOKEY;
	if (len % CRYPTFS_SECTOR_SIZE)
		return -EINVAL;
	if (offset & CRYPTFS_SECTOR_MASK)
		return -EINVAL;

	req = skcipher_request_alloc(cryptfs_tfm, GFP_NOFS);
	if (!req)
		return -ENOMEM;

	skcipher_request_set_callback(req,
		CRYPTO_TFM_REQ_MAY_BACKLOG | CRYPTO_TFM_REQ_MAY_SLEEP,
		crypto_req_done, &wait);

	for (off = 0; off < len; off += CRYPTFS_SECTOR_SIZE) {
		struct scatterlist sg;
		u8 iv[16];
		__le64 sector = cpu_to_le64((u64)(offset + off) /
					    CRYPTFS_SECTOR_SIZE);

		memset(iv, 0, sizeof(iv));
		memcpy(iv, &sector, sizeof(sector));

		sg_init_one(&sg, (u8 *)data + off, CRYPTFS_SECTOR_SIZE);
		skcipher_request_set_crypt(req, &sg, &sg,
					   CRYPTFS_SECTOR_SIZE, iv);

		if (encrypt)
			err = crypto_wait_req(crypto_skcipher_encrypt(req),
					      &wait);
		else
			err = crypto_wait_req(crypto_skcipher_decrypt(req),
					      &wait);
		if (err) {
			pr_err_ratelimited("cryptfs: %s sector %llu failed: %d\n",
				encrypt ? "encrypt" : "decrypt",
				(unsigned long long)le64_to_cpu(sector), err);
			break;
		}
		reinit_completion(&wait.completion);
	}

	skcipher_request_free(req);
	return err;
}
