/*
 * btaddr: give a Bluetooth controller its public address over the kernel's
 * mgmt API (what "btmgmt public-addr" does, without BlueZ's libraries).
 *   btaddr INDEX            print the controller's address (Read Info)
 *   btaddr INDEX XX:..:XX   Set Public Address, then print it
 * The driver must implement set_bdaddr (btbcm does); the address is applied
 * when the controller is next powered on.
 */
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define BTPROTO_HCI		1
#define HCI_DEV_NONE		0xffff
#define HCI_CHANNEL_CONTROL	3

#define MGMT_OP_READ_INFO		0x0004
#define MGMT_OP_SET_PUBLIC_ADDRESS	0x0039
#define MGMT_EV_CMD_COMPLETE		0x0001
#define MGMT_EV_CMD_STATUS		0x0002

struct sockaddr_hci {
	unsigned short	hci_family;
	unsigned short	hci_dev;
	unsigned short	hci_channel;
};

struct mgmt_hdr {
	uint16_t opcode;
	uint16_t index;
	uint16_t len;
} __attribute__((packed));

/* send one command, wait for its Command Complete/Status; returns status */
static int cmd(int s, uint16_t op, uint16_t idx, const void *p, uint16_t len,
	       uint8_t *rsp, int *rlen)
{
	uint8_t buf[512];
	struct mgmt_hdr *h = (void *)buf;
	struct pollfd pfd = { .fd = s, .events = POLLIN };

	h->opcode = op;
	h->index = idx;
	h->len = len;
	memcpy(buf + sizeof(*h), p, len);
	if (write(s, buf, sizeof(*h) + len) < 0)
		return -errno;
	while (poll(&pfd, 1, 3000) > 0) {
		int n = read(s, buf, sizeof(buf));
		uint16_t ev, eop;

		if (n < (int)sizeof(*h) + 3)
			continue;
		ev = h->opcode;
		eop = buf[6] | buf[7] << 8;
		if ((ev != MGMT_EV_CMD_COMPLETE && ev != MGMT_EV_CMD_STATUS) ||
		    eop != op || h->index != idx)
			continue;
		if (rsp && rlen) {
			*rlen = n - 9;
			memcpy(rsp, buf + 9, *rlen);
		}
		return buf[8];
	}
	return -ETIMEDOUT;
}

static int print_addr(int s, uint16_t idx)
{
	uint8_t r[300];
	int rl = 0, st = cmd(s, MGMT_OP_READ_INFO, idx, NULL, 0, r, &rl);

	if (st || rl < 6) {
		fprintf(stderr, "read info: status %d\n", st);
		return 1;
	}
	printf("hci%u %02X:%02X:%02X:%02X:%02X:%02X\n", idx,
	       r[5], r[4], r[3], r[2], r[1], r[0]);
	return 0;
}

int main(int argc, char **argv)
{
	struct sockaddr_hci a = { 31 /* AF_BLUETOOTH */, HCI_DEV_NONE, HCI_CHANNEL_CONTROL };
	unsigned b[6];
	uint8_t le[6];
	uint16_t idx;
	int s, i, st;

	if (argc < 2)
		return 1;
	idx = atoi(argv[1]);
	s = socket(31, SOCK_RAW | SOCK_CLOEXEC, BTPROTO_HCI);
	if (s < 0 || bind(s, (void *)&a, sizeof(a)) < 0) {
		perror("mgmt socket");
		return 1;
	}
	if (argc > 2) {
		/* only once the controller is fully registered: a set during
		 * btbcm's setup leaves the index removed for good */
		if (print_addr(s, idx))
			return 2;
		if (sscanf(argv[2], "%x:%x:%x:%x:%x:%x",
			   &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6)
			return 1;
		for (i = 0; i < 6; i++)
			le[i] = b[5 - i];
		st = cmd(s, MGMT_OP_SET_PUBLIC_ADDRESS, idx, le, 6, NULL, NULL);
		if (st) {
			fprintf(stderr, "set public address: status %d\n", st);
			return 1;
		}
	}
	/* the kernel re-adds the index after re-initialising the chip */
	for (i = 0; i < 50; i++) {
		if (!print_addr(s, idx))
			return 0;
		usleep(100000);
	}
	return 1;
}
