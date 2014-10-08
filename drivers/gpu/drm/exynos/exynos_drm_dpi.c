/*
 * Exynos DRM Parallel output support.
 *
 * Copyright (c) 2014 Samsung Electronics Co., Ltd
 *
 * Contacts: Andrzej Hajda <a.hajda@samsung.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
*/

#include <drm/drmP.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_panel.h>
#include <drm/drm_edid.h>

#include <linux/regulator/consumer.h>
#include <linux/regmap.h>
#include <linux/mfd/syscon.h>
#include <linux/clk.h>

#include <video/of_videomode.h>
#include <video/videomode.h>

#include "exynos_drm_drv.h"

struct exynos_dpi {
	struct device *dev;
	struct i2c_adapter *ddc_adpt;
	struct clk *vclk;

	struct drm_panel *panel;
	struct drm_connector connector;
	struct drm_encoder *encoder;

	struct videomode *vm;
	int dpms_mode;
};

#define connector_to_dpi(c) container_of(c, struct exynos_dpi, connector)

static enum drm_connector_status
exynos_dpi_detect(struct drm_connector *connector, bool force)
{
	struct exynos_dpi *ctx = connector_to_dpi(connector);

	if (ctx->panel && !ctx->panel->connector)
		drm_panel_attach(ctx->panel, &ctx->connector);

	if (ctx->ddc_adpt && drm_probe_ddc(ctx->ddc_adpt))
		return connector_status_connected;

	return connector_status_disconnected;
}

static void exynos_dpi_connector_destroy(struct drm_connector *connector)
{
	drm_sysfs_connector_remove(connector);
	drm_connector_cleanup(connector);
}

static int exynos_drm_connector_mode_valid(struct drm_connector *connector,
					    struct drm_display_mode *mode)
{
	struct exynos_dpi *ctx = connector_to_dpi(connector);
	unsigned long ideal_clk = mode->clock * 1000;
	int vsync_len, vbpd, vfpd, hsync_len, hbpd, hfpd;

	/* For a display mode to be supported, the parameters must fit in the
	 * register widths of the FIMD hardware, and we must be able to produce
	 * an accurate pixel clock.
	 *
	 * Note that 1 is subtracted from many of these parameters before they
	 * are submitted to the hardware.
	 */

	if (mode->hdisplay > 2048 || mode->vdisplay > 2048) {
		pr_info("%dx%d VGA unsupported: resolution out of range\n",
			mode->hdisplay, mode->hdisplay);
		return MODE_BAD;
	}

	vsync_len = mode->vsync_end - mode->vsync_start;
	vbpd = mode->vtotal - mode->vsync_end;
	vfpd = mode->vsync_start - mode->vdisplay;
	hsync_len = mode->hsync_end - mode->hsync_start;
	hbpd = mode->htotal - mode->hsync_end;
	hfpd = mode->hsync_start - mode->hdisplay;

	if (vsync_len > 256 || vbpd > 256 || vfpd > 256) {
		pr_info("%dx%d VGA unsupported: V params out of range (%d,%d,%d)\n",
			mode->hdisplay, mode->vdisplay, vsync_len, vbpd, vfpd);
		return MODE_BAD;
	}

	if (hsync_len > 256 || hbpd > 256 || hfpd > 256) {
		pr_info("%dx%d VGA unsupported: H params out of range (%d,%d,%d)\n",
			mode->hdisplay, mode->vdisplay, hsync_len, hbpd, hfpd);
		return MODE_BAD;
	}

	if (clk_round_rate(ctx->vclk, ideal_clk) != ideal_clk) {
		pr_info("%dx%d VGA unsupported: Requires pixel clock %ld\n",
			mode->hdisplay, mode->vdisplay, ideal_clk);
		return MODE_BAD;
	}

	return MODE_OK;
}


static struct drm_connector_funcs exynos_dpi_connector_funcs = {
	.dpms = drm_helper_connector_dpms,
	.detect = exynos_dpi_detect,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = exynos_dpi_connector_destroy,
};

static int exynos_dpi_get_modes(struct drm_connector *connector)
{
	struct exynos_dpi *ctx = connector_to_dpi(connector);
	struct edid *edid;

	if (!ctx->ddc_adpt)
		return -ENODEV;

	edid = drm_get_edid(connector, ctx->ddc_adpt);
	if (!edid)
		return -ENODEV;

	pr_info("VGA monitor : width[%d] x height[%d]\n",
		edid->width_cm, edid->height_cm);

	drm_mode_connector_update_edid_property(connector, edid);

	return drm_add_edid_modes(connector, edid);
}

