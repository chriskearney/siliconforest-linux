# SPDX-License-Identifier: GPL-2.0-only
# Copyright 2026 Ryan Murray
"""Actual disconnect authority, DWC3 teardown and firmware resource admission."""
from pathlib import Path
import re
from common import block, source_defines

def disconnect(root):
    # Copyright 2026 Ryan Murray

    r = root

    def function(file, name):
        s = (r / file).read_text()
        m = re.search(r'^[\w* \t]+\b' + name + r'\([^;{}]*\)\s*\{', s, re.M)
        assert m, name
        pos = m.end()
        depth = 1
        while depth:
            depth += (s[pos] == '{') - (s[pos] == '}')
            pos += 1
        return s[m.start():pos]

    pm = (r / 'drivers/usb/typec/tipd/cd321x-pm.h').read_text()
    pm = pm.replace('#include <linux/types.h>', '')
    core = 'drivers/usb/typec/tipd/core.c'
    dcp = 'drivers/gpu/drm/apple/dcp.c'
    funcs = '\n'.join(function(f, n) for f, n in (
        ('drivers/gpu/drm/apple/iomfb.c', 'iomfb_pause_presentation'),
        (dcp, 'dcp_external_report_disconnected'),
        (dcp, 'dcp_external_publish_generation'),
        (dcp, 'dcp_external_stop_locked'),
        (dcp, 'dcp_external_disconnect'),
        (dcp, 'dcp_external_stop_work'),
        (dcp, 'dcp_external_hotplug'),
        (dcp, 'dcp_external_link_locked'),
        (dcp, 'dcp_dptx_connect_oob'),
        (core, 'cd321x_retry_revalidation'),
        (core, 'cd321x_update_work')))
    prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
typedef uint32_t u32; typedef uint64_t u64;
#define guard(x) fake_guard
#define fake_guard(x) ((void)(x))
#define scoped_guard(x,y) for (int once = ((void)(y), 1); once; once = 0)
#define lockdep_assert_held(x) ((void)(x))
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x,v) ((x) = (v))
#define container_of(p,t,m) ((t *)((char *)(p) - offsetof(t,m)))
#define to_delayed_work(w) container_of(w,struct delayed_work,work)
#define msecs_to_jiffies(x) (x)
#define IS_REACHABLE(x) 1
#define CONFIG_DRM 1
#define IS_ERR(x) false
#define PTR_ERR(x) (-EINVAL)
#define dev_warn(...) ((void)0)
#define dev_err(...) ((void)0)
#define CD321X_DEBOUNCE_DELAY_MS 500
#define TPS_STATUS_PLUG_PRESENT (1U << 0)
#define TPS_STATUS_PLUG_UPSIDE_DOWN (1U << 1)
#define TPS_DATA_STATUS_USB2_CONNECTION (1U << 0)
#define TPS_DATA_STATUS_USB3_CONNECTION (1U << 1)
#define TPS_DATA_STATUS_TBT_CONNECTION (1U << 2)
#define TPS_DATA_STATUS_DP_CONNECTION (1U << 3)
#define TPS_DATA_STATUS_DP_PIN_ASSIGNMENT_MASK (3U << 4)
#define TPS_DATA_STATUS_USB_DATA_ROLE (1U << 6)
#define CD321X_DATA_STATUS_USB4_CONNECTION (1U << 7)
#define CD321X_DATA_STATUS_HPD_LEVEL (1U << 8)
#define TPS_POWER_STATUS_PWROPMODE(x) (x)
#define TPS_STATUS_TO_UPSIDE_DOWN(x) ((x)&TPS_STATUS_PLUG_UPSIDE_DOWN)
#define TPS_STATUS_TO_TYPEC_PORTROLE(x) 0
#define TPS_STATUS_TO_TYPEC_VCONN(x) 0
#define TPS_STATUS_TO_TYPEC_DATAROLE(x) 0
#define TYPEC_STATE_SAFE 0
#define TYPEC_STATE_USB 1
#define TYPEC_THUNDERBOLT_SWITCH_OFF 0
#define TYPEC_ACCESSORY_NONE 0
#define DCP_HOTPLUG_NOTIFY 1
struct work_struct { bool pending; };
struct delayed_work { struct work_struct work; };
static int system_freezable_wq;
static int schedule_work(struct work_struct *w) { w->pending = true; return 1; }
static void cancel_work_sync(struct work_struct *w) { w->pending = false; }
static void cancel_delayed_work_sync(struct delayed_work *w) { w->work.pending = false; }
#define cancel_delayed_work cancel_delayed_work_sync
static void mod_delayed_work(int q, struct delayed_work *w, int t)
{ (void)q; (void)t; w->work.pending = true; }
struct apple_connector { bool connected, edid_invalidation_pending; };
struct apple_dcp {
 int external_lock, hpd_mutex;
 struct { int lock; bool presentation_paused; } iomfb;
 struct { bool connected; void *service; } dptxport[1];
 struct apple_connector *connector;
 struct delayed_work typec_reconnect_wq, placeholder_edid_wq;
 struct work_struct external_stop_work;
 bool external_running, external_stopped, external_power_attempted;
 bool external_cable, typec_cable_connected, typec_crtc_off, pipe_enabled_h17p;
 bool external_physically_absent, external_prepared, placeholder_retried;
 int typec_reconnect_tries;
 bool external_hotplug_pending, external_hotplug_connected, quiescing;
 u64 external_generation, external_rx_generation, external_crtc_generation;
 u64 external_stop_generation, typec_generation;
 int external_error, mode_state; void *avep;
};
struct platform_device { struct apple_dcp *data; };
static struct apple_dcp *platform_get_drvdata(struct platform_device *p) { return p->data; }
static int raw_error, raw_links;
static int dcp_dptx_connect_raw(struct apple_dcp *d, u32 port, bool *waiting)
{ assert(port == 0); *waiting = false; raw_links++;
 if (!raw_error) d->dptxport[0].connected = true;
 return raw_error; }
static bool dcp_is_typec_output(struct apple_dcp *d) { (void)d; return true; }
static int dcp_dptx_connect(struct apple_dcp *d, u32 port) { (void)d; (void)port; return 0; }
#define DPTX_RECONNECT_DELAY 500
static int settle_error, power_error, hpd_error, release_error;
static int settles, powers, releases, detaches, notices;
static bool dcp_is_external_h17p(struct apple_dcp *d) { (void)d; return true; }
static unsigned dcp_mode_hotplug(int *mode, bool connected, bool *sink)
{ (void)mode; if (sink) *sink = connected; return 1; }
static void dcp_handle_hotplug_actions(struct apple_dcp *d, unsigned act)
{ (void)d; notices += !!act; }
static int iomfb_queue_settle(struct apple_dcp *d, int t)
{ (void)d; (void)t; settles++; return settle_error; }
static int iomfb_external_power_h17p(struct apple_dcp *d, bool on)
{ (void)d; assert(!on); powers++; return power_error; }
static bool dptx_service_available(void *p) { (void)p; return true; }
static int dptxport_set_hpd(void *p, bool on)
{ (void)p; assert(!on); return hpd_error; }
static int dptxport_release_display(void *p)
{ (void)p; releases++; return release_error; }
static void av_service_disconnect(struct apple_dcp *d) { (void)d; }
static void dpavservep_detach(struct apple_dcp *d) { (void)d; detaches++; }
static void dcp_firmware_metadata_hotplug(struct apple_dcp *d, bool c) { (void)d; (void)c; }
static void dcp_firmware_metadata_connection(struct apple_dcp *d, u64 g, bool c)
{ (void)d; (void)g; (void)c; }
static int dcp_external_disconnect(struct apple_dcp *, bool);
static struct apple_dcp display;
static void drm_connector_oob_hotplug_event(void *node, int status)
{ assert(node); if (!status) dcp_external_disconnect(&display, true); }
#define connector_status_disconnected 0
#define connector_status_connected 1
'''
    shim = r'''
