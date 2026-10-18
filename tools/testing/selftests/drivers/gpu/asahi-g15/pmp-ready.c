// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Exercise the production current14 supplier/entry selection. */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define IS_ENABLED(x) (x)
struct device_node { int id; };
struct device { struct device_node *of_node; };
static struct device_node supplier_node, other_node, report_node, entry_node;
static const char *machine = "apple,t8122", *supplier_compat = "apple,t8122-pmp-v2";
static bool has_report = true, has_entry = true, matching_supplier = true;
static const char *selected_report;
static int ready_result, calls, refs;
static bool of_machine_is_compatible(const char *name) { return !strcmp(machine, name); }
static bool of_device_is_compatible(struct device_node *node, const char *name)
{
	assert(node == &supplier_node);
	return !strcmp(supplier_compat, name);
}
static struct device_node *of_find_compatible_node(void *a, void *b, const char *name)
{
	assert(!a && !b);
	selected_report = name;
	if (!has_report)
		return NULL;
	refs++;
	return &report_node;
}
static struct device_node *of_parse_phandle(struct device_node *node, const char *name, int i)
{
	assert(node == &report_node && !strcmp(name, "apple,pmp") && i == 0);
	refs++;
	return matching_supplier ? &supplier_node : &other_node;
}
static struct device_node *apple_pmp_report_entry(struct device_node *node, unsigned int id)
{
	assert(node == &report_node && id == 7);
	if (!has_entry)
		return NULL;
	refs++;
	return &entry_node;
}
static unsigned long msecs_to_jiffies(unsigned int ms) { return ms * 2UL; }
static int apple_pmp_report_wait_ready(struct device_node *node, unsigned long timeout)
{
	assert(node == &entry_node && timeout == 10000);
	calls++;
	return ready_result;
}
static void of_node_put(struct device_node *node) { if (node) refs--; }
#include "pmp-ready-production.h"

int main(void)
{
	struct device supplier = { .of_node = &supplier_node };

	assert(apple_pmp_report_wait_current14(&supplier, 5000) == 0);
	assert(!strcmp(selected_report, "apple,t8122-pmp-v2-report") && calls == 1 && !refs);
	machine = "apple,t6030";
	supplier_compat = "apple,t6030-pmp-v2";
#if CONFIG_SILICONFOREST_NATIVE_GPU
	assert(apple_pmp_report_wait_current14(&supplier, 5000) == 0);
	assert(!strcmp(selected_report, "apple,t6030-pmp-v2-report") && calls == 2 && !refs);
	ready_result = -ETIMEDOUT;
	assert(apple_pmp_report_wait_current14(&supplier, 5000) == -ETIMEDOUT);
	ready_result = -EIO;
	assert(apple_pmp_report_wait_current14(&supplier, 5000) == -EIO);
	assert(calls == 4 && !refs);
#else
	assert(apple_pmp_report_wait_current14(&supplier, 5000) == -ENODEV);
	assert(calls == 1 && !refs);
#endif
	/* Rejection cases must never reach the readiness consumer. */
	machine = "apple,t8122";
	supplier_compat = "apple,t6030-pmp-v2";
	int before = calls;
	assert(apple_pmp_report_wait_current14(&supplier, 5000) == -ENODEV);
	supplier_compat = "apple,t8122-pmp-v2";
	has_report = false;
	assert(apple_pmp_report_wait_current14(&supplier, 5000) == -ENODEV);
	has_report = true;
	has_entry = false;
	assert(apple_pmp_report_wait_current14(&supplier, 5000) == -ENODEV);
	has_entry = true;
	matching_supplier = false;
	assert(apple_pmp_report_wait_current14(&supplier, 5000) == -ENODEV);
	assert(calls == before && !refs);
	puts("production PMP readiness: exact supplier/entry, config scope, error propagation and balanced refs: PASS");
}