static struct drm_encoder *
exynos_dpi_best_encoder(struct drm_connector *connector)
{
	struct exynos_dpi *ctx = connector_to_dpi(connector);

	return ctx->encoder;
}

static struct drm_connector_helper_funcs exynos_dpi_connector_helper_funcs = {
	.get_modes = exynos_dpi_get_modes,
	.mode_valid	= exynos_drm_connector_mode_valid,
	.best_encoder = exynos_dpi_best_encoder,
};

static int exynos_dpi_create_connector(struct exynos_drm_display *display,
				       struct drm_encoder *encoder)
{
	struct exynos_dpi *ctx = display->ctx;
	struct drm_connector *connector = &ctx->connector;
	int ret;

	ctx->encoder = encoder;

	connector->polled = DRM_CONNECTOR_POLL_CONNECT | DRM_CONNECTOR_POLL_DISCONNECT;

	ret = drm_connector_init(encoder->dev, connector,
				 &exynos_dpi_connector_funcs,
				 DRM_MODE_CONNECTOR_VGA);
	if (ret) {
		DRM_ERROR("failed to initialize connector with drm\n");
		return ret;
	}

	connector->status = exynos_dpi_detect(connector, true);
	drm_connector_helper_add(connector, &exynos_dpi_connector_helper_funcs);
	drm_sysfs_connector_add(connector);
	drm_mode_connector_attach_encoder(connector, encoder);

	return 0;
}

static void exynos_dpi_poweron(struct exynos_dpi *ctx)
{
	if (ctx->panel)
		drm_panel_enable(ctx->panel);
}

static void exynos_dpi_poweroff(struct exynos_dpi *ctx)
{
	if (ctx->panel)
		drm_panel_disable(ctx->panel);
}

static void exynos_dpi_dpms(struct exynos_drm_display *display, int mode)
{
	struct exynos_dpi *ctx = display->ctx;

	switch (mode) {
	case DRM_MODE_DPMS_ON:
		if (ctx->dpms_mode != DRM_MODE_DPMS_ON)
				exynos_dpi_poweron(ctx);
			break;
	case DRM_MODE_DPMS_STANDBY:
	case DRM_MODE_DPMS_SUSPEND:
	case DRM_MODE_DPMS_OFF:
		if (ctx->dpms_mode == DRM_MODE_DPMS_ON)
			exynos_dpi_poweroff(ctx);
		break;
	default:
		break;
	}
	ctx->dpms_mode = mode;
}

static void exynos_dpi_mode_set(struct exynos_drm_display *display,
				struct drm_display_mode *mode)
{
	/* At 1280x1024@60Hz and higher there is not enough memory bandwidth
	 * available for the display controller when the GPU is busy. So we
	 * apply a "QoS" scheme.
	 * I found these numbers through guesswork. The GPU performance is
	 * degraded by about 30%, but there are no flickers.
	 */
	if (mode->clock >= 135000)
		exynos4412_qos(3, 3);
	else
		exynos4412_qos(0, 0);
}


static struct exynos_drm_display_ops exynos_dpi_display_ops = {
	.mode_set = exynos_dpi_mode_set,
	.create_connector = exynos_dpi_create_connector,
	.dpms = exynos_dpi_dpms
};

static struct exynos_drm_display exynos_dpi_display = {
	.type = EXYNOS_DISPLAY_TYPE_LCD,
	.ops = &exynos_dpi_display_ops,
};

/* of_* functions will be removed after merge of of_graph patches */
static struct device_node *
of_get_child_by_name_reg(struct device_node *parent, const char *name, u32 reg)
{
	struct device_node *np;

	for_each_child_of_node(parent, np) {
		u32 r;

		if (!np->name || of_node_cmp(np->name, name))
			continue;

		if (of_property_read_u32(np, "reg", &r) < 0)
			r = 0;

		if (reg == r)
			break;
	}

	return np;
}

static struct device_node *of_graph_get_port_by_reg(struct device_node *parent,
						    u32 reg)
{
	struct device_node *ports, *port;

	ports = of_get_child_by_name(parent, "ports");
	if (ports)
		parent = ports;

	port = of_get_child_by_name_reg(parent, "port", reg);

	of_node_put(ports);

	return port;
}

static struct device_node *
of_graph_get_endpoint_by_reg(struct device_node *port, u32 reg)
{
	return of_get_child_by_name_reg(port, "endpoint", reg);
}