enum usb_role { USB_ROLE_NONE, USB_ROLE_HOST, USB_ROLE_DEVICE };
enum typec_pwr_opmode { TYPEC_PWR_MODE_USB, TYPEC_PWR_MODE_PD };
enum typec_orientation { TYPEC_ORIENTATION_NONE, TYPEC_ORIENTATION_NORMAL, TYPEC_ORIENTATION_REVERSE };
struct usb_pd_identity { int id; };
struct typec_mux_state { void *alt, *data; int mode; };
struct typec_thunderbolt_switch_data { int state; };
struct typec_partner_desc { bool usb_pd; int accessory; struct usb_pd_identity *identity; };
struct cd321x_status { u32 status, data_status, status_changed, data_status_changed, pwr_status;
 struct usb_pd_identity partner_identity; };
struct tps6598x { void *partner, *dev, *port, *psy; int lock; u32 data_status; enum usb_role *role_sw; };
struct cd321x { struct tps6598x tps; struct cd321x_status update_status;
 struct cd321x_pm_state pm; struct delayed_work update_work, resume_work;
 struct typec_mux_state state; bool state_valid, display_route_active, cur_partner_is_pd;
 struct usb_pd_identity cur_partner_identity; void *connector_fwnode, *tbt_switch; };
static int tbt_error, role_error, safe_error, mode_error;
static int tbts, roles, safes;
static enum usb_role usb_role_switch_get_role(enum usb_role *r) { return *r; }
static int usb_role_switch_set_role(enum usb_role *r, enum usb_role n)
{ roles++; if (role_error) return role_error; *r = n; return 0; }
static int typec_thunderbolt_switch_set(void *p, void *s)
{ (void)p; (void)s; tbts++; return tbt_error; }
static int typec_set_mode(void *p, int s)
{ (void)p; assert(s == TYPEC_STATE_SAFE); safes++; return safe_error; }
static int cd321x_typec_update_mode(struct tps6598x *t, struct cd321x_status *s, bool *busy)
{ (void)t; (void)s; *busy = mode_error == -EBUSY; return mode_error; }
static void typec_unregister_partner(void *p) { (void)p; }
static void *typec_register_partner(void *p, struct typec_partner_desc *d)
{ (void)p; (void)d; return (void *)1; }
static void typec_partner_set_identity(void *p) { (void)p; }
#define typec_set_pwr_opmode(...) ((void)0)
#define typec_set_pwr_role(...) ((void)0)
#define typec_set_vconn_role(...) ((void)0)
#define typec_set_orientation(...) ((void)0)
#define typec_set_data_role(...) ((void)0)
#define power_supply_changed(...) ((void)0)
'''
    tests = r'''
