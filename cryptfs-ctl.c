/*
 * cryptfs-ctl — userspace-утилита для управления ключом cryptfs.
 *
 *   cryptfs-ctl genkey                 # вывести случайный 64-байтный ключ
 *   cryptfs-ctl setkey <128 hex chars> # установить ключ в модуль
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>

#include "cryptfs_uapi.h"

#define CRYPTFS_CTL_DEV "/dev/cryptfs_ctl"

static int hex2nibble(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

static int parse_hex_key(const char *hex, unsigned char *out, size_t expected)
{
	size_t hlen = strlen(hex), i;

	if (hlen != expected * 2) {
		fprintf(stderr,
			"error: key must be %zu hex characters (%zu bytes)\n",
			expected * 2, expected);
		return -1;
	}
	for (i = 0; i < expected; i++) {
		int hi = hex2nibble(hex[i * 2]);
		int lo = hex2nibble(hex[i * 2 + 1]);
		if (hi < 0 || lo < 0) {
			fprintf(stderr, "error: invalid hex character\n");
			return -1;
		}
		out[i] = (unsigned char)((hi << 4) | lo);
	}
	return 0;
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage:\n"
		"  %s setkey <%d-hex-chars>    set AES-256-XTS key (%d bytes)\n"
		"  %s genkey                   print a random key in hex\n",
		prog, CRYPTFS_KEY_SIZE * 2, CRYPTFS_KEY_SIZE, prog);
}

static int cmd_setkey(const char *hex)
{
	struct cryptfs_key k;
	int fd, rc;

	if (parse_hex_key(hex, k.key, CRYPTFS_KEY_SIZE))
		return 2;

	fd = open(CRYPTFS_CTL_DEV, O_WRONLY);
	if (fd < 0) {
		fprintf(stderr, "open %s: %s\n",
			CRYPTFS_CTL_DEV, strerror(errno));
		return 2;
	}

	rc = ioctl(fd, CRYPTFS_IOC_SETKEY, &k);
	if (rc < 0) {
		fprintf(stderr, "ioctl SETKEY: %s\n", strerror(errno));
		close(fd);
		return 2;
	}

	close(fd);
	printf("cryptfs: key set\n");
	return 0;
}

static int cmd_genkey(void)
{
	unsigned char k[CRYPTFS_KEY_SIZE];
	FILE *u;
	size_t i;

	u = fopen("/dev/urandom", "rb");
	if (!u) {
		perror("fopen /dev/urandom");
		return 2;
	}
	if (fread(k, 1, sizeof(k), u) != sizeof(k)) {
		fprintf(stderr, "fread: short read\n");
		fclose(u);
		return 2;
	}
	fclose(u);

	for (i = 0; i < sizeof(k); i++)
		printf("%02x", k[i]);
	printf("\n");
	return 0;
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		usage(argv[0]);
		return 1;
	}
	if (!strcmp(argv[1], "setkey")) {
		if (argc < 3) {
			usage(argv[0]);
			return 1;
		}
		return cmd_setkey(argv[2]);
	}
	if (!strcmp(argv[1], "genkey"))
		return cmd_genkey();

	usage(argv[0]);
	return 1;
}
