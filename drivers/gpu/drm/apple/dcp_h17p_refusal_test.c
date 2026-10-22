// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * A T8140 external display processor that is not in its inherited idle state
 * when it is bound. Included at the end of dcp.c, so that the production
 * dcp_comp_bind_h17p(), dcp_h17p_probe_finish(), the component, shutdown and
 * PM entry points, the devres releases and the DRM admission run unchanged.
 *
 * The fixture mirrors dcp_platform_probe_h17p() up to the start of its
 * external processor: the same context allocation and devres release, the
 * same probe-time works and locks, and the T8140 external hardware profile.
 * Only hardware discovery is stubbed: the coprocessor registers and their
 * mapping, the PIODMA child and the display registers. Fixed-rate clocks are
 * registered for the device. The pin, the work and DMA domain handling, the
 * refusal and the component are real. Counting stubs only observe the Type-C
 * work toggles and the external disconnect, and one test fails the pin.
 */
#include <linux/clk-provider.h>
#include <linux/clkdev.h>

#include <kunit/platform_device.h>
#include <kunit/static_stub.h>
#include <kunit/test.h>

#define REFUSAL_RUNNING 0x6d	/* started, not stopped */
#define REFUSAL_IDLE 0x6e	/* started, stopped in its idle loop */

/* A non-NULL mapping and PIODMA domain that the refused paths never use. */
#define REFUSAL_COPROC ((void __iomem *)0x1000)
#define REFUSAL_PIODMA_DOMAIN ((struct iommu_domain *)0x2000)

struct refusal_test {
	struct platform_device *pdev;
	struct apple_dcp *dcp;
	u32 control;
	u32 status[2];		/* first and second idle check */
	unsigned int reads;
	unsigned int pins, disables, enables, disconnects, piodma_creates;
	int pin_ret;
};

static struct refusal_test *refusal_current;

static void __iomem *refusal_coproc_map(struct device *dev)
{
	return REFUSAL_COPROC;
}

static u32 refusal_coproc_read(struct apple_dcp *dcp, u32 reg)
{
	struct refusal_test *rt = refusal_current;

	if (reg == APPLE_DCP_COPROC_CPU_CONTROL)
		return rt->control;
	KUNIT_ASSERT_EQ(kunit_get_current_test(), reg, APPLE_DCP_COPROC_CPU_STATUS);
	return rt->status[min(rt->reads++, 1U)];
}

static int refusal_piodma_create(struct apple_dcp *dcp)
{
	refusal_current->piodma_creates++;
	dcp->iommu_dom = REFUSAL_PIODMA_DOMAIN;
	return 0;
}

static int refusal_disp_regs(struct apple_dcp *dcp)
{
	return 0;
}

static int refusal_pin_fails(struct apple_dcp *dcp)
{
	refusal_current->pins++;
	return refusal_current->pin_ret;
}

static void refusal_disable_typec(struct apple_dcp *dcp, bool release_cable)
{
	refusal_current->disables++;
}

static void refusal_enable_typec(struct apple_dcp *dcp)
{
	refusal_current->enables++;
}

static int refusal_disconnect(struct apple_dcp *dcp, bool physical)
{
	refusal_current->disconnects++;
	return 0;
}

static void refusal_clkdev_drop(void *data)
{
	clkdev_drop(data);
}

static void refusal_clk_unregister(void *data)
{
	clk_hw_unregister_fixed_rate(data);
}

/* Clocks that devm_clk_get() finds for @dev_id, as the device tree gives. */
static void refusal_clocks(struct kunit *test, const char *dev_id)
{
	static const char *const con_ids[] = { NULL, "clock-194" };
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(con_ids); i++) {
		struct clk_lookup *cl;
		struct clk_hw *hw;
		char name[48];

		snprintf(name, sizeof(name), "%s-clk%u", dev_id, i);
		hw = clk_hw_register_fixed_rate(NULL, name, NULL, 0, 600000000);
		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, hw);
		KUNIT_ASSERT_EQ(test, 0,
				kunit_add_action_or_reset(test, refusal_clk_unregister, hw));
		cl = clkdev_hw_create(hw, con_ids[i], "%s", dev_id);
		KUNIT_ASSERT_NOT_NULL(test, cl);
		KUNIT_ASSERT_EQ(test, 0,
				kunit_add_action_or_reset(test, refusal_clkdev_drop, cl));
	}
}

static void refusal_component_del(void *data)
{
	component_del(data, &dcp_comp_ops);
}