static int failures, checks;
#define CHECK(x) do { checks++; if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); failures++; } } while (0)
static struct apple_connector sink;
static enum usb_role role;
static struct cd321x port;
static void init(void)
{
 memset(&display,0,sizeof(display)); memset(&sink,0,sizeof(sink)); memset(&port,0,sizeof(port));
 sink.connected = true; display.connector = &sink; display.external_prepared = true;
 raw_error = raw_links = 0;
 display.external_running = display.external_power_attempted = true;
 display.external_cable = display.typec_cable_connected = true;
 display.dptxport[0].connected = display.pipe_enabled_h17p = true;
 display.external_generation = display.external_rx_generation = display.external_crtc_generation = 7;
 display.typec_generation = 4;
 port.tps.partner = (void *)1; port.tps.role_sw = &role; role = USB_ROLE_HOST;
 port.pm.phase = CD321X_PM_RUNNING; port.state_valid = true; port.state.mode = 2;
 port.display_route_active = true; port.connector_fwnode = (void *)1;
 settle_error = power_error = hpd_error = release_error = 0;
 tbt_error = role_error = safe_error = mode_error = 0;
 settles = powers = releases = detaches = notices = tbts = roles = safes = 0;
}
static void callback(bool connected)
{
 if (!dcp_external_hotplug(&display,connected))
  dcp_mode_hotplug(&display.mode_state,connected,&sink.connected);
}
int main(void)
{
 /* Unplug at each fallible transition with failing DCP power teardown. */
 for (int which = 0; which < 3; which++) {
  init(); power_error = -ETIMEDOUT;
  if (which == 0) tbt_error = -ETIMEDOUT;
  if (which == 1) role_error = -EIO;
  if (which == 2) safe_error = -ETIMEDOUT;
  cd321x_update_work(&port.update_work.work);
  CHECK(!sink.connected); CHECK(!display.external_cable);
  CHECK(display.external_error == -ETIMEDOUT);
  CHECK(display.dptxport[0].connected && display.pipe_enabled_h17p);
  CHECK(!display.external_stopped && !display.typec_crtc_off);
  CHECK(port.display_route_active && !port.state_valid);
  CHECK(port.pm.phase == CD321X_PM_REVALIDATE && port.pm.attempts_left == CD321X_RESUME_ATTEMPTS);
  CHECK(notices > 0 && releases == 0);
  callback(true); CHECK(!sink.connected);
  printf("case transition-%d complete\n",which);
 }
 /* A rejected/nonowner port cannot disconnect the shared owner's sink. */
 for (int which = 0; which < 3; which++) {
  init(); port.display_route_active = false;
  if (which == 0) tbt_error = -EIO;
  if (which == 1) role_error = -EIO;
  if (which == 2) safe_error = -EIO;
  cd321x_update_work(&port.update_work.work);
  CHECK(sink.connected && display.external_cable && !notices);
 }
 init(); port.display_route_active = false; role = USB_ROLE_NONE;
 port.update_status.status = TPS_STATUS_PLUG_PRESENT;
 port.update_status.data_status = TPS_DATA_STATUS_DP_CONNECTION | CD321X_DATA_STATUS_HPD_LEVEL;
 mode_error = -EBUSY; cd321x_update_work(&port.update_work.work);
 CHECK(sink.connected && !notices && !port.display_route_active);
 /* Two ports share the connector: busy-port unplug leaves its owner live. */
 init();
 struct cd321x second = port;
 enum usb_role second_role = USB_ROLE_NONE;
 second.tps.role_sw = &second_role; second.display_route_active = false;
 second.update_status.status = TPS_STATUS_PLUG_PRESENT;
 second.update_status.data_status = TPS_DATA_STATUS_DP_CONNECTION | CD321X_DATA_STATUS_HPD_LEVEL;
 mode_error = -EBUSY; cd321x_update_work(&second.update_work.work);
 CHECK(!second.display_route_active && sink.connected && !notices);
 second.update_status.status = second.update_status.data_status = 0;
 tbt_error = -EIO; cd321x_update_work(&second.update_work.work);
 CHECK(port.display_route_active && sink.connected && !notices);
 power_error = -EIO; cd321x_update_work(&port.update_work.work);
 CHECK(!sink.connected && port.display_route_active);
 CHECK(display.dptxport[0].connected && display.pipe_enabled_h17p);
 /* Revalidation is bounded even though the failed hardware remains owned. */
 init(); tbt_error = -EIO;
 cd321x_update_work(&port.update_work.work);
 for (int i = 0; i < CD321X_RESUME_ATTEMPTS; i++) {
  CHECK(cd321x_pm_begin_read(&port.pm)); cd321x_pm_snapshot_ready(&port.pm);
  cd321x_update_work(&port.update_work.work);
 }
 CHECK(port.pm.phase == CD321X_PM_STALE && !port.pm.attempts_left);
 CHECK(!sink.connected && port.display_route_active);
 /* Stop failure stages preserve all state whose shutdown was not acked. */
 for (int which = 0; which < 4; which++) {
  init();
  if (which == 0) settle_error = -EIO;
  if (which == 1) power_error = -EIO;
  if (which == 2) hpd_error = -EIO;
  if (which == 3) release_error = -EIO;
  CHECK(dcp_external_disconnect(&display,true) == -EIO);
  CHECK(!sink.connected && !display.typec_cable_connected);
  CHECK(display.external_generation == 8 && display.external_rx_generation == 8);
  CHECK(display.external_error == -EIO && display.dptxport[0].connected);
  CHECK(!display.external_stopped && !display.typec_crtc_off);
  CHECK(display.pipe_enabled_h17p == (which < 2));
  callback(true); CHECK(!sink.connected);
 }
 /* An existing error returns before pause: late RX still cannot resurrect. */
 init(); display.external_error = -EIO;
 CHECK(dcp_external_disconnect(&display,true) == -EIO);
 CHECK(!display.iomfb.presentation_paused);
 callback(true); CHECK(!sink.connected);
 CHECK(display.dptxport[0].connected && display.pipe_enabled_h17p);
 /* A queued callback from the old sink cannot be replayed after unplug. */
 init(); display.external_error = -EIO;
 display.iomfb.presentation_paused = true; callback(true);
 CHECK(display.external_hotplug_pending);
 CHECK(dcp_external_disconnect(&display,true) == -EIO);
 iomfb_pause_presentation(&display,false); CHECK(!sink.connected);
 /* An old stop generation cannot touch a later session. */
 init(); display.external_stop_generation = 6;
 dcp_external_stop_work(&display.external_stop_work);
 CHECK(sink.connected && settles == 0 && powers == 0 && releases == 0);
 CHECK(dcp_external_stop_locked(&display,6) == 0 && sink.connected);
 /* Same-cable CRTC off remains distinct from physical absence. */
 init(); CHECK(dcp_external_disconnect(&display,false) == 0);
 CHECK(display.external_cable && display.typec_cable_connected);
 CHECK(display.external_generation == 7);
 /* Genuine discovery and the paused DPMS generation gate still work. */
 init(); sink.connected = false; callback(true); CHECK(sink.connected);
 init(); sink.connected = false; display.iomfb.presentation_paused = true;
 display.external_power_attempted = false; callback(true); CHECK(sink.connected);
 init(); sink.connected = false; display.iomfb.presentation_paused = true;
 display.external_stopped = true; display.external_rx_generation = 8;
 callback(true); CHECK(sink.connected);
 init(); sink.connected = false; display.iomfb.presentation_paused = true;
 display.external_stopped = true; callback(true); CHECK(!sink.connected);
 init(); sink.connected = false; display.iomfb.presentation_paused = true;
 display.external_error = -EIO; callback(true); CHECK(!sink.connected);
 /* Fresh firmware discovery precedes OOB: no second RX is required. */
 init(); sink.connected = false;
 display.external_cable = display.typec_cable_connected = false;
 display.external_generation = display.external_rx_generation = 37;
 display.external_power_attempted = false; display.external_stopped = true;
 display.dptxport[0].connected = false;
 callback(true); CHECK(sink.connected); CHECK(!display.external_physically_absent);
 struct platform_device device = { .data = &display };
 CHECK(dcp_dptx_connect_oob(&device,0) == 0);
 CHECK(raw_links == 1 && sink.connected);
 CHECK(display.external_generation == 38);
 /* Initially paused discovery keeps the original pending-replay behavior. */
 init(); sink.connected = false; display.external_cable = false;
 display.external_power_attempted = false; display.iomfb.presentation_paused = true;
 callback(true); CHECK(display.external_hotplug_pending); CHECK(!sink.connected);
 CHECK(dcp_dptx_connect_oob(&device,0) == 0 && raw_links == 1);
 iomfb_pause_presentation(&display,false); CHECK(sink.connected);
 /* Physical absence consumes late RX and clears an already queued replay. */
 init(); display.iomfb.presentation_paused = true; callback(true);
 CHECK(display.external_hotplug_pending);
 CHECK(dcp_external_disconnect(&display,true) == 0);
 CHECK(display.external_physically_absent && !display.external_hotplug_pending);
 callback(true); CHECK(!sink.connected && !display.external_hotplug_pending);
 /* A real next OOB connect removes the tombstone and permits discovery. */
 CHECK(dcp_dptx_connect_oob(&device,0) == 0);
 CHECK(!display.external_physically_absent && display.external_cable);
 CHECK(display.external_generation == 9 && raw_links == 1);
 callback(true); CHECK(sink.connected);
 /* Same-cable DPMS does not create a physical-absence tombstone. */
 init(); CHECK(dcp_external_disconnect(&display,false) == 0);
 CHECK(!display.external_physically_absent && display.external_cable);
 sink.connected = false; callback(true); CHECK(!sink.connected);
 CHECK(display.external_hotplug_pending);
 iomfb_pause_presentation(&display,false); CHECK(sink.connected);
 /* A failed hardware stop remains owned, even after a real OOB reconnect. */
 init(); display.external_error = -EIO;
 CHECK(dcp_external_disconnect(&display,true) == -EIO);
 CHECK(display.external_physically_absent); callback(true); CHECK(!sink.connected);
 CHECK(dcp_dptx_connect_oob(&device,0) == -EIO);
 CHECK(!display.external_physically_absent && display.external_error == -EIO);
 CHECK(raw_links == 0 && display.dptxport[0].connected && display.pipe_enabled_h17p);
 /* Successful physical teardown may release ownership normally. */
 init(); CHECK(dcp_external_disconnect(&display,true) == 0);
 CHECK(!sink.connected && !display.dptxport[0].connected);
 CHECK(display.external_stopped && display.typec_crtc_off && !display.pipe_enabled_h17p);
 printf("%d assertions, %d failures\n", checks, failures); return !!failures;
}
'''
    source = prefix + pm + shim + funcs + tests
    return source

def teardown(root):

    files = {n: (root / f).read_text() for n, f in {
        'atc': 'drivers/phy/apple/atc.c',
        'dp': 'drivers/phy/apple/atc-j700-dp.h',
        'phy': 'drivers/phy/phy-core.c',
        'core': 'drivers/usb/dwc3/core.c',
        'apple': 'drivers/usb/dwc3/dwc3-apple.c',
        'state': 'drivers/usb/dwc3/dwc3-apple-state.h',
    }.items()}


    def fn(n, name):
        s = files[n]
        normalized = n == 'atc' and name == 'atcphy_mux_set' and 'static int atcphy_mux_set_locked(' in s
        if normalized:
            name = 'atcphy_mux_set_locked'
        m = re.search(r'^(?:static [^\n]*\b|(?:int|void) )' + name + r'\(', s, re.M)
        assert m, (n, name)
        result = block(s, m.start())
        return result.replace('atcphy_mux_set_locked(', 'atcphy_mux_set(', 1) if normalized else result

    decls = []
    for name in ('atcphy_generation', 'atcphy_lane_mode', 'atcphy_pipehandler_state', 'atcphy_mode', 'atcphy_dp_link_rate'):
        decls.append(block(files['atc'], files['atc'].index('enum ' + name + ' {')) + ';')
    for name in ('atcphy_mode_configuration', 'atcphy_hw', 'atcphy_dp_link_rate_configuration'):
        decls.append(block(files['atc'], files['atc'].index('struct ' + name + ' {')) + ';')
    decls.append(block(files['state'], files['state'].index('enum dwc3_apple_state {')) + ';')
    start = files['atc'].index('static const struct {\n\tconst struct atcphy_mode_configuration normal;')
    end = files['atc'].index('\n};', start) + 3
    decls.append(files['atc'][start:end])
    start = files['atc'].index('static const struct atcphy_dp_link_rate_configuration dp_lr_config[]')
    end = files['atc'].index('\n};', start) + 3
    decls.append(files['atc'][start:end])

    names = {
        'atc': ('atcphy_pipehandler_lock', 'atcphy_pipehandler_unlock',
                'atcphy_pipehandler_check', 'atcphy_pipehandler_set_mux',
                'atcphy_clear_nonselected_phy_reset',
                'atcphy_configure_pipehandler_usb3_host_t8103',
                'atcphy_configure_pipehandler_usb3_host_t8122',
                'atcphy_configure_pipehandler_usb3', 'atcphy_configure_pipehandler_usb4',
                'atcphy_enable_dummy_phy', 'atcphy_configure_pipehandler_dummy',
                'atcphy_configure_pipehandler', 'atcphy_park_pipehandler',
                'atcphy_get_mode_config', 'atcphy_dp_configure',
                'j700_field_update', 'j700_aux_disable', 'atcphy_disable_dp_aux',
                'atcphy_power_off', 'atcphy_power_on', 'atcphy_configure', 'atcphy_usb2_init', 'atcphy_usb3_power_off',
                'atcphy_usb3_set_mode', 'atcphy_usb2_power_off',
                '_atcphy_dwc3_reset_assert', 'atcphy_dwc3_reset_assert', 'atcphy_mux_set'),
        'phy': ('phy_init', 'phy_exit', 'phy_power_on', 'phy_power_off'),
        'core': ('dwc3_phy_power_on', 'dwc3_phy_power_off', 'dwc3_phy_exit', 'dwc3_core_exit'),
        'apple': ('dwc3_apple_core_stop', 'dwc3_apple_core_init', 'dwc3_apple_core_start',
                  'dwc3_apple_init', 'dwc3_apple_exit', 'dwc3_usb_role_switch_set'),
    }
    names['core'] = ('dwc3_phy_power_on', 'dwc3_phy_power_off', 'dwc3_phy_exit',
                     'dwc3_core_exit_after_phy_power_off', 'dwc3_core_exit',
                     'dwc3_core_remove_common', 'dwc3_core_remove', 'dwc3_core_remove_no_shutdown')
    names['atc'] += ('atcphy_dwc3_reset_deassert',)
    names['atc'] = tuple(name for name in names['atc'] if name not in ('atcphy_get_mode_config', 'j700_field_update'))
    names = {n: tuple(name for name in group if re.search(r'^(?:static [^\n]*\b|(?:int|void) )' + name + r'\(', files[n], re.M))
             for n, group in names.items()}
    context = (fn('atc', 'core_read32') + '\n' + fn('atc', 'atcphy_get_mode_config') + '\n' + fn('atc', 'j700_field_update') + '\n' + files['dp'][files['dp'].index('/* Masks are the union'):files['dp'].index('/* Retry only the idle operations')])
    functions = '\n\n'.join(fn(n, name) for n, group in names.items() for name in group)
    definitions = []
    lines = iter(files['atc'].splitlines())
    for line in lines:
        if not line.startswith('#define '):
            continue
        definition = [line]
        while definition[-1].endswith('\\'):
            definition.append(next(lines))
        definitions.append('\n'.join(definition))
    definitions = '\n'.join(definitions)
    here = Path(__file__).resolve().parent
    fallback = '' if 'dwc3_core_remove_no_shutdown' in names['core'] else '#define dwc3_core_remove_no_shutdown dwc3_core_remove\n'
    unit = ((here / 'dwc3/prefix.c').read_text() + '\n' + definitions + '\n' +
            '\n'.join(decls) + '\n' + (here / 'dwc3/mocks.c').read_text() + '\n' +
            context + '\n' + functions + '\n' + fallback + (here / 'dwc3/cases.c').read_text())
    return unit

def profile(root):

    base = root / "drivers/gpu/drm/apple"
    source = (base / "dcp.c").read_text()


    def block(marker):
        start = source.index(marker)
        opening = source.index("{", start)
        end, depth = opening + 1, 1
        while depth:
            depth += (source[end] == "{") - (source[end] == "}")
            end += 1
        return source[start:end]


    clock = source[source.index("static const struct resource t8140_firmware_clock"):
                   source.index("static const struct apple_dcp_hw_data apple_dcp_hw_t8140 =")]
    profiles = clock + "\n" + "\n".join(block(marker) + ";" for marker in (
        "static const struct apple_dcp_hw_data apple_dcp_hw_t8140 =",
        "static const struct apple_dcp_hw_data apple_dcp_hw_t8140_external =",
    ))
    admission = block("static int dcp_get_disp_regs(")
    headers = "\n".join(line for name in ("dcp-external-bw.h", "dcp-link.h")
                        for line in (base / name).read_text().splitlines()
                        if not line.startswith("#include"))
    prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
typedef uint32_t u32; typedef uint64_t u64;
typedef uint32_t __le32; typedef uint64_t __le64;
#define __packed __attribute__((packed))
#undef static_assert
#define static_assert(condition, ...) _Static_assert(condition, "wire layout")
#define le32_to_cpu(v) (v)
#define cpu_to_le32(v) (v)
#define cpu_to_le64(v) (v)
#define IORESOURCE_MEM 1
#define MAX_DISP_REGISTERS 7
#define DCP_FIRMWARE_H17P 3
#define DCP_IOMFB_METHODS_H17P_EXTERNAL 2
#define DEFINE_RES_MEM(s,n) { .start=(s), .end=(s)+(n)-1 }
#define resource_size(r) ((r)->end-(r)->start+1)
struct resource { u64 start,end; };
struct device_node { bool named; unsigned count; };
struct device { struct device_node *of_node; };
struct platform_device { struct device dev; int num_resources; struct resource resources[10]; };
#define to_platform_device(d) ((struct platform_device *)(d))
struct apple_dcp_hw_data {
    unsigned num_dptx_ports; bool adopt_live_session;
    int iomfb_method_profile,firmware_compat;
    const struct resource *firmware_clock;
    u32 firmware_display_windows,firmware_scratch,firmware_request;
};
struct apple_dcp {
    struct device *dev; struct apple_dcp_hw_data hw;
    struct resource *disp_registers[10],disp_bw_scratch_res,disp_bw_doorbell_res;
    u32 disp_bw_scratch_index,disp_bw_scratch_offset,disp_bw_doorbell_index,nr_disp_registers;
};
static bool of_property_present(struct device_node *n,const char *name)
{ return !strcmp(name,"reg-names") && n->named; }
static int of_property_count_strings(struct device_node *n,const char *name)
{ assert(!strcmp(name,"reg-names"));return n->count+1; }
static int of_property_read_string_index(struct device_node *n,const char *name,int i,const char **v)
{
    static const char *names[]={"coproc","disp-0","disp-1","disp-2","disp-3","disp-4","disp-5"};
    assert(!strcmp(name,"reg-names"));if(i<0 || (unsigned)i>n->count)return -EINVAL;
    *v=names[i];return 0;
}
static struct resource *platform_get_resource(struct platform_device *p,int type,int i)
{ assert(type==IORESOURCE_MEM);return i>=1 && i<p->num_resources?&p->resources[i-1]:NULL; }
static struct resource *platform_get_resource_byname(struct platform_device *p,int type,const char *name)
{ int i=-1;assert(sscanf(name,"disp-%d",&i)==1);return platform_get_resource(p,type,i+1); }
static int dcp_get_bw_scratch_reg(struct apple_dcp *p,u32 count) { (void)p;(void)count;return -ENOENT; }
static int dcp_get_bw_doorbell_reg(struct apple_dcp *p,u32 count) { (void)p;(void)count;return -ENOENT; }
'''
    tests = r'''
static struct platform_device provider;
static struct device_node node;
static struct apple_dcp d;
static void reset(bool external,bool named)
{
    node=(struct device_node){.named=named,.count=5};
    provider=(struct platform_device){.dev.of_node=&node,.num_resources=6};
    for(unsigned i=0;i<5;i++)provider.resources[i]=(struct resource)DEFINE_RES_MEM(0x1000ULL*i,0x1000);
    provider.resources[4]=t8140_firmware_clock;
    d=(struct apple_dcp){.dev=&provider.dev,.hw=external?apple_dcp_hw_t8140_external:apple_dcp_hw_t8140};
}
int main(void)
{
    const u32 fw[]={26,6,2},unknown[]={0,0,0};
    assert(apple_dcp_external_bw_valid(0xbc000,0x28000,0x70000));
    assert(!apple_dcp_external_bw_valid(0xbc000,0x28002,0x70000));
    struct apple_dcp_h17p_clock_request clock={.clock_id=APPLE_DCP_H17P_CLOCK_ID_154};u64 rate;
    assert(!apple_dcp_h17p_clock_rate(&clock,123,456,&rate) && rate==123);
    assert(apple_dcp_external_firmware_supported(fw,3,fw,3));
    assert(!apple_dcp_external_firmware_supported(NULL,3,fw,3));
    assert(!apple_dcp_external_firmware_supported(fw,3,NULL,3));
    assert(!apple_dcp_external_firmware_supported(unknown,3,fw,3));
    assert(apple_dcp_hw_t8140.num_dptx_ports==0 && apple_dcp_hw_t8140.adopt_live_session);
    assert(apple_dcp_hw_t8140.firmware_display_windows==5);
    assert(apple_dcp_hw_t8140.firmware_scratch==0x20000 && apple_dcp_hw_t8140.firmware_request==0x68000);
    assert(apple_dcp_hw_t8140_external.num_dptx_ports==1 && !apple_dcp_hw_t8140_external.adopt_live_session);
    assert(apple_dcp_hw_t8140_external.firmware_display_windows==4);
    assert(apple_dcp_hw_t8140_external.firmware_scratch==0x28000 && apple_dcp_hw_t8140_external.firmware_request==0x70000);
    for(unsigned named=0;named<2;named++) {
        reset(true,named);assert(dcp_get_disp_regs(&d)==0);
        assert(d.nr_disp_registers==5 && d.disp_bw_scratch_index==4);
        assert(d.disp_bw_scratch_res.start+d.disp_bw_scratch_offset==0x302828000ULL);
        assert(d.disp_bw_doorbell_res.start==0x302870000ULL && resource_size(&d.disp_bw_doorbell_res)==4);
        struct apple_dcp_h17p_rt_bw_request request={0};struct apple_dcp_h17p_rt_bw_reply reply;
        assert(!apple_dcp_h17p_rt_bw_encode_reply(&request,&reply,d.disp_bw_scratch_res.start+d.disp_bw_scratch_offset,d.disp_bw_doorbell_res.start));
        assert(reply.config.scratch==0x302828000ULL && reply.config.clock_request==0x302870000ULL && !reply.status);
        request.config_null=1;assert(apple_dcp_h17p_rt_bw_encode_reply(&request,&reply,0,0)==-EINVAL);
        reset(false,named);assert(dcp_get_disp_regs(&d)==0 && d.nr_disp_registers==6);
        assert(d.disp_bw_scratch_offset==0x20000 && d.disp_bw_doorbell_res.start==0x302868000ULL);
        reset(true,named);provider.resources[4].start+=4;assert(dcp_get_disp_regs(&d)==-EINVAL);
        reset(true,named);provider.resources[4].end-=4;assert(dcp_get_disp_regs(&d)==-EINVAL);
        reset(true,named);node.count=4;provider.num_resources=5;assert(dcp_get_disp_regs(&d)==-EINVAL);
        reset(true,named);d.hw.firmware_scratch=0xbc000;assert(dcp_get_disp_regs(&d)==-EINVAL);
        reset(true,named);d.hw.firmware_request=0xbc000;assert(dcp_get_disp_regs(&d)==-EINVAL);
        reset(true,named);d.hw.firmware_display_windows=0;assert(dcp_get_disp_regs(&d)==-EINVAL);
    }
    puts("PASS: actual internal/external profiles, resource admission and D003 bandwidth reply");
}
'''
    return prefix + headers + "\n" + profiles + admission + tests

