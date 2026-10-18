// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright 2023 */

#include <linux/completion.h>

#include "afk.h"
#include "dcp.h"

static void disp_service_init(struct apple_epic_service *service, const char *name,
			const char *class, s64 unit)
{
}


static const struct apple_epic_service_ops ibootep_ops[] = {
	{
		.name = "disp0-service",
		.init = disp_service_init,
	},
	{}
};

static const struct apple_epic_service_ops external_ibootep_ops[] = {
	{
		.name = "dispext-service",
		.init = disp_service_init,
	},
	{}
};

int ibootep_init(struct apple_dcp *dcp)
{
	const struct apple_epic_service_ops *ops = ibootep_ops;

	if (dcp->hw.iomfb_method_profile == DCP_IOMFB_METHODS_H17P_EXTERNAL)
		ops = external_ibootep_ops;
	dcp->ibootep = afk_init(dcp, DISP0_ENDPOINT, ops);
	if (IS_ERR(dcp->ibootep))
		return PTR_ERR(dcp->ibootep);

	return afk_start(dcp->ibootep);
}
