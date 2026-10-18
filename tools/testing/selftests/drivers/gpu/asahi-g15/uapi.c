// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Catch binding the imported driver to the maintained, incompatible command ABI. */
#include <stddef.h>
#include <stdio.h>
#include "asahi-selector.h"

#ifdef CONFIG_SILICONFOREST_M3
_Static_assert(DRM_ASAHI_FEATURE_PER_COMMAND_INPUT_SYNCS == 8, "M3 feature bits");
_Static_assert(sizeof(struct drm_asahi_cmd_render) == 384, "M3 render size");
_Static_assert(offsetof(struct drm_asahi_cmd_render, prior_vdm) == 256, "M3 render prior");
_Static_assert(sizeof(struct drm_asahi_cmd_compute) == 136, "M3 compute size");
_Static_assert(offsetof(struct drm_asahi_cmd_compute, scs_layout) == 64, "M3 compute tail");
#else
_Static_assert(DRM_ASAHI_FEATURE_PER_COMMAND_INPUT_SYNCS == 1024, "maintained feature bits");
_Static_assert(sizeof(struct drm_asahi_cmd_render) == 448, "maintained render size");
_Static_assert(offsetof(struct drm_asahi_cmd_render, prior_vdm) == 320, "maintained render prior");
_Static_assert(sizeof(struct drm_asahi_cmd_compute) == 200, "maintained compute size");
_Static_assert(offsetof(struct drm_asahi_cmd_compute, scs_layout) == 128, "maintained compute tail");
#endif
_Static_assert(sizeof(struct drm_asahi_vm_create) == 24, "VM ioctl size");

int main(void)
{
#ifdef CONFIG_SILICONFOREST_M3
	puts("M3 UAPI selection and layout: PASS");
#else
	puts("maintained UAPI selection and layout: PASS");
#endif
	return 0;
}