def firmware(root):

    source = (root / "drivers/gpu/drm/apple/iomfb_h17p.c").read_text()
    body = source[source.index("struct dcp_h17p_provider_request {"):
                  source.index("static bool trampoline_luma_span_h17p")]
    prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint32_t __le32;
#define __packed __attribute__((packed))
#undef static_assert
#define static_assert(cond) _Static_assert(cond, "")
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define sizeof_field(t, f) sizeof(((t *)0)->f)
#define le32_to_cpu(x) (x)
#define cpu_to_le32(x) (x)
#define WRITE_ONCE(x, v) ((x) = (v))
#define DCP_IOMFB_METHODS_H17G 1
#define DCP_IOMFB_METHODS_H17P_EXTERNAL 2
#define trace_iomfb_callback(d, t, n) ((void)(d), (void)(t))
#define dev_err(...) ((void)0)
#define dev_warn_once(...) ((void)0)
struct device_node { bool internal, external, present; int length; };
struct device { struct device_node *of_node; };
struct apple_dcp { struct { int iomfb_method_profile; } hw; struct device *dev; bool crashed; };
static bool j700 = true;
static bool of_machine_is_compatible(const char *name)
{
	assert(!strcmp(name, "apple,j700"));
	return j700;
}
static bool of_device_is_compatible(struct device_node *n, const char *name)
{
	return (!strcmp(name, "apple,t8140-dcp") && n->internal) ||
	       (!strcmp(name, "apple,t8140-dcpext") && n->external);
}
static bool of_property_present(struct device_node *n, const char *name)
{
	(void)name;
	return n->present;
}
static int of_property_count_u8_elems(struct device_node *n, const char *name)
{
	(void)name;
	return n->present ? n->length : -EINVAL;
}
static int of_property_read_u8_array(struct device_node *n, const char *name, void *p, unsigned int len)
{
	(void)n; (void)name;
	memset(p, 0x5a, len);
	return 0;
}
static const void *memchr_inv(const void *p, int value, size_t size)
{
	const u8 *b = p;
	for (size_t i = 0; i < size; i++)
		if (b[i] != value)
			return b + i;
	return NULL;
}
'''
    suffix = r'''