static struct device_node *
of_graph_get_remote_port_parent(const struct device_node *node)
{
	struct device_node *np;
	unsigned int depth;

	np = of_parse_phandle(node, "remote-endpoint", 0);

	/* Walk 3 levels up only if there is 'ports' node. */
	for (depth = 3; depth && np; depth--) {
		np = of_get_next_parent(np);
		if (depth == 2 && of_node_cmp(np->name, "ports"))
			break;
	}
	return np;
}

enum {
	FIMD_PORT_IN0,
	FIMD_PORT_IN1,
	FIMD_PORT_IN2,
	FIMD_PORT_RGB,
	FIMD_PORT_WRB,
};

static struct device_node *exynos_dpi_of_find_panel_node(struct device *dev)
{
	struct device_node *np, *ep;

	np = of_graph_get_port_by_reg(dev->of_node, FIMD_PORT_RGB);
	if (!np)
		return NULL;

	ep = of_graph_get_endpoint_by_reg(np, 0);
	of_node_put(np);
	if (!ep)
		return NULL;

	np = of_graph_get_remote_port_parent(ep);
	of_node_put(ep);

	return np;
}

static int exynos_dpi_parse_dt(struct exynos_dpi *ctx)
{
	struct device *dev = ctx->dev;
	struct device_node *dn = dev->of_node;
	struct device_node *ddc_node;

	ddc_node = of_parse_phandle(dn, "ddc", 0);
	if (!ddc_node) {
		pr_err("Failed to find ddc\n");
		return -ENODEV;
	}

	ctx->ddc_adpt = of_find_i2c_adapter_by_node(ddc_node);
	if (!ctx->ddc_adpt) {
		DRM_ERROR("Failed to get ddc i2c adapter by node\n");
		return -EPROBE_DEFER;
	}

#if 0
	np = of_get_child_by_name(dn, "display-timings");
	if (np) {
		struct videomode *vm;
		int ret;

		of_node_put(np);

		vm = devm_kzalloc(dev, sizeof(*ctx->vm), GFP_KERNEL);
		if (!vm)
			return -ENOMEM;

		ret = of_get_videomode(dn, vm, 0);
		if (ret < 0) {
			devm_kfree(dev, vm);
			return ret;
		}

		ctx->vm = vm;

		return 0;
	}

	if (!ctx->panel_node)
		return -EINVAL;
#endif

	return 0;
}

struct exynos_drm_display *exynos_dpi_probe(struct device *dev)
{
	struct regmap *sysreg;
	struct exynos_dpi *ctx;
	int ret;

	ret = exynos_drm_component_add(dev,
					EXYNOS_DEVICE_TYPE_CONNECTOR,
					exynos_dpi_display.type);
	if (ret)
		return ERR_PTR(ret);

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		goto err_del_component;

	ctx->dev = dev;
	exynos_dpi_display.ctx = ctx;
	ctx->dpms_mode = DRM_MODE_DPMS_OFF;

	ctx->vclk = devm_clk_get(dev, "vclk");
	if (IS_ERR(ctx->vclk)) {
		dev_err(dev, "failed to get video clock\n");
		ret = PTR_ERR(ctx->vclk);
		goto err_del_component;
	}

	ret = exynos_dpi_parse_dt(ctx);
	if (ret < 0) {
		devm_kfree(dev, ctx);
		goto err_del_component;
	}

	sysreg = syscon_regmap_lookup_by_phandle(dev->of_node, "samsung,sysreg");
	if (IS_ERR(sysreg)) {
		dev_err(dev, "syscon regmap lookup failed.\n");
		goto err_del_component;
	}
	/* Set output to bypass image enhacement units and go to screen */
	regmap_write(sysreg, 0x210, 0x3);

#if 0
	if (ctx->panel_node) {
		ctx->panel = of_drm_find_panel(ctx->panel_node);
		if (!ctx->panel) {
			exynos_drm_component_del(dev,
						EXYNOS_DEVICE_TYPE_CONNECTOR);
			return ERR_PTR(-EPROBE_DEFER);
		}
	}
#endif

	return &exynos_dpi_display;

err_del_component:
	exynos_drm_component_del(dev, EXYNOS_DEVICE_TYPE_CONNECTOR);

	return NULL;
}

int exynos_dpi_remove(struct device *dev)
{
	struct drm_encoder *encoder = exynos_dpi_display.encoder;
	struct exynos_dpi *ctx = exynos_dpi_display.ctx;

	exynos_dpi_dpms(&exynos_dpi_display, DRM_MODE_DPMS_OFF);
	encoder->funcs->destroy(encoder);
	drm_connector_cleanup(&ctx->connector);

	exynos_drm_component_del(dev, EXYNOS_DEVICE_TYPE_CONNECTOR);

	return 0;
}