/*
 * A platform device with a firmware node of its own: dcp_session_retained()
 * identifies a device by that node. The node is never removed, so that it
 * lives as long as a session retained for the device.
 */
static struct platform_device *refusal_device(struct kunit *test, const char *name)
{
	struct platform_device *pdev;
	struct fwnode_handle *fwnode;

	pdev = kunit_platform_device_alloc(test, name, PLATFORM_DEVID_NONE);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, pdev);
	fwnode = fwnode_create_software_node(NULL, NULL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, fwnode);
	device_set_node(&pdev->dev, fwnode);
	KUNIT_ASSERT_EQ(test, 0, kunit_platform_device_add(test, pdev));
	return pdev;
}

static const char *refusal_name(struct kunit *test)
{
	static atomic_t serial = ATOMIC_INIT(0);
	char name[32];
	char *copy;

	snprintf(name, sizeof(name), "dcp-h17p-refusal-%d", atomic_inc_return(&serial));
	copy = kunit_kstrdup(test, name, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, copy);
	return copy;
}

/* A processor as dcp_platform_probe_h17p() leaves it before its startup. */
static struct refusal_test *refusal_setup(struct kunit *test,
					  const struct apple_dcp_hw_data *hw, bool clocks)
{
	struct refusal_test *rt;
	struct apple_dcp *dcp;
	struct device *dev;
	const char *name;

	rt = kunit_kzalloc(test, sizeof(*rt), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, rt);
	refusal_current = rt;
	rt->control = APPLE_DCP_COPROC_CPU_CONTROL_RUN;
	rt->status[0] = REFUSAL_IDLE;
	rt->status[1] = REFUSAL_IDLE;

	name = refusal_name(test);
	/* Registered before the device, so that its devres puts them first. */
	if (clocks)
		refusal_clocks(test, name);
	rt->pdev = refusal_device(test, name);
	dev = &rt->pdev->dev;
	KUNIT_ASSERT_FALSE(test, dcp_session_retained(dev));

	/* dcp_platform_probe_h17p(): context, its devres and probe-time state. */
	dcp = kzalloc_obj(*dcp);
	KUNIT_ASSERT_NOT_NULL(test, dcp);
	KUNIT_ASSERT_EQ(test, 0, devm_add_action_or_reset(dev, dcp_release_context, dcp));
	INIT_LIST_HEAD(&dcp->swapped_out_fbs);
	mutex_init(&dcp->swapped_out_fbs_lock);
	KUNIT_ASSERT_EQ(test, 0, dcp_backlight_init(dcp, dev));
	iomfb_queue_init(dcp);
	dcp->fw_compat = DCP_FIRMWARE_H17P;
	dcp->dev = dev;
	dcp->hw = *hw;
	mutex_init(&dcp->hpd_mutex);
	mutex_init(&dcp->external_lock);
	INIT_WORK(&dcp->external_stop_work, dcp_external_stop_work);
	KUNIT_ASSERT_EQ(test, 0, iommu_device_use_default_domain(dev));
	KUNIT_ASSERT_EQ(test, 0, devm_add_action_or_reset(dev, dcp_release_dma_domain, dcp));
	INIT_DELAYED_WORK(&dcp->typec_reconnect_wq, dcp_typec_reconnect_work_h17p);
	INIT_DELAYED_WORK(&dcp->placeholder_edid_wq, dcp_placeholder_edid_work_h17p);
	INIT_DELAYED_WORK(&dcp->bl_fallback_wq, dcp_work_backlight_fallback);
	disable_delayed_work(&dcp->typec_reconnect_wq);
	disable_delayed_work(&dcp->placeholder_edid_wq);
	platform_set_drvdata(rt->pdev, dcp);
	rt->dcp = dcp;

	kunit_activate_static_stub(test, dcp_coproc_map, refusal_coproc_map);
	kunit_activate_static_stub(test, dcp_coproc_read, refusal_coproc_read);
	kunit_activate_static_stub(test, dcp_create_piodma_iommu_dev_h17p, refusal_piodma_create);
	kunit_activate_static_stub(test, dcp_get_disp_regs_h17p, refusal_disp_regs);
	kunit_activate_static_stub(test, dcp_disable_typec_work_h17p, refusal_disable_typec);
	kunit_activate_static_stub(test, dcp_enable_typec_work_h17p, refusal_enable_typec);
	kunit_activate_static_stub(test, dcp_external_disconnect, refusal_disconnect);
	return rt;
}

