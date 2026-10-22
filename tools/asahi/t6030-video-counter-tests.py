#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Exercise the opt-in packet counter policy without touching hardware."""
import importlib.util
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("extract", root / "tools/asahi/dp-release-tests.py")
extract = importlib.util.module_from_spec(spec)
spec.loader.exec_module(extract)
source = (root / "drivers/thunderbolt/tunnel.c").read_text()
bodies = "\n".join(extract.function(source, name) for name in
                   ("tb_dp_video_counter_wanted", "tb_dp_init_video_path"))
fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <string.h>
#define TB_PATH_NONE 0
#define TB_DP_VIDEO_PRIORITY 1
#define TB_DP_VIDEO_WEIGHT 1
#define tb_port_dbg(...) do {} while (0)
struct tb_port { unsigned port; bool apple, m2_right; };
struct tb_path_hop { struct tb_port *in_port; int in_counter_index; };
struct tb_path { unsigned path_length; struct tb_path_hop hops[2];
 int egress_fc_enable, egress_shared_buffer, ingress_fc_enable;
 int ingress_shared_buffer, priority, weight; };
static bool dp_video_counter, m3;
static bool of_machine_is_compatible(const char *name) {
 return m3 && !strcmp(name, "apple,t6030");
}
static bool tb_port_is_apple_host_dpin(const struct tb_port *p) { return p->apple; }
static bool tb_dp_is_apple_t602x_right_dpin(const struct tb_port *p) { return p->m2_right; }
static int tb_dp_init_video_credits(struct tb_path_hop *hop) { return 0; }
static void tb_init_pm_support(struct tb_path_hop *hop) {}
#define tb_path_for_each_hop(path, hop) \
 for (hop = (path)->hops; hop < (path)->hops + (path)->path_length; hop++)
/* FUNCTIONS */
int main(void) {
 unsigned tested = 0;
 for (int enabled = 0; enabled < 2; enabled++)
 for (int soc = 0; soc < 2; soc++)
 for (int apple = 0; apple < 2; apple++)
 for (int m2 = 0; m2 < 2; m2++)
 for (unsigned port = 4; port <= 7; port++)
 for (unsigned length = 0; length <= 2; length++) {
  struct tb_port in = {port, apple, m2}, downstream = {1, false, false};
  struct tb_path path = {.path_length = length,
   .hops = {{&in, -1}, {&downstream, -1}}};
  dp_video_counter = enabled; m3 = soc;
  bool wanted = enabled && length && (m2 || (soc && apple && (port == 5 || port == 6)));
  assert(tb_dp_video_counter_wanted(&path) == wanted);
  assert(tb_dp_init_video_path(&path, true) == 0);
  int expected = wanted ? (soc && port == 6 ? 1 : 0) : -1;
  assert(path.hops[0].in_counter_index == expected);
  assert(path.hops[1].in_counter_index == (length == 2 ? expected : -1));
  tested++;
 }
 assert(tested == 192);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="t6030-video-counter-") as directory:
    directory = Path(directory)
    for name, body, expect in (
        ("real", bodies, 0),
        ("shared-counter-mutant", bodies.replace(
            "path->hops[0].in_port->port == 6 ? 1 : 0", "false ? 1 : 0"), -1),
    ):
        program = directory / (name + ".c")
        binary = directory / name
        program.write_text(fixture.replace("/* FUNCTIONS */", body))
        subprocess.run(["cc", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                        str(program), "-o", str(binary)], check=True)
        run = subprocess.run([str(binary)], cwd=directory, capture_output=True)
        assert (run.returncode == 0) == (expect == 0), (name, run.stderr)
        print(f"{name}: {'PASS' if expect == 0 else 'detected'}")