int main(void)
{
	struct device_node node = { .external = true };
	struct device dev = { .of_node = &node };
	struct apple_dcp dcp = { .dev = &dev, .hw.iomfb_method_profile = 2 };
	struct dcp_h17p_provider_request req = { .scope = { 'V', 'O', 'R', 'P' }, .capacity = 0xc00 };
	struct dcp_h17p_provider_reply reply;
	unsigned int cases = 0;
	for (unsigned int i = 0; i < ARRAY_SIZE(provider_properties); i++) {
		memset(&reply, 0xff, sizeof(reply));
		strcpy(req.name, provider_properties[i].name);
		assert(trampoline_provider_property_h17p(&dcp, 400, &reply, &req));
		assert(!dcp.crashed && reply.length == provider_properties[i].length);
		if (reply.length)
			assert(!memcmp(reply.data, provider_properties[i].legacy_data, reply.length));
		cases++;
	}
	strcpy(req.name, "power-lut-data-x");
	j700 = false;
	assert(!trampoline_provider_property_h17p(&dcp, 400, &reply, &req) && dcp.crashed);
	j700 = true; dcp.crashed = false; dcp.hw.iomfb_method_profile = 3;
	assert(!trampoline_provider_property_h17p(&dcp, 400, &reply, &req) && dcp.crashed);
	node.external = false; node.internal = true; dcp.crashed = false;
	assert(trampoline_provider_property_h17p(&dcp, 400, &reply, &req) && !dcp.crashed);
	node.internal = false; node.external = true; node.present = true; node.length = 3;
	dcp.hw.iomfb_method_profile = 2; dcp.crashed = false;
	assert(!trampoline_provider_property_h17p(&dcp, 400, &reply, &req) && dcp.crashed);
	node.length = 4; dcp.crashed = false;
	assert(trampoline_provider_property_h17p(&dcp, 400, &reply, &req) && !dcp.crashed);
	assert(reply.length == 4 && reply.data[0] == 0x5a);
	strcpy(req.name, "unqualified-property");
	assert(!trampoline_provider_property_h17p(&dcp, 400, &reply, &req));
	printf("PASS %u provider payloads and scope/length rejection cases\n", cases);
	return 0;
}
'''
    return prefix + body + suffix

def readiness(root):

    dcp = (root / 'drivers/gpu/drm/apple/dcp.c').read_text()
    drm = (root / 'drivers/gpu/drm/apple/apple_drv.c').read_text()


    def function(source, marker):
        start = source.index(marker)
        opening = source.index('{', start)
        end, depth = opening + 1, 1
        while depth:
            depth += (source[end] == '{') - (source[end] == '}')
            end += 1
        return source[start:end]


    # This slice is the production probe's publication boundary, including its
    # failure exits and prepared-before-component_add ordering, not a copied model.
    probe = function(dcp, 'static int dcp_platform_probe(')
    publish = probe[probe.rindex('\tif (dcp->hw.iomfb_method_profile ==\n\t    DCP_IOMFB_METHODS_H17P_EXTERNAL) {'):]
    production = '\n'.join((
        function(dcp, 'bool dcp_h17p_external_supported('),
        function(dcp, 'bool dcp_h17p_external_ready('),
        function(drm, 'static bool apple_dcp_usable('),
        function(drm, 'static int add_dcp_components('),
        function(drm, 'static int apple_drm_init_dcp('),
        'static int publish_provider(struct platform_device *pdev, struct apple_dcp *dcp)\n'
        '{\n\tstruct device *dev = &pdev->dev;\n\tint ret;\n' + publish,
    ))
    prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
typedef uint32_t u32;
typedef uint64_t u64;
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x, v) ((x) = (v))
#define IS_ENABLED(x) 0
#define MAX_COPROCESSORS 4
#define DL_FLAG_AUTOREMOVE_SUPPLIER 0
#define DCP_IOMFB_METHODS_H17P_EXTERNAL 2
#define dev_warn(...) ((void)0)
#define msleep(x) ((void)(x))
#define component_compare_of 0
#define apple_dcp_id_tbl 0
static int dcp_comp_ops;
struct device_node { bool available, external, phys; unsigned int index; u32 firmware[3], compat[3]; int firmware_cells, compat_cells; };
struct device { struct device_node *of_node; void *data; };
struct platform_device { struct device dev; bool bound, registered; };
struct apple_dcp {
    bool external_prepared, quiescing, crashed;
    struct { int iomfb_method_profile; } hw;
};
struct drm_device { int unused; };
struct drm_encoder { int unused; };
struct apple_drm_private { struct drm_device drm; };
struct component_match { bool includes[2]; };
static struct device_node nodes[2];
static struct platform_device devices[2];
static struct apple_dcp dcps[2];
static struct apple_drm_private priv;
static struct device master = { .data = &priv };
static struct component_match matches;
static bool machine_supported, absent_external;
static unsigned int references, released, node_puts, probes[2], bind_attempts, disables;
static int preflight_error, external_probe_error, bind_result;
static void try_bind(void);
static bool of_machine_is_compatible(const char *s)
{ assert(!strcmp(s,"apple,j700")); return machine_supported; }
static int of_property_read_variable_u32_array(struct device_node *n,const char *s,u32 *v,int min,int max)
{
    assert(min==3 && max==3);
    bool fw=!strcmp(s,"apple,firmware-version");
    assert(fw || !strcmp(s,"apple,firmware-compat"));
    int count=fw?n->firmware_cells:n->compat_cells;
    if(count!=3) return -EINVAL;
    memcpy(v,fw?n->firmware:n->compat,3*sizeof(*v)); return count;
}
static bool of_device_is_available(struct device_node *n) { return n->available; }
static bool of_device_is_compatible(struct device_node *n, const char *s)
{
    return n->external && (!strcmp(s, "apple,t8140-dcpext") || !strcmp(s, "apple,dcpext"));
}
static bool of_property_present(struct device_node *n, const char *s)
{ assert(!strcmp(s, "phys")); return n->phys; }
#define for_each_matching_node(np, table) \
    for (unsigned int node_i = 0; node_i < 2 && ((np) = &nodes[node_i]); node_i++)
#define for_each_endpoint_of_node(np, endpoint) \
    for ((endpoint) = NULL; (endpoint) != NULL; (endpoint) = NULL)
static struct device_node *of_graph_get_remote_port_parent(struct device_node *n)
{ return n; }
static void of_node_put(struct device_node *n) { (void)n; node_puts++; }
static struct platform_device *of_find_device_by_node(struct device_node *n)
{
    if (n->external && absent_external)
        return NULL;
    references++;
    return &devices[n->index];
}
static void *platform_get_drvdata(struct platform_device *p) { return p->dev.data; }
static void *dev_get_drvdata(struct device *d) { return d->data; }
static void put_device(struct device *d) { (void)d; released++; }
/* Deliberately provided: using this during synchronous bind is a regression. */
#define device_is_bound(dev) (devices[(dev)->of_node->index].bound)
static void drm_of_component_match_add(struct device *d, struct component_match **m,
                                       int compare, struct device_node *n)
{ (void)d; (void)compare; *m = &matches; matches.includes[n->index] = true; }
static void device_link_add(struct device *a, struct device *b, int flags)
{ (void)a; (void)b; (void)flags; }
static int apple_probe_per_dcp(struct device *d, struct drm_device *drm,
                              struct platform_device *p, int index, bool external,
                              struct drm_encoder **encoder, u32 *mask)
{
    (void)d; (void)drm; (void)index; (void)encoder; (void)mask;
    assert(external == p->dev.of_node->external);
    probes[p->dev.of_node->index]++;
    return external ? external_probe_error : 0;
}
static int apple_probe_typec_ports(struct drm_device *d, struct platform_device **p,
                                  u32 *masks, int count)
{ (void)d; (void)p; (void)masks; (void)count; return 0; }
static int dcp_start(struct platform_device *p) { (void)p; return 0; }
static int dcp_wait_ready(struct platform_device *p, u64 timeout)
{ (void)p; (void)timeout; return 0; }
static u64 get_jiffies_64(void) { return 0; }
static u64 msecs_to_jiffies(unsigned int ms) { return ms; }
static bool time_after_eq64(u64 a, u64 b) { return a >= b; }
static int dcp_comp_bind(struct device *d, void *master_data, void *data)
{ (void)d; (void)master_data; (void)data; return preflight_error; }
static void dcp_disable_typec_work(struct apple_dcp *d, bool quiesce)
{ disables++; d->quiescing = quiesce; }
static int component_add(struct device *d, const void *ops)
{
    (void)ops;
    struct platform_device *p = &devices[d->of_node->index];
    assert(!p->bound);
    p->registered = true;
    try_bind(); /* Linux component_add may bind the master before probe returns. */
    return 0;
}
'''
    suffix = r'''
static void try_bind(void)
{
    for (unsigned int i = 0; i < 2; i++)
        if (matches.includes[i] && !devices[i].registered)
            return;
    bind_attempts++;
    bind_result = apple_drm_init_dcp(&master);
}
static void initial(bool supported_firmware)
{
    memset(nodes, 0, sizeof(nodes));
    memset(devices, 0, sizeof(devices));
    memset(dcps, 0, sizeof(dcps));
    memset(&matches, 0, sizeof(matches));
    memset(probes, 0, sizeof(probes));
    for (unsigned int i = 0; i < 2; i++) {
        nodes[i] = (struct device_node){ .available = true, .external = i == 1, .index = i };
        devices[i].dev = (struct device){ .of_node = &nodes[i], .data = &dcps[i] };
    }
    dcps[1].hw.iomfb_method_profile = DCP_IOMFB_METHODS_H17P_EXTERNAL;
    devices[0].registered = devices[0].bound = true;
    machine_supported = true;
    nodes[1].firmware_cells=nodes[1].compat_cells=3;
    if(supported_firmware) {
        u32 version[]={26,6,2};
        memcpy(nodes[1].firmware,version,sizeof(version));
        memcpy(nodes[1].compat,version,sizeof(version));
    }
    absent_external = false;
    references = released = node_puts = bind_attempts = disables = 0;
    preflight_error = external_probe_error = bind_result = 0;
}
static int match_providers(void)
{
    struct component_match *match = NULL;
    return add_dcp_components(&master, &match);
}
int main(void)
{
    assert(apple_dcp_external_bw_valid(0xbc000,0x28000,0x70000));
    assert(apple_dcp_external_clock_window(0x302800000ULL,0xbc000,0x302800000ULL,0xbc000));
    /* No command line state: exact J700 firmware selects the external profile. */
    initial(true); assert(dcp_h17p_external_supported(&nodes[1]));
    machine_supported=false;
    assert(!dcp_h17p_external_supported(&nodes[1]) && match_providers()==1);
    for(unsigned i=0;i<3;i++) {
        initial(true); nodes[1].firmware[i]++;
        assert(!dcp_h17p_external_supported(&nodes[1]) && match_providers()==1);
        initial(true); nodes[1].compat[i]++;
        assert(!dcp_h17p_external_supported(&nodes[1]) && match_providers()==1);
    }
    for(int count=-1;count<=5;count++) if(count!=3) {
        initial(true); nodes[1].firmware_cells=count;
        assert(!dcp_h17p_external_supported(&nodes[1]));
        initial(true); nodes[1].compat_cells=count;
        assert(!dcp_h17p_external_supported(&nodes[1]));
    }

    /* Unknown external firmware preserves the normal internal provider. */
    initial(false);
    assert(match_providers() == 1 && matches.includes[0] && !matches.includes[1]);
    assert(!dcp_h17p_external_ready(&nodes[1]) && references == 0);
    try_bind();
    assert(bind_attempts == 1 && bind_result == 0 && probes[0] == 1 && !probes[1]);

    /* Delayed external provider remains in the match; the master must wait. */
    initial(true);
    assert(match_providers() == 2 && matches.includes[1]);
    try_bind();
    assert(bind_attempts == 0 && !probes[0] && !probes[1]);
    /* Last provider is prepared, but driver core bound is still false here. */
    assert(publish_provider(&devices[1], &dcps[1]) == 0);
    assert(dcps[1].external_prepared && !devices[1].bound);
    assert(bind_attempts == 1 && bind_result == 0 && probes[0] == 1 && probes[1] == 1);
    devices[1].bound = true;

    /* Permanent preflight failure cannot silently drop a requested provider. */
    initial(true);
    assert(match_providers() == 2);
    preflight_error = -EIO;
    assert(publish_provider(&devices[1], &dcps[1]) == -EIO);
    assert(!devices[1].registered && !dcps[1].external_prepared && disables == 1);
    try_bind();
    assert(matches.includes[1] && bind_attempts == 0 && !probes[0]);

    /* An unavailable DT node is excluded; a matched but absent device is fatal. */
    initial(true);
    nodes[1].available = false;
    assert(!apple_dcp_usable(&nodes[1]) && match_providers() == 1);
    initial(true);
    absent_external = true;
    assert(match_providers() == 2);
    assert(apple_drm_init_dcp(&master) == -ENODEV && !probes[1]);

    /* Readiness rejects every unavailable provider state and releases lookup refs. */
    for (unsigned int state = 0; state < 4; state++) {
        initial(true);
        dcps[1].external_prepared = true;
        if (state == 0) devices[1].dev.data = NULL;
        if (state == 1) dcps[1].external_prepared = false;
        if (state == 2) dcps[1].quiescing = true;
        if (state == 3) dcps[1].crashed = true;
        assert(!dcp_h17p_external_ready(&nodes[1]));
        assert(references == 1 && released == 1);
        assert(apple_drm_init_dcp(&master) == -ENODEV && !probes[1]);
    }
    initial(true);
    dcps[1].external_prepared = true;
    external_probe_error = -EIO;
    assert(apple_drm_init_dcp(&master) == -EIO);
    assert(probes[1] == 1 && node_puts == 1);
    puts("PASS: automatic hardware/firmware matching, delayed/preflight-failed providers, synchronous publication, readiness rejection");
    return 0;
}
'''

    header = (root / 'drivers/gpu/drm/apple/dcp-external-bw.h').read_text().replace('#include <linux/types.h>', '')
    return prefix + header + production + suffix

