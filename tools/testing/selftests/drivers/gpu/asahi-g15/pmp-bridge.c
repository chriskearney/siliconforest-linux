/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Compile the production glue functions with both PMP owners. */
#include <assert.h>
#include <stdbool.h>
#include <errno.h>
#include <stdio.h>

struct device { void *of_node; };
static bool board = true, compatible = true;
static int links, votes, link_result, vote_result;
static bool asahi_g15_board(void) { return board; }
static bool of_device_is_compatible(void *node, const char *name)
{
	(void)node;
	(void)name;
	return compatible;
}
static int apple_pmp_link_device(struct device *dev)
{
	assert(dev->of_node);
	links++;
	return link_result;
}
static int apple_pmp_set_device_power(unsigned char command, unsigned short device,
				    unsigned int enabled)
{
	assert(command == 0x0f && device == 5 && enabled == 1);
	votes++;
	return vote_result;
}
static int apple_pmp_j514s_gpu_prepare(struct device *dev)
{
	return apple_pmp_link_device(dev);
}
static int apple_pmp_j514s_gpu_get(struct device *dev)
{
	assert(dev->of_node);
	return apple_pmp_set_device_power(0x0f, 5, 1);
}
#include "pmp-production.h"

int main(void)
{
	struct device dev = { .of_node = &dev };

	board = false;
	assert(asahi_g15_pmp_prepare(&dev) == -ENODEV);
	assert(asahi_g15_pmp_get(&dev) == -ENODEV);
	board = true;
	compatible = false;
	assert(asahi_g15_pmp_prepare(&dev) == -ENODEV);
	assert(asahi_g15_pmp_get(&dev) == -ENODEV);
	compatible = true;
	dev.of_node = NULL;
	assert(asahi_g15_pmp_prepare(&dev) == -ENODEV);
	assert(asahi_g15_pmp_get(&dev) == -ENODEV);
	assert(links == 0 && votes == 0);
	dev.of_node = &dev;
	link_result = -517; /* EPROBE_DEFER */
	assert(asahi_g15_pmp_prepare(&dev) == -517);
	assert(votes == 0);
	link_result = 0;
	assert(asahi_g15_pmp_prepare(&dev) == 0);
	vote_result = -ETIMEDOUT;
	assert(asahi_g15_pmp_get(&dev) == -ETIMEDOUT);
	vote_result = -EIO;
	assert(asahi_g15_pmp_get(&dev) == -EIO);
	vote_result = 0;
	assert(asahi_g15_pmp_get(&dev) == 0);
	assert(links == 2 && votes == 3);
	puts("production PMP bridge: board rejection, defer, ACK errors and exact AGX vote: PASS");
}