/* What every refusal leaves: pinned, never prepared, routes ignored. */
static void refusal_expect_refused(struct kunit *test, struct refusal_test *rt)
{
	struct apple_dcp *dcp = rt->dcp;

	KUNIT_EXPECT_TRUE(test, READ_ONCE(dcp->ext_refused));
	KUNIT_EXPECT_TRUE(test, READ_ONCE(dcp->ext_dead));
	KUNIT_EXPECT_TRUE(test, READ_ONCE(dcp->retain_dma));
	KUNIT_EXPECT_TRUE(test, dcp_session_retained(dcp->dev));
	KUNIT_EXPECT_FALSE(test, READ_ONCE(dcp->external_prepared));
	KUNIT_EXPECT_NULL(test, dcp->rtk);
}

/* The bind of a processor that is running instead of idle. */
static void refusal_first_check_test(struct kunit *test)
{
	struct refusal_test *rt = refusal_setup(test, &apple_dcp_hw_t8140_external, false);
	struct apple_dcp *dcp = rt->dcp;

	rt->status[0] = REFUSAL_RUNNING;
	KUNIT_EXPECT_EQ(test, dcp_comp_bind_h17p(dcp->dev, NULL, NULL), 0);
	refusal_expect_refused(test, rt);
	/* Refused at the first check: no PIODMA, and no bind-time work set up. */
	KUNIT_EXPECT_EQ(test, rt->reads, 1U);
	KUNIT_EXPECT_EQ(test, rt->piodma_creates, 0U);
	KUNIT_EXPECT_NULL(test, dcp->iommu_dom);
	KUNIT_EXPECT_NULL(test, dcp->vblank_wq.func);
	KUNIT_EXPECT_FALSE(test, READ_ONCE(dcp->quiescing));
}

/* A cleared RUN bit is outside the idle state too. */
static void refusal_run_cleared_test(struct kunit *test)
{
	struct refusal_test *rt = refusal_setup(test, &apple_dcp_hw_t8140_external, false);

	rt->control = 0;
	KUNIT_EXPECT_EQ(test, dcp_comp_bind_h17p(rt->dcp->dev, NULL, NULL), 0);
	refusal_expect_refused(test, rt);
	KUNIT_EXPECT_NULL(test, rt->dcp->vblank_wq.func);
}

/* The internal processor's adoption stays strict: no refusal for it. */
static void refusal_internal_unchanged_test(struct kunit *test)
{
	struct refusal_test *rt = refusal_setup(test, &apple_dcp_hw_t8140, false);
	struct apple_dcp *dcp = rt->dcp;

	rt->control = 0;
	KUNIT_EXPECT_EQ(test, dcp_comp_bind_h17p(dcp->dev, NULL, NULL), -EOPNOTSUPP);
	KUNIT_EXPECT_FALSE(test, READ_ONCE(dcp->ext_refused));
	KUNIT_EXPECT_FALSE(test, READ_ONCE(dcp->ext_dead));
	KUNIT_EXPECT_FALSE(test, READ_ONCE(dcp->retain_dma));
}

/* Without a pinned session the refusal does not happen: the error stays. */
static void refusal_pin_failure_strict_test(struct kunit *test)
{
	struct refusal_test *rt = refusal_setup(test, &apple_dcp_hw_t8140_external, false);
	struct apple_dcp *dcp = rt->dcp;

	rt->status[0] = REFUSAL_RUNNING;
	rt->pin_ret = -ENOMEM;
	kunit_activate_static_stub(test, dcp_pin_live_session, refusal_pin_fails);
	KUNIT_EXPECT_EQ(test, dcp_comp_bind_h17p(dcp->dev, NULL, NULL), -EBUSY);
	KUNIT_EXPECT_EQ(test, rt->pins, 1U);
	KUNIT_EXPECT_FALSE(test, READ_ONCE(dcp->ext_refused));
	KUNIT_EXPECT_FALSE(test, READ_ONCE(dcp->ext_dead));
	KUNIT_EXPECT_FALSE(test, READ_ONCE(dcp->retain_dma));
}

