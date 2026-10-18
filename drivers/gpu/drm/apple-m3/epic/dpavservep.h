// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright The Asahi Linux Contributors */

#ifndef _DRM_APPLE_EPIC_DPAVSERV_H
#define _DRM_APPLE_EPIC_DPAVSERV_H

#include <linux/completion.h>
#include <linux/mutex.h>
#include <linux/types.h>

struct drm_edid;
struct apple_epic_service;
struct apple_dcp;

struct dcpavserv {
	/* Process context only: serializes retirement and EDID publication.
	 * Nest below mode_config.mutex and hpd_mutex; never wait for firmware.
	 */
	struct mutex lock;
	u64 epoch; /* Changes even when the same service slot is reused. */
	bool enabled;
	struct completion enable_completion;
	u32 unit;
	struct apple_epic_service *service;
};

bool dcpavserv_edid_current(struct apple_dcp *dcp, u64 generation);
bool dcpavserv_edid_service_current(struct apple_dcp *dcp, u64 epoch);
const struct drm_edid *dcpavserv_copy_edid(struct apple_dcp *dcp, u64 generation,
					u64 *epoch);
void dpavservep_detach(struct apple_dcp *dcp);

#endif /* _DRM_APPLE_EPIC_DPAVSERV_H */