def vrr(root):

    source = (root / 'drivers/gpu/drm/apple/dcp.c').read_text()
    start = source.index('int dcp_crtc_atomic_check(')
    opening = source.index('{', start)
    end, depth = opening + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    check = source[start:end]
    prefix = r"""
#undef NDEBUG
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <errno.h>
#define DCP_IOMFB_METHODS_H17P_EXTERNAL 2
#define READ_ONCE(x) (x)
#define dev_dbg(...) ((void)0)
struct apple_connector { bool connected; };
struct apple_dcp {
    bool crashed;
    struct { int iomfb_method_profile; } hw;
    struct { bool valid; } mode_state;
    struct apple_connector *connector;
    void *dev;
};
struct platform_device { struct apple_dcp *data; };
struct drm_crtc { struct platform_device *dcp; };
struct drm_crtc_state { bool vrr_enabled, modeset; };
struct drm_atomic_commit { struct drm_crtc_state crtc; };
#define to_apple_crtc(x) (x)
#define platform_get_drvdata(x) ((x)->data)
static struct drm_crtc_state *drm_atomic_get_new_crtc_state(
    struct drm_atomic_commit *s, struct drm_crtc *c)
{ (void)c; return &s->crtc; }
static bool drm_atomic_crtc_needs_modeset(struct drm_crtc_state *s)
{ return s->modeset; }
"""
    suffix = r"""
int main(void)
{
    struct apple_connector connector = { .connected = true };
    struct apple_dcp dcp = { .connector = &connector };
    struct platform_device pdev = { .data = &dcp };
    struct drm_crtc crtc = { .dcp = &pdev };
    struct drm_atomic_commit state = {0};
    unsigned int cases = 0;
    /* Check initial modesets, plane-only commits and link recovery states. */
    for (unsigned int profile = 0; profile < 3; profile++)
    for (unsigned int valid = 0; valid < 2; valid++)
    for (unsigned int modeset = 0; modeset < 2; modeset++)
    for (unsigned int link = 0; link < 3; link++)
    for (unsigned int vrr = 0; vrr < 2; vrr++) {
        dcp.hw.iomfb_method_profile = profile;
        dcp.mode_state.valid = valid;
        state.crtc.modeset = modeset;
        state.crtc.vrr_enabled = vrr;
        dcp.connector = link == 2 ? NULL : &connector;
        connector.connected = link == 1;
        int expected = profile == DCP_IOMFB_METHODS_H17P_EXTERNAL && vrr ? -EINVAL : 0;
        int actual = dcp_crtc_atomic_check(&crtc, &state);
        if (actual != expected) {
            fprintf(stderr, "profile=%u valid=%u modeset=%u link=%u vrr=%u: got %d expected %d\n",
                    profile, valid, modeset, link, vrr, actual, expected);
            return 1;
        }
        cases++;
    }
    dcp.crashed = true;
    state.crtc.vrr_enabled = false;
    assert(dcp_crtc_atomic_check(&crtc, &state) == -EINVAL);
    printf("PASS: %u production atomic VRR admission cases and crashed-device rejection\n", cases);
    return 0;
}
"""
    return prefix + check + suffix