/* Idle at the first check, running at the startup recheck. */
static void refusal_second_check_test(struct kunit *test)
{
	struct refusal_test *rt = refusal_setup(test, &apple_dcp_hw_t8140_external, true);
	struct apple_dcp *dcp = rt->dcp;

	rt->status[1] = REFUSAL_RUNNING;
	KUNIT_EXPECT_EQ(test, dcp_comp_bind_h17p(dcp->dev, NULL, NULL), 0);
	refusal_expect_refused(test, rt);
	KUNIT_EXPECT_EQ(test, rt->reads, 2U);
	KUNIT_EXPECT_EQ(test, rt->piodma_creates, 1U);
	/* Pinned before the recheck: PIODMA and its domain are retained. */
	KUNIT_EXPECT_PTR_EQ(test, dcp->iommu_dom, REFUSAL_PIODMA_DOMAIN);
	KUNIT_EXPECT_TRUE(test, READ_ONCE(dcp->quiescing));
	/* The bind-time works were set up, and nothing queued them. */
	KUNIT_EXPECT_NOT_NULL(test, dcp->vblank_wq.func);
	KUNIT_EXPECT_FALSE(test, work_pending(&dcp->vblank_wq));
	KUNIT_EXPECT_FALSE(test, delayed_work_pending(&dcp->swap_watchdog_wq));
}

/* dcp_platform_probe_h17p()'s order: startup, then the component. */
static void refusal_probe_order_test(struct kunit *test)
{
	struct refusal_test *rt = refusal_setup(test, &apple_dcp_hw_t8140_external, false);
	struct apple_dcp *dcp = rt->dcp;

	rt->status[0] = REFUSAL_RUNNING;
	KUNIT_ASSERT_EQ(test, dcp_h17p_probe_finish(rt->pdev, dcp), 0);
	KUNIT_ASSERT_EQ(test, 0, kunit_add_action_or_reset(test, refusal_component_del,
							   dcp->dev));
	refusal_expect_refused(test, rt);
	KUNIT_EXPECT_EQ(test, rt->disables, 0U);
	/* The master's own bind of the component is a no-op. */
	KUNIT_EXPECT_EQ(test, dcp_comp_bind(dcp->dev, NULL, NULL), 0);
	KUNIT_EXPECT_EQ(test, rt->reads, 1U);
}

/* A failed pin fails the probe before the component is added, as before. */
static void refusal_probe_pin_failure_test(struct kunit *test)
{
	struct refusal_test *rt = refusal_setup(test, &apple_dcp_hw_t8140_external, false);

	rt->status[0] = REFUSAL_RUNNING;
	rt->pin_ret = -ENOMEM;
	kunit_activate_static_stub(test, dcp_pin_live_session, refusal_pin_fails);
	KUNIT_EXPECT_EQ(test, dcp_h17p_probe_finish(rt->pdev, rt->dcp), -EBUSY);
	KUNIT_EXPECT_EQ(test, rt->disables, 1U);
	KUNIT_EXPECT_FALSE(test, READ_ONCE(rt->dcp->external_prepared));
}

static int refusal_bind_ok(struct device *dev, struct device *main, void *data)
{
	return 0;
}

/* Supplementary: a successful startup is still prepared before the add. */
static void refusal_probe_success_prepared_test(struct kunit *test)
{
	struct refusal_test *rt = refusal_setup(test, &apple_dcp_hw_t8140_external, false);

	kunit_activate_static_stub(test, dcp_comp_bind_h17p, refusal_bind_ok);
	KUNIT_ASSERT_EQ(test, dcp_h17p_probe_finish(rt->pdev, rt->dcp), 0);
	KUNIT_ASSERT_EQ(test, 0, kunit_add_action_or_reset(test, refusal_component_del,
							   rt->dcp->dev));
	KUNIT_EXPECT_TRUE(test, READ_ONCE(rt->dcp->external_prepared));
	KUNIT_EXPECT_FALSE(test, READ_ONCE(rt->dcp->ext_refused));
}

/* Refused after the recheck set quiescing: the component bind is a no-op. */
static void refusal_component_bind_noop_test(struct kunit *test)
{
	struct refusal_test *rt = refusal_setup(test, &apple_dcp_hw_t8140_external, true);
	struct apple_dcp *dcp = rt->dcp;

	rt->status[1] = REFUSAL_RUNNING;
	KUNIT_ASSERT_EQ(test, dcp_comp_bind_h17p(dcp->dev, NULL, NULL), 0);
	KUNIT_ASSERT_TRUE(test, READ_ONCE(dcp->quiescing));
	KUNIT_EXPECT_EQ(test, dcp_comp_bind(dcp->dev, NULL, NULL), 0);
	KUNIT_EXPECT_EQ(test, rt->reads, 2U);
	KUNIT_EXPECT_EQ(test, rt->piodma_creates, 1U);
}

