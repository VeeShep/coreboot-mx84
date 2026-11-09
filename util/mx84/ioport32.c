// SPDX-License-Identifier: GPL-2.0-only
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/io.h>

int main(int argc, char **argv)
{
	char *end;
	unsigned long port;

	if (argc != 2) {
		fprintf(stderr, "usage: %s PORT\n", argv[0]);
		return 2;
	}

	errno = 0;
	port = strtoul(argv[1], &end, 0);
	if (errno || *end || port > UINT16_MAX || (port & 3)) {
		fprintf(stderr, "invalid aligned I/O port: %s\n", argv[1]);
		return 2;
	}
	if (ioperm(port, 4, 1)) {
		perror("ioperm");
		return 1;
	}

	printf("0x%04lx: 0x%08x\n", port, inl(port));
	ioperm(port, 4, 0);
	return 0;
}