def power_reply(root):
    """Actual external power callback validation with host completion/refcount mocks."""
    source = (root / 'drivers/gpu/drm/apple/iomfb_h17p.c').read_text()
    production = block(source, 'struct dcp_external_power_cookie {') + ';\n'
    production += block(source, 'static void iomfb_external_power_complete(')
    production += block(source, 'static bool iomfb_external_reply_size(')
    prefix = r"""
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
typedef uint32_t u32; typedef uint64_t u64;
#define WRITE_ONCE(p,v) ((p)=(v))
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
struct completion { bool done; };
struct kref { unsigned count; };
struct dcp_wait_cookie { struct kref refcount; struct completion done; u32 status; };
struct dcp_packet_header { char tag[4]; u32 in_len,out_len; };
struct dcp_set_power_state_req_h17p { u64 unklong; u32 unkint; unsigned char null,padding[3]; } __attribute__((packed));
struct dcp_set_power_state_resp { u32 unknown,ret; } __attribute__((packed));
struct apple_dcp { int unused; };
static unsigned released;
static u64 get_unaligned_le64(const void *p) { u64 v; memcpy(&v,p,sizeof(v)); return v; }
static void complete(struct completion *c) { assert(!c->done); c->done=true; }
static void release_wait_cookie(struct kref *r) { assert(!r->count); released++; }
static void kref_put(struct kref *r,void (*release)(struct kref *))
{ assert(r->count); if (!--r->count) release(r); }
"""
    controls = r"""
int main(void)
{
    struct apple_dcp d={};
    struct { struct dcp_packet_header h; struct dcp_set_power_state_req_h17p q; unsigned char reply[8]; } packet={};
    for(unsigned on=0;on<2;on++) {
        const u64 replies[]={0,1,0x123400000000ULL};
        for(unsigned i=0;i<3;i++) {
            packet.h.in_len=sizeof(packet.q);packet.h.out_len=sizeof(packet.reply);memcpy(packet.reply,&replies[i],sizeof(packet.reply));
            struct dcp_external_power_cookie c={.on=on,.wait.refcount.count=2};
            assert(iomfb_external_reply_size(&packet.reply,sizeof(packet.q),sizeof(packet.reply)));
            iomfb_external_power_complete(&d,&packet.reply,&c);
            assert(c.wait.done.done && c.wait.refcount.count==1);
            assert((int)c.wait.status==(replies[i]==on?0:-EREMOTEIO));
            kref_put(&c.wait.refcount,release_wait_cookie);
        }
    }
    for(unsigned fault=0;fault<3;fault++) {
        packet.h.in_len=sizeof(packet.q);packet.h.out_len=sizeof(packet.reply);
        void *out=&packet.reply;
        if(fault==0) out=NULL;
        if(fault==1) packet.h.in_len--;
        if(fault==2) packet.h.out_len--;
        struct dcp_external_power_cookie c={.wait.refcount.count=2};
        assert(!iomfb_external_reply_size(out,sizeof(packet.q),sizeof(packet.reply)));
        iomfb_external_power_complete(&d,out,&c);
        assert(c.wait.done.done && c.wait.refcount.count==1 && (int)c.wait.status==-EPROTO);
        kref_put(&c.wait.refcount,release_wait_cookie);
    }
    assert(released==9);
    puts("PASS: actual exact ON/OFF echoes, nonzero status and null/truncated reply rejection preserve callback completion and references");
}
"""
    return prefix + production + controls