/*
 * Every later entry point leaves a refused processor as it is. The works set
 * up only by a bind are still unset after a first-check refusal, so any of
 * these that reached the unbind would cancel a work that was never set up.
 */
static void refusal_entry_points(struct kunit *test, struct refusal_test *rt)
{
	struct apple_dcp *dcp = rt->dcp;
	struct device *dev = dcp->dev;
	bool quiescing = READ_ONCE(dcp->quiescing);

	dcp_comp_unbind(dev, NULL, NULL);
	dcp_comp_unbind_h17p(dev, NULL, NULL);
	dcp_platform_shutdown_h17p(rt->pdev);
	KUNIT_EXPECT_EQ(test, dcp_platform_suspend_h17p(dev), 0);
	KUNIT_EXPECT_TRUE(test, dev->power.wakeup_path);
	KUNIT_EXPECT_EQ(test, dcp_platform_resume_h17p(dev), 0);

	KUNIT_EXPECT_EQ(test, READ_ONCE(dcp->quiescing), quiescing);
	KUNIT_EXPECT_FALSE(test, READ_ONCE(dcp->iomfb.stopped));
	KUNIT_EXPECT_EQ(test, rt->disables, 0U);
	KUNIT_EXPECT_EQ(test, rt->enables, 0U);
	KUNIT_EXPECT_EQ(test, rt->disconnects, 0U);
	/* The Type-C works stay disabled since probe: nothing can queue them. */
	KUNIT_EXPECT_FALSE(test, queue_delayed_work(system_wq, &dcp->typec_reconnect_wq, 0));
	KUNIT_EXPECT_FALSE(test, queue_delayed_work(system_wq, &dcp->placeholder_edid_wq, 0));
	refusal_expect_refused(test, rt);
}

static void refusal_entry_points_first_test(struct kunit *test)
{
	struct refusal_test *rt = refusal_setup(test, &apple_dcp_hw_t8140_external, false);

	rt->status[0] = REFUSAL_RUNNING;
	KUNIT_ASSERT_EQ(test, dcp_comp_bind_h17p(rt->dcp->dev, NULL, NULL), 0);
	KUNIT_ASSERT_NULL(test, rt->dcp->vblank_wq.func);
	refusal_entry_points(test, rt);
}

static void refusal_entry_points_second_test(struct kunit *test)
{
	struct refusal_test *rt = refusal_setup(test, &apple_dcp_hw_t8140_external, true);

	rt->status[1] = REFUSAL_RUNNING;
	KUNIT_ASSERT_EQ(test, dcp_comp_bind_h17p(rt->dcp->dev, NULL, NULL), 0);
	refusal_entry_points(test, rt);
	KUNIT_EXPECT_PTR_EQ(test, rt->dcp->iommu_dom, REFUSAL_PIODMA_DOMAIN);
}

/*
 * The retained session blocks a later probe of the same device, and only of
 * that device: another one that was never refused is not retained.
 */
static void refusal_reprobe_blocked_test(struct kunit *test)
{
	struct refusal_test *rt = refusal_setup(test, &apple_dcp_hw_t8140_external, false);
	struct platform_device *other = refusal_device(test, refusal_name(test));

	rt->status[0] = REFUSAL_RUNNING;
	KUNIT_EXPECT_FALSE(test, dcp_session_retained(rt->dcp->dev));
	KUNIT_ASSERT_EQ(test, dcp_comp_bind_h17p(rt->dcp->dev, NULL, NULL), 0);
	KUNIT_EXPECT_TRUE(test, dcp_session_retained(rt->dcp->dev));
	KUNIT_EXPECT_FALSE(test, dcp_session_retained(&other->dev));
	/* As the driver core calls it: with the driver set during the probe. */
	rt->pdev->dev.driver = &apple_platform_driver.driver;
	KUNIT_EXPECT_EQ(test, dcp_platform_probe_h17p(rt->pdev), -EBUSY);
	rt->pdev->dev.driver = NULL;
}

/*
 * The devres releases of the probe run while the session is pinned. The
 * context, and the Type-C routes inside it that a USB-C controller may still
 * hold, stay valid; KASAN fails the test on any access after a free.
 */
