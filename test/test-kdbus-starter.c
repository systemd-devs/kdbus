#include <stdio.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stddef.h>
#include <unistd.h>
#include <stdint.h>
#include <errno.h>
#include <assert.h>
#include <poll.h>
#include <sys/ioctl.h>

#include "kdbus-util.h"
#include "kdbus-enum.h"

#define POOL_SIZE (16 * 1024LU * 1024LU)
static struct conn *make_starter(const char *path, const char *name)
{
	int fd, ret;
	struct kdbus_cmd_hello *hello;
	struct kdbus_item *item;
	struct conn *conn;
	size_t size, slen;

	slen = strlen(name) + 1;
	size = sizeof(*hello) + KDBUS_ITEM_SIZE(slen);

	hello = alloca(size);
	memset(hello, 0, size);

	printf("-- opening STARTER bus connection %s\n", path);
	fd = open(path, O_RDWR|O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "--- error %d (%m)\n", fd);
		return NULL;
	}

	hello->size = size;
	hello->pool_size = POOL_SIZE;
	hello->conn_flags = KDBUS_HELLO_STARTER;

	item = hello->items;
	item->size = KDBUS_ITEM_SIZE(slen);
	item->type = KDBUS_ITEM_STARTER_NAME;
	strcpy(item->str, name);

	ret = ioctl(fd, KDBUS_CMD_HELLO, hello);
	if (ret < 0) {
		fprintf(stderr, "--- error when saying hello: %d (%m)\n", ret);
		return NULL;
	}
	printf("-- Our peer ID for starter %s: %llu\n", name, (unsigned long long) hello->id);

	conn = malloc(sizeof(*conn));
	if (!conn) {
		fprintf(stderr, "unable to malloc()!?\n");
		return NULL;
	}

	conn->fd = fd;
	conn->id = hello->id;

	return conn;
}


int main(int argc, char *argv[])
{
	struct {
		struct kdbus_cmd_bus_make head;

		/* name item */
		uint64_t n_size;
		uint64_t n_type;
		char name[64];
	} __attribute__ ((__aligned__(8))) bus_make;
	int fdc, ret;
	char *bus;
	struct conn *starter, *conn_a;
	struct pollfd fds[2];
	int starter_done = 0;

	printf("-- opening /dev/kdbus/control\n");
	fdc = open("/dev/kdbus/control", O_RDWR|O_CLOEXEC);
	if (fdc < 0) {
		fprintf(stderr, "--- error %d (%m)\n", fdc);
		return EXIT_FAILURE;
	}

	memset(&bus_make, 0, sizeof(bus_make));
	bus_make.head.bloom_size = 64;

	snprintf(bus_make.name, sizeof(bus_make.name), "%u-testbus", getuid());
	bus_make.n_type = KDBUS_MAKE_NAME;
	bus_make.n_size = KDBUS_PART_HEADER_SIZE + strlen(bus_make.name) + 1;

	bus_make.head.size = sizeof(struct kdbus_cmd_bus_make) +
			     bus_make.n_size;

	printf("-- creating bus '%s'\n", bus_make.name);
	ret = ioctl(fdc, KDBUS_CMD_BUS_MAKE, &bus_make);
	if (ret) {
		fprintf(stderr, "--- error %d (%m)\n", ret);
		return EXIT_FAILURE;
	}

	if (asprintf(&bus, "/dev/kdbus/%s/bus", bus_make.name) < 0)
		return EXIT_FAILURE;

	starter = make_starter(bus, "foo.test.starter");

	conn_a = connect_to_bus(bus);
	if (!starter || !conn_a)
		return EXIT_FAILURE;

	upload_policy(conn_a->fd, "foo.test.starter");
	add_match_empty(conn_a->fd);

	name_list(conn_a, KDBUS_NAME_LIST_NAMES |
			KDBUS_NAME_LIST_UNIQUE |
			KDBUS_NAME_LIST_STARTERS |
			KDBUS_NAME_LIST_QUEUED);

	msg_send(conn_a, "foo.test.starter", 0xdeafbeef, KDBUS_DST_ID_NAME);

	fds[0].fd = starter->fd;
	fds[1].fd = conn_a->fd;

	printf("-- entering poll loop ...\n");
	for (;;) {
		int i, nfds = sizeof(fds) / sizeof(fds[0]);

		for (i = 0; i < nfds; i++) {
			fds[i].events = POLLIN | POLLPRI;
			fds[i].revents = 0;
		}

		ret = poll(fds, nfds, 3000);
		if (ret <= 0)
			break;

		name_list(conn_a, KDBUS_NAME_LIST_NAMES);

		if ((fds[0].revents & POLLIN) && !starter_done) {
			printf("Starter was called back!\n");
			ret = name_acquire(conn_a, "foo.test.starter", KDBUS_NAME_REPLACE_EXISTING);
			if (ret != 0)
				break;

			starter_done = 1;
		}

		if (fds[1].revents & POLLIN) {
			msg_recv(conn_a);
			break;
		}
	}

	printf("-- closing bus connections\n");
	close(starter->fd);
	close(conn_a->fd);
	free(starter);
	free(conn_a);

	printf("-- closing bus master\n");
	close(fdc);
	free(bus);

	return EXIT_SUCCESS;
}