def dispatcher(root):
    source = (root / "drivers/gpu/drm/apple/iomfb.c").read_text()
    start = source.index("static bool dcp_external_zero_reply(")
    end = source.index("static void dcpep_handle_ack(", start)
    prefix = r"""
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
#define DCP_FIRMWARE_H17P 1
#define DCP_IOMFB_METHODS_H17P_EXTERNAL 2
#define DCP_IOMFB_METHODS_H17G 3
#define DCP_MAX_CALL_DEPTH 8
#define IOMFB_MAX_CB 600
#define DCP_PACKET_ALIGNMENT 64
#define ALIGN(n, a) (((n) + (a) - 1) & ~((a) - 1))
#define dev_err(...) ((void)dev)
#define dev_info(...) ((void)dev)
#define dev_warn(...) ((void)dev)
enum dcp_context_id { DCP_CONTEXT_CB = 0, DCP_CONTEXT_ASYNC = 3 };
struct device_node { bool external; };
struct device { struct device_node *of_node; };
struct dcp_packet_header { char tag[4]; u32 in_len, out_len; };
struct dcp_channel { unsigned depth; void *output[8]; unsigned end[8]; };
struct apple_dcp {
    struct { int iomfb_method_profile; } hw;
    struct device *dev;
    int fw_compat;
    bool crashed;
    bool (**cb_handlers)(struct apple_dcp *, int, void *, void *);
    unsigned long unknown_callbacks[16];
    int start_done;
    struct dcp_channel channel;
};
static bool machine;
static unsigned acks, case_tag;
static struct dcp_channel *dcp_get_channel(struct apple_dcp *d, enum dcp_context_id c)
{ (void)c; return &d->channel; }
static int dcp_parse_tag(char *p) { (void)p; return case_tag; }
static bool of_machine_is_compatible(const char *n) { (void)n; return machine; }
static bool of_device_is_compatible(struct device_node *n, const char *s)
{ (void)s; return n->external; }
static bool iomfb_check_callback_h17p(struct apple_dcp *d, int t, u32 i, u32 o)
{ (void)d; (void)t; (void)i; (void)o; return true; }
static bool iomfb_uses_queue(struct apple_dcp *d) { (void)d; return false; }
static bool test_and_set_bit(int t, unsigned long *b) { (void)t; (void)b; return false; }
static void complete(int *p) { *p = 1; }
static u8 dcp_push_depth(unsigned *d) { return (*d)++; }
static void dcp_ack(struct apple_dcp *d, enum dcp_context_id c)
{ (void)c; acks++; d->channel.depth--; }
"""
    suffix = r"""
int main(void)
{
    struct device_node node;
    struct device dev = { &node };
    bool (*handlers[IOMFB_MAX_CB])(struct apple_dcp *, int, void *, void *) = {};
    unsigned cases = 0;
    for (unsigned scope = 0; scope < 8; scope++)
    for (unsigned tag = 133; tag <= 135; tag++)
    for (unsigned context = 0; context < 8; context++)
    for (unsigned in = 0; in <= 8; in++)
    for (unsigned out = 0; out <= 8; out++) {
        struct apple_dcp d = {
            .dev = &dev, .fw_compat = 1,
            .cb_handlers = handlers, .hw.iomfb_method_profile = 2,
        };
        union { uint64_t align; unsigned char data[64]; } packet;
        struct dcp_packet_header *h = (void *)packet.data;
        memset(&packet, 0xa5, sizeof(packet));
        h->in_len = in; h->out_len = out;
        machine = scope & 1; node.external = scope & 2;
        if (!(scope & 4))
            d.hw.iomfb_method_profile = 0;
        case_tag = tag; acks = 0;
        dcpep_handle_cb(&d, context, h, sizeof(*h) + in + out, 0);
        bool accepted = scope == 7 && tag == 134 && context == 3 && !in && out == 4;
        if (scope & 4) {
            assert(acks == accepted && d.crashed != accepted);
            for (unsigned i = 0; i < out; i++)
                assert(packet.data[sizeof(*h) + in + i] == (accepted ? 0 : 0xa5));
        } else {
            assert(acks == 1 && !d.crashed); /* Existing legacy behavior. */
        }
        assert(packet.data[sizeof(*h) + in + out] == 0xa5);
        cases++;
    }
    return cases != 15552;
}
"""

    return prefix + source[start:end] + suffix