static void refusal_devres_and_routes_test(struct kunit *test)
{
	struct refusal_test *rt = refusal_setup(test, &apple_dcp_hw_t8140_external, false);
	struct apple_dcp *dcp = rt->dcp;
	struct apple_dcp_typec_route *route = &dcp->typec_routes[0];

	route->dcp = dcp;
	dcp->nr_typec_routes = 1;
	rt->status[0] = REFUSAL_RUNNING;
	KUNIT_ASSERT_EQ(test, dcp_comp_bind_h17p(dcp->dev, NULL, NULL), 0);

	dcp_release_dma_domain(dcp);
	dcp_release_context(dcp);
	/* As dcp_typec_route_set() finds a route through its cached mux. */
	KUNIT_EXPECT_PTR_EQ(test, READ_ONCE(route->dcp), dcp);
	KUNIT_EXPECT_TRUE(test, READ_ONCE(route->dcp->ext_dead));
	KUNIT_EXPECT_TRUE(test, READ_ONCE(dcp->retain_dma));
}

/* The DRM device's admission of an external processor. */
static void refusal_admission_test(struct kunit *test)
{
	struct refusal_test *rt = refusal_setup(test, &apple_dcp_hw_t8140_external, false);
	struct apple_dcp *dcp = rt->dcp;

	KUNIT_EXPECT_EQ(test, dcp_h17p_external_admit_dcp(NULL), -ENODEV);
	/* Not prepared: not ready, the DRM device fails as before. */
	KUNIT_EXPECT_EQ(test, dcp_h17p_external_admit_dcp(dcp), -ENODEV);
	WRITE_ONCE(dcp->external_prepared, true);
	KUNIT_EXPECT_EQ(test, dcp_h17p_external_admit_dcp(dcp), 0);
	WRITE_ONCE(dcp->crashed, true);
	KUNIT_EXPECT_EQ(test, dcp_h17p_external_admit_dcp(dcp), -ENODEV);
	WRITE_ONCE(dcp->crashed, false);

	/* Refused by the real bind: skipped. */
	WRITE_ONCE(dcp->external_prepared, false);
	rt->status[0] = REFUSAL_RUNNING;
	KUNIT_ASSERT_EQ(test, dcp_comp_bind_h17p(dcp->dev, NULL, NULL), 0);
	KUNIT_EXPECT_EQ(test, dcp_h17p_external_admit_dcp(dcp), 1);
	KUNIT_EXPECT_FALSE(test, dcp_h17p_external_ready_dcp(dcp));
}

/*
 * NOT a real sequence: a refusal never sets external_prepared. This
 * deliberately inconsistent state checks the readiness predicate on its own.
 */
static void refusal_ready_predicate_inconsistent_test(struct kunit *test)
{
	struct refusal_test *rt = refusal_setup(test, &apple_dcp_hw_t8140_external, false);
	struct apple_dcp *dcp = rt->dcp;

	WRITE_ONCE(dcp->external_prepared, true);
	KUNIT_EXPECT_TRUE(test, dcp_h17p_external_ready_dcp(dcp));
	WRITE_ONCE(dcp->ext_refused, true);
	KUNIT_EXPECT_FALSE(test, dcp_h17p_external_ready_dcp(dcp));
	KUNIT_EXPECT_EQ(test, dcp_h17p_external_admit_dcp(dcp), 1);
}

static struct kunit_case dcp_h17p_refusal_cases[] = {
	KUNIT_CASE(refusal_first_check_test),
	KUNIT_CASE(refusal_run_cleared_test),
	KUNIT_CASE(refusal_internal_unchanged_test),
	KUNIT_CASE(refusal_pin_failure_strict_test),
	KUNIT_CASE(refusal_second_check_test),
	KUNIT_CASE(refusal_probe_order_test),
	KUNIT_CASE(refusal_probe_pin_failure_test),
	KUNIT_CASE(refusal_probe_success_prepared_test),
	KUNIT_CASE(refusal_component_bind_noop_test),
	KUNIT_CASE(refusal_entry_points_first_test),
	KUNIT_CASE(refusal_entry_points_second_test),
	KUNIT_CASE(refusal_reprobe_blocked_test),
	KUNIT_CASE(refusal_devres_and_routes_test),
	KUNIT_CASE(refusal_admission_test),
	KUNIT_CASE(refusal_ready_predicate_inconsistent_test),
	{}
};

static struct kunit_suite dcp_h17p_refusal_suite = {
	.name = "dcp_h17p_external_refusal",
	.test_cases = dcp_h17p_refusal_cases,
};

kunit_test_suite(dcp_h17p_refusal_suite);
