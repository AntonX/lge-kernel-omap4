/*
 * hdmi.c
 *
 * HDMI interface DSS driver setting for TI's OMAP4 family of processor.
 * Copyright (C) 2010-2011 Texas Instruments Incorporated - http://www.ti.com/
 * Authors: Yong Zhi
 *	Mythri pk <mythripk@ti.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#define DSS_SUBSYS_NAME "HDMI"

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/interrupt.h>
#include <linux/seq_file.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/string.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/pm_runtime.h>
#include <linux/clk.h>
#include <video/omapdss.h>
#include <video/hdmi_ti_4xxx_ip.h>
#include <linux/gpio.h>
#include <linux/fb.h>
#include <linux/omapfb.h>

#include "dss.h"
#include "dss_features.h"

#ifdef CONFIG_OMAP_PM
#include <linux/pm_qos_params.h>
static struct pm_qos_request_list pm_qos_handle;
#endif

// by Joshua
enum extension_edid_db {
	DATABLOCK_AUDIO	= 1,
	DATABLOCK_VIDEO	= 2,
	DATABLOCK_VENDOR = 3,
	DATABLOCK_SPEAKERS = 4,
	DATABLOCK_VESA_DTC = 5,
	DATABLOCK_RESERVED = 6,
	DATABLOCK_VCDB = 7,  // by Joshua
};

// by Joshua
#define HDMI_EDID_EX_DATABLOCK_TAG_MASK		0xE0
#define HDMI_EDID_EX_DATABLOCK_LEN_MASK		0x1F
#define HDMI_WP			0x0
#define HDMI_CORE_SYS		0x400
#define HDMI_CORE_AV		0x900
#define HDMI_PLLCTRL		0x200
#define HDMI_PHY		0x300

/* HDMI EDID Length move this */
#define HDMI_EDID_MAX_LENGTH			512
#define EDID_TIMING_DESCRIPTOR_SIZE		0x12
#define EDID_DESCRIPTOR_BLOCK0_ADDRESS		0x36
#define EDID_DESCRIPTOR_BLOCK1_ADDRESS		0x80
#define EDID_SIZE_BLOCK0_TIMING_DESCRIPTOR	4
#define EDID_SIZE_BLOCK1_TIMING_DESCRIPTOR	4

#define OMAP_HDMI_TIMINGS_NB			34
#define HDMI_DEFAULT_REGN 15
#define HDMI_DEFAULT_REGM2 1

static struct {
	struct mutex lock;
	struct omap_display_platform_data *pdata;
	struct platform_device *pdev;
	struct omap_dss_device *dssdev;
	struct hdmi_ip_data hdmi_data;
	int code;
	int mode;
	u8 edid[HDMI_EDID_MAX_LENGTH];
	u8 edid_set;
	bool can_do_hdmi;

	bool custom_set;
	enum hdmi_deep_color_mode deep_color;
	struct hdmi_config cfg;
	struct regulator *hdmi_reg;

	int hdmi_irq;
	struct clk *sys_clk;
	struct clk *hdmi_clk;

	int runtime_count;
	int enabled;
	bool set_mode;
	bool wp_reset_done;

	u8 s3d_mode;
	bool s3d_enable;
	u8 s3d_type;  //mo2sanghyun.lee 

	void (*hdmi_start_frame_cb)(void);
	void (*hdmi_irq_cb)(int);
	bool (*hdmi_power_on_cb)(void);
} hdmi;

static const u8 edid_header[8] = {0x0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x0};

#if defined(CONFIG_MACH_LGE_COSMO_3D_DISPLAY) || defined (CONFIG_MACH_LGE_CX2)
bool hdmi_s3d_supported(void);

static void hdmi_video_stop(struct omap_dss_device *dssdev)
{
//    int i, count=0;
    HDMIDBG("hdmi_video_stop\n");
    if(hdmi_ti_4xxx_wp_get_video_state(&hdmi.hdmi_data))
    {
        hdmi_ti_4xxx_wp_video_start(&hdmi.hdmi_data, 0);
        dispc_enable_channel(OMAP_DSS_CHANNEL_DIGIT, dssdev->type, 0);
    }
    else
    {
        HDMIDBG("HDMI Video is not on. skip stop hdmi\n");
    }

}

static void hdmi_video_start(struct omap_dss_device *dssdev)
{
    HDMIDBG("hdmi_video_start\n");
    if(!hdmi_ti_4xxx_wp_get_video_state(&hdmi.hdmi_data))
    {
        HDMIDBG("Checking HDMI Video start condition. HDMI go(%d)\n", dispc_go_busy(OMAP_DSS_CHANNEL_DIGIT));
        if ( dispc_go_busy(OMAP_DSS_CHANNEL_DIGIT) )
        {
            printk(KERN_ERR"unable to start vidoe\n");
            return ;
        }

    }
    dispc_enable_channel(OMAP_DSS_CHANNEL_DIGIT, dssdev->type, 1);
    hdmi_ti_4xxx_wp_video_start(&hdmi.hdmi_data, 1);
}

int hdmi_enable_s3d (struct omap_dss_device *dssdev, bool enable)
{
        struct hdmi_core_vendor_specific_infoframe config;
        HDMIDBG("hdmi_enable_s3d enable = %d, s3d_enable=%d,  s3d_mnode=%d\n", enable, hdmi.s3d_enable, hdmi.s3d_mode);

        if(hdmi.s3d_enable == enable)
            return 0;

	hdmi_ti_4xxx_wp_video_start(&hdmi.hdmi_data, 0);
        
        config.enable = hdmi.s3d_enable = enable;
        config.s3d_structure = hdmi.s3d_mode;
        
#if 1   //mno2sanghyyun.lee sidebyside test     
        if(hdmi.s3d_mode == NULL)
        {
            config.s3d_structure = HDMI_SIDE_BY_SIDE_HALF;
            HDMIDBG("Force setting to %s \n", (config.s3d_structure==HDMI_SIDE_BY_SIDE_HALF)?"HDMI_SIDE_BY_SIDE_HALF":"HDMI_TOPBOTTOM");
        }
#endif        

        if (config.s3d_structure == HDMI_SIDE_BY_SIDE_HALF)
            config.s3d_ext_data = S3D_DISP_SUB_SAMPLE_V; //S3D_DISP_SUB_SAMPLE_V
        else 
            config.s3d_ext_data = S3D_DISP_SUB_SAMPLE_H;  //S3D_DISP_SUB_SAMPLE_H
        
        hdmi_core_vsi_config(&hdmi.hdmi_data, &config);

        mdelay(300);
  	hdmi_video_start(dssdev);

        return 0;
}

int hdmi_get_s3d_enabled(struct omap_dss_device *dssdev)
{
        HDMIDBG("s3d_enable=%d\n", hdmi.s3d_enable);
        return hdmi.s3d_enable;
}

int hdmi_set_s3d_disp_type(struct omap_dss_device *dssdev, struct s3d_disp_info *info)
{
        HDMIDBG("type=%d, sub_samp=%d, order=%d\n", info->type, info->sub_samp, info->order);
        hdmi.s3d_type = info->type;
        
        if(info->type == 2/*S3D_TOP_BOTTOM*/)
            hdmi.s3d_mode = HDMI_TOPBOTTOM;
        else if(info->type == 1/*S3D_SIDE_BY_SIDE*/)
            hdmi.s3d_mode = HDMI_SIDE_BY_SIDE_HALF;
        else
        {
            HDMIDBG(KERN_ERR"ERROR   ======3d type missed=======\n");
            return -1;
        }
        return 0;
}
int hdmi_get_s3d_disp_type(struct omap_dss_device *dssdev, struct s3d_disp_info *info)
{
        HDMIDBG("type\n", info->type);
        return hdmi.s3d_type;
}

EXPORT_SYMBOL(hdmi_enable_s3d);
EXPORT_SYMBOL(hdmi_get_s3d_enabled);
EXPORT_SYMBOL(hdmi_set_s3d_disp_type);
EXPORT_SYMBOL(hdmi_get_s3d_disp_type);
#endif

static int hdmi_runtime_get(void)
{
	int r;

	DSSDBG("hdmi_runtime_get\n");

	if (hdmi.runtime_count++ == 0) {
		r = dss_runtime_get();
		if (r)
			goto err_get_dss;

		r = dispc_runtime_get();
		if (r)
			goto err_get_dispc;

		clk_enable(hdmi.sys_clk);
		clk_enable(hdmi.hdmi_clk);

		r = pm_runtime_get_sync(&hdmi.pdev->dev);
		WARN_ON(r);
		if (r < 0)
			goto err_runtime_get;
	}

	return 0;

err_runtime_get:
	clk_disable(hdmi.sys_clk);
	clk_disable(hdmi.hdmi_clk);
	dispc_runtime_put();
err_get_dispc:
	dss_runtime_put();
err_get_dss:
	return r;
}

static void hdmi_runtime_put(void)
{
	int r;

	DSSDBG("hdmi_runtime_put\n");

	if (--hdmi.runtime_count == 0) {
		r = pm_runtime_put_sync(&hdmi.pdev->dev);
		WARN_ON(r);

		clk_disable(hdmi.sys_clk);
		clk_disable(hdmi.hdmi_clk);

		dispc_runtime_put();
		dss_runtime_put();
	}
}

int hdmi_init_display(struct omap_dss_device *dssdev)
{
	DSSDBG("init_display\n");

	return 0;
}

static int relaxed_fb_mode_is_equal(const struct fb_videomode *mode1,
				    const struct fb_videomode *mode2)
{
	u32 ratio1 = mode1->flag & (FB_FLAG_RATIO_4_3 | FB_FLAG_RATIO_16_9);
	u32 ratio2 = mode2->flag & (FB_FLAG_RATIO_4_3 | FB_FLAG_RATIO_16_9);

	return (mode1->xres         == mode2->xres &&
		mode1->yres         == mode2->yres &&
		mode1->pixclock     <= mode2->pixclock * 201 / 200 &&
		mode1->pixclock     >= mode2->pixclock * 200 / 201 &&
		mode1->hsync_len + mode1->left_margin + mode1->right_margin ==
		mode2->hsync_len + mode2->left_margin + mode2->right_margin &&
		mode1->vsync_len + mode1->upper_margin + mode1->lower_margin ==
		mode2->vsync_len + mode2->upper_margin + mode2->lower_margin &&
		(!ratio1 || !ratio2 || ratio1 == ratio2) &&
		(mode1->vmode & FB_VMODE_INTERLACED) ==
		(mode2->vmode & FB_VMODE_INTERLACED));
}

static int hdmi_set_timings(struct fb_videomode *vm, bool check_only)
{
	int i = 0;
	HDMIDBG("hdmi_set_timings\n");
	DSSDBG("hdmi_set_timings\n");

	if (!vm->xres || !vm->yres || !vm->pixclock)
		goto fail;

	for (i = 0; i < CEA_MODEDB_SIZE; i++) {
		if (relaxed_fb_mode_is_equal(cea_modes + i, vm)) {
			*vm = cea_modes[i];
			if (check_only)
				return 1;
			hdmi.cfg.cm.code = i;
			hdmi.cfg.cm.mode = HDMI_HDMI;
			hdmi.cfg.timings = cea_modes[hdmi.cfg.cm.code];
			goto done;
		}
	}

	for (i = 0; i < VESA_MODEDB_SIZE; i++) {
		if (relaxed_fb_mode_is_equal(vesa_modes + i, vm)) {
			*vm = vesa_modes[i];
			if (check_only)
				return 1;
			hdmi.cfg.cm.code = i;
			hdmi.cfg.cm.mode = HDMI_DVI;
			hdmi.cfg.timings = vesa_modes[hdmi.cfg.cm.code];
			goto done;
		}
	}

fail:
	if (check_only)
		return 0;
	hdmi.cfg.cm.code = 1;
	hdmi.cfg.cm.mode = HDMI_HDMI;
	hdmi.cfg.timings = cea_modes[hdmi.cfg.cm.code];

	i = -1;
done:

	DSSDBG("%s-%d\n", hdmi.cfg.cm.mode ? "CEA" : "VESA", hdmi.cfg.cm.code);
	HDMIDBG("%s-%d\n", hdmi.cfg.cm.mode ? "CEA" : "VESA", hdmi.cfg.cm.code);
	return i >= 0;
}

void hdmi_get_monspecs(struct fb_monspecs *specs)
{
	int i, j;
	char *edid = (char *) hdmi.edid;

	memset(specs, 0x0, sizeof(*specs));
	if (!hdmi.edid_set)
		return;

	fb_edid_to_monspecs(edid, specs);
	if (specs->modedb == NULL)
		return;

	for (i = 1; i <= edid[0x7e] && i * 128 < HDMI_EDID_MAX_LENGTH; i++) {
		if (edid[i * 128] == 0x2)
			fb_edid_add_monspecs(edid + i * 128, specs);
	}

	hdmi.can_do_hdmi = specs->misc & FB_MISC_HDMI;

	/* filter out resolutions we don't support */
	for (i = j = 0; i < specs->modedb_len; i++) {
		u32 max_pclk = hdmi.dssdev->clocks.hdmi.max_pixclk_khz;
		if (!hdmi_set_timings(&specs->modedb[i], true))
			continue;

		if (max_pclk &&
		    max_pclk < PICOS2KHZ(specs->modedb[i].pixclock))
			continue;

		if (specs->modedb[i].flag & FB_FLAG_PIXEL_REPEAT)
			continue;

		specs->modedb[j++] = specs->modedb[i];
	}
	specs->modedb_len = j;
}

u8 *hdmi_read_edid(struct omap_video_timings *dp)
{
	int ret = 0, i;

	if (hdmi.edid_set)
		return hdmi.edid;

	memset(hdmi.edid, 0, HDMI_EDID_MAX_LENGTH);

	ret = read_ti_4xxx_edid(&hdmi.hdmi_data, hdmi.edid,
						HDMI_EDID_MAX_LENGTH);

	for (i = 0; i < HDMI_EDID_MAX_LENGTH; i += 16)
		pr_info("edid[%03x] = %02x %02x %02x %02x %02x %02x %02x %02x "
			 "%02x %02x %02x %02x %02x %02x %02x %02x\n", i,
			hdmi.edid[i], hdmi.edid[i + 1], hdmi.edid[i + 2],
			hdmi.edid[i + 3], hdmi.edid[i + 4], hdmi.edid[i + 5],
			hdmi.edid[i + 6], hdmi.edid[i + 7], hdmi.edid[i + 8],
			hdmi.edid[i + 9], hdmi.edid[i + 10], hdmi.edid[i + 11],
			hdmi.edid[i + 12], hdmi.edid[i + 13], hdmi.edid[i + 14],
			hdmi.edid[i + 15]);

	if (ret) {
		DSSWARN("failed to read E-EDID\n");
		return NULL;
	}

	if (memcmp(hdmi.edid, edid_header, sizeof(edid_header)))
		return NULL;

	hdmi.edid_set = true;
	return hdmi.edid;
}




// by Joshua
int hdmi_get_datablock_offset(u8 *edid, enum extension_edid_db datablock, int *offset)
{
	int current_byte, disp, i = 0, length = 0;

	if (edid[0x7e] == 0x00)
		return 1;

	disp = edid[(0x80) + 2];
	printk(KERN_DEBUG "Extension block present db %d %x\n", datablock, disp);
	if (disp == 0x4)
		return 1;

	i = 0x80 + 0x4;
	printk(KERN_DEBUG "%x\n", i);
	while (i < (0x80 + disp)) {
		current_byte = edid[i];
		printk(KERN_DEBUG "i = %x cur_byte = %x (cur_byte & EX_DATABLOCK_TAG_MASK) = %d\n",
			i, current_byte, ( (current_byte & HDMI_EDID_EX_DATABLOCK_TAG_MASK) >> 5 ));  // by Joshua

		if ((current_byte >> 5)	== datablock) {
			*offset = i;
			printk(KERN_DEBUG "datablock %d %d\n", datablock, *offset);
			return 0;
		} else {
			length = (current_byte & HDMI_EDID_EX_DATABLOCK_LEN_MASK) + 1;
			i += length;
		}
	}
	return 1;
}

#if defined(CONFIG_MACH_LGE_COSMO_3D_DISPLAY) || defined (CONFIG_MACH_LGE_CX2)
bool hdmi_s3d_supported(void)
{
	bool s3d_support = false;
	int offset, current_byte;
	if (!hdmi_get_datablock_offset(hdmi.edid, DATABLOCK_VENDOR, &offset)) {
		offset += 8;
		current_byte = hdmi.edid[offset++];
		/*Latency_Fields_Present?*/
		if (current_byte & 0x80)
			offset += 2;
		/*I_Latency_Fields_Present?*/
		if (current_byte & 0x40)
			offset += 2;
		/*HDMI_Video_present?*/
		if (current_byte & 0x20) {
			current_byte = hdmi.edid[offset];
			/*3D_Present?*/
			if (current_byte & 0x80) {
				printk(KERN_INFO "S3D supported\n");
				s3d_support = true;
			}
		}
	}
	return s3d_support;
}
#endif

// by Joshua
char hdmi_get_extended_vcdb(u8 *edid)
{
	char tag;
	char extended_tag;
	char current_byte;
	int length, offset;
	enum extension_edid_db vcdb =  DATABLOCK_VCDB;
	char QS_VCDB;


	printk(KERN_INFO "check VCDB..\n");

	if (!hdmi_get_datablock_offset(edid, vcdb, &offset)) {

EXTENDED_TAG_Lable:

		current_byte = edid[offset];
		tag = (current_byte & HDMI_EDID_EX_DATABLOCK_TAG_MASK) >> 5;
		length = current_byte & HDMI_EDID_EX_DATABLOCK_LEN_MASK;
		extended_tag = edid[offset+1];

		if(tag != DATABLOCK_VCDB ){
			printk(KERN_INFO "[1]No DATABLOCK_VCDB...\n");
			return -1;
		}

		if(extended_tag == 0){
			if( edid[offset+2] & 0x40)  // 0x40 : bit 6
				QS_VCDB = 1;
			else
				QS_VCDB = 0;

			printk(KERN_INFO "QS_VCDB = %d...\n",QS_VCDB);
		}
		else{
			offset += (length + 1);
			goto EXTENDED_TAG_Lable;
		}

		return QS_VCDB;
	}
	else
		printk(KERN_INFO "[2]No DATABLOCK_VCDB...\n");


	return -1;
}


// by Joshua
void hdmi_avi_cfg_lr_fr(void)
{
	char qs_vcdb;

	qs_vcdb = hdmi_get_extended_vcdb((u8 *)hdmi.edid);
	hdmi_ti_4xxx_avi_dbq3(&hdmi.hdmi_data, &hdmi.cfg, qs_vcdb);
}



static void hdmi_compute_pll(struct omap_dss_device *dssdev, int phy,
		struct hdmi_pll_info *pi)
{
	unsigned long clkin, refclk;
	u32 mf;

	clkin = clk_get_rate(hdmi.sys_clk) / 10000;
	/*
	 * Input clock is predivided by N + 1
	 * out put of which is reference clk
	 */
	if (dssdev->clocks.hdmi.regn == 0)
		pi->regn = HDMI_DEFAULT_REGN;
	else
		pi->regn = dssdev->clocks.hdmi.regn;

	refclk = clkin / (pi->regn + 1);

	/*
	 * multiplier is pixel_clk/ref_clk
	 * Multiplying by 100 to avoid fractional part removal
	 */
	pi->regm = (phy * 100 / (refclk)) / 100;

	if (dssdev->clocks.hdmi.regm2 == 0)
		pi->regm2 = HDMI_DEFAULT_REGM2;
	else
		pi->regm2 = dssdev->clocks.hdmi.regm2;

	/*
	 * fractional multiplier is remainder of the difference between
	 * multiplier and actual phy(required pixel clock thus should be
	 * multiplied by 2^18(262144) divided by the reference clock
	 */
	mf = (phy - pi->regm * refclk) * 262144;
	pi->regmf = mf / (refclk);

	/*
	 * Dcofreq should be set to 1 if required pixel clock
	 * is greater than 1000MHz
	 */
	pi->dcofreq = phy > 1000 * 100;
	pi->regsd = ((pi->regm * clkin / 10) / ((pi->regn + 1) * 250) + 5) / 10;

	DSSDBG("M = %d Mf = %d\n", pi->regm, pi->regmf);
	DSSDBG("range = %d sd = %d\n", pi->dcofreq, pi->regsd);
}

// LGE_CHANGE_S [sungho.jung@lge.com] 2012-03-03. // Add GB code
struct omap_dss_device *get_hdmi_device(void)
{
	int match(struct omap_dss_device *dssdev, void *arg) {
		return sysfs_streq(dssdev->name , "hdmi");
	}

	return omap_dss_find_device(NULL, match);
}
EXPORT_SYMBOL(get_hdmi_device);

#ifdef CONFIG_OMAP4_HDCP
void hdcp_send_uevent(u8 on)
{
	int ret = 0;

	struct omap_dss_device *dssdev = get_hdmi_device();
	if (dssdev == NULL) {
		printk(KERN_INFO "DSS device(HDMI)is NULL..can't send HDCP events\n");
		return;
	}

	switch (on) {
		case 0: ret = kobject_uevent(&dssdev->dev.kobj, KOBJ_HDCP_OFF);
			break;
		case 1: ret = kobject_uevent(&dssdev->dev.kobj, KOBJ_HDCP_ON);
			break;
		default:
			printk(KERN_INFO "error!! currently scenario is nothing!!! (HDCP) on = %d\n", on);
	}

	if (ret)
		printk(KERN_INFO "hdcp_send_uevent uevent(%d) (%d)\n", on, ret);
	return;
}
EXPORT_SYMBOL(hdcp_send_uevent);
// LGE_CHANGE_E [sungho.jung@lge.com] 2012-03-03.

static void hdmi_load_hdcp_keys(struct omap_dss_device *dssdev)
{
	int aksv;
	DSSDBG("hdmi_load_hdcp_keys\n");
	/* load the keys and reset the wrapper to populate the AKSV registers*/
	if (hdmi.hdmi_power_on_cb) {
		aksv = hdmi_ti_4xx_check_aksv_data(&hdmi.hdmi_data);
		if ((aksv == HDMI_AKSV_ZERO) &&
		    hdmi.custom_set &&
		    hdmi.hdmi_power_on_cb()) {
			hdmi_ti_4xxx_set_wait_soft_reset(&hdmi.hdmi_data);
			/* HDCP keys are available in the AKSV registers 2ms after
			 * the RESET# rising edge, hence the delay before reading
			 * the registers*/
			mdelay(10);
			aksv = hdmi_ti_4xx_check_aksv_data(&hdmi.hdmi_data);
			hdmi.wp_reset_done = (aksv == HDMI_AKSV_VALID) ?
				true : false;
			DSSINFO("HDMI_WRAPPER RESET DONE\n");
		} else if (aksv == HDMI_AKSV_VALID)
			hdmi.wp_reset_done = true;
		else if (aksv == HDMI_AKSV_ERROR)
			hdmi.wp_reset_done = false;

		if (!hdmi.wp_reset_done)
			DSSERR("*** INVALID AKSV: %d "
				"Do not perform HDCP AUTHENTICATION\n", aksv);
	}

}
#endif

/* Set / Release c-state constraints */
static void hdmi_set_l3_cstr(struct omap_dss_device *dssdev, bool enable)
{
#ifdef CONFIG_OMAP_PM
	DSSINFO("%s c-state constraint for HDMI\n\n",
		enable ? "Set" : "Release");

	if (enable)
		pm_qos_add_request(&pm_qos_handle, PM_QOS_CPU_DMA_LATENCY, 100);
	else
		pm_qos_remove_request(&pm_qos_handle);
#else
	DSSINFO("C-STATE Constraints require COMFIG_OMAP_PM to be set\n");
#endif
}

static int hdmi_power_on(struct omap_dss_device *dssdev)
{
	int r;
	struct hdmi_pll_info pll_data;
	struct omap_video_timings *p;
	unsigned long phy;

	HDMIDBG("Enter\n");
	r = hdmi_runtime_get();
	if (r)
		return r;

	hdmi_set_l3_cstr(dssdev, true);

	/* Load the HDCP keys if not already loaded*/
#ifdef CONFIG_OMAP4_HDCP
	hdmi_load_hdcp_keys(dssdev);
#endif
	hdmi_ti_4xxx_wp_video_start(&hdmi.hdmi_data, 0);

	dispc_enable_channel(OMAP_DSS_CHANNEL_DIGIT, dssdev->type, 0);

	p = &dssdev->panel.timings;

	DSSDBG("hdmi_power_on x_res= %d y_res = %d\n",
		dssdev->panel.timings.x_res,
		dssdev->panel.timings.y_res);
	HDMIDBG("hdmi_power_on x_res= %d y_res = %d mode = %d\n",
		dssdev->panel.timings.x_res,
		dssdev->panel.timings.y_res, hdmi.mode);

	if (!hdmi.custom_set) {
        // wooho47.jung@lge.com 2012.04.19
        // MOD : for default mode. p2 is not dvi, is hdmi.
        #if 1
	    struct fb_videomode vga = cea_modes[4];
	    hdmi_set_timings(&vga, false);
        #else
	    struct fb_videomode vesa_vga = vesa_modes[4];
	    hdmi_set_timings(&vesa_vga, false);
        #endif
	}

	omapfb_fb2dss_timings(&hdmi.cfg.timings, &dssdev->panel.timings);

	phy = p->pixel_clock;

	switch (hdmi.deep_color) {
	case HDMI_DEEP_COLOR_30BIT:
		phy = (p->pixel_clock * 125) / 100 ;
		hdmi.cfg.deep_color = HDMI_DEEP_COLOR_30BIT;
		break;
	case HDMI_DEEP_COLOR_36BIT:
		if (p->pixel_clock == 148500) {
			printk(KERN_ERR "36 bit deep color not supported");
			goto err;
		}

		phy = (p->pixel_clock * 150) / 100;
		hdmi.cfg.deep_color = HDMI_DEEP_COLOR_36BIT;
		break;
	case HDMI_DEEP_COLOR_24BIT:
	default:
		phy = p->pixel_clock;
		hdmi.cfg.deep_color = HDMI_DEEP_COLOR_24BIT;
		break;
	}

	hdmi_compute_pll(dssdev, phy, &pll_data);

	/* config the PLL and PHY hdmi_set_pll_pwrfirst */
	r = hdmi_ti_4xxx_pll_program(&hdmi.hdmi_data, &pll_data);
	if (r) {
		DSSDBG("Failed to lock PLL\n");
		HDMIDBG("Failed to lock PLL\n");
		goto err;
	}

	r = hdmi_ti_4xxx_phy_init(&hdmi.hdmi_data, phy);
	if (r) {
		DSSDBG("Failed to start PHY\n");
		HDMIDBG("Failed to start PHY\n");
		goto err;
	}

	hdmi.cfg.cm.mode = hdmi.can_do_hdmi ? hdmi.mode : HDMI_DVI;
	hdmi.cfg.cm.code = hdmi.code;
	hdmi_ti_4xxx_basic_configure(&hdmi.hdmi_data, &hdmi.cfg);
	if (hdmi.s3d_enable) {
		struct hdmi_core_vendor_specific_infoframe config;
		config.enable = hdmi.s3d_enable;
		config.s3d_structure = hdmi.s3d_mode;
		if (config.s3d_structure == 8)
			config.s3d_ext_data = 1;
		hdmi_core_vsi_config(&hdmi.hdmi_data, &config);
	}
	/* Make selection of HDMI in DSS */
	dss_select_hdmi_venc_clk_source(DSS_HDMI_M_PCLK);

	/*
	 * Select the DISPC clock source as PRCM clock in case when both LCD
	 * panels are disabled and we cannot use DSI PLL for this purpose.
	 */
	if (!dispc_is_channel_enabled(OMAP_DSS_CHANNEL_LCD) &&
	    !dispc_is_channel_enabled(OMAP_DSS_CHANNEL_LCD2))
		dss_select_dispc_clk_source(dssdev->clocks.dispc.dispc_fclk_src);


	/* bypass TV gamma table */
// LGE_CHANGE_S [sungho.jung@lge.com] 2011-10-28, [SU540, LU5400]
#if defined(CONFIG_P2_GAMMA) || defined(CONFIG_U2_GAMMA) || defined(CONFIG_COSMO_GAMMA) || defined(CONFIG_CX2_GAMMA)
        dispc_enable_gamma_table(1);
#else
        dispc_enable_gamma_table(0);
#endif
// LGE_CHANGE_E [sungho.jung@lge.com] 2011-10-28, [SU540, LU5400]

	/* tv size */
	dispc_set_digit_size(dssdev->panel.timings.x_res,
			dssdev->panel.timings.y_res);

	dispc_enable_channel(OMAP_DSS_CHANNEL_DIGIT, dssdev->type, 1);

	hdmi_ti_4xxx_wp_video_start(&hdmi.hdmi_data, 1);

#ifdef CONFIG_OMAP4_HDCP
	if (hdmi.hdmi_start_frame_cb &&
	    hdmi.custom_set &&
	    hdmi.wp_reset_done)
		(*hdmi.hdmi_start_frame_cb)();
#endif
	return 0;
err:
	hdmi_set_l3_cstr(dssdev, false);
	hdmi_runtime_put();
	return -EIO;
}

static void hdmi_power_off(struct omap_dss_device *dssdev)
{
	HDMIDBG("Enter\n");
	enum hdmi_pwrchg_reasons reason = HDMI_PWRCHG_DEFAULT;
	if (hdmi.hdmi_irq_cb)
		hdmi.hdmi_irq_cb(HDMI_HPD_LOW);

	hdmi_ti_4xxx_wp_video_start(&hdmi.hdmi_data, 0);

	dispc_enable_channel(OMAP_DSS_CHANNEL_DIGIT, dssdev->type, 0);
	if (hdmi.set_mode)
		reason = reason | HDMI_PWRCHG_MODE_CHANGE;
	if (dssdev->sync_lost_error)
		reason = reason | HDMI_PWRCHG_RESYNC;
	hdmi_ti_4xxx_phy_off(&hdmi.hdmi_data, reason);
	hdmi_ti_4xxx_set_pll_pwr(&hdmi.hdmi_data, HDMI_PLLPWRCMD_ALLOFF);
	hdmi_set_l3_cstr(dssdev, false);
	hdmi_runtime_put();
	hdmi.deep_color = HDMI_DEEP_COLOR_24BIT;
}

int omapdss_hdmi_get_pixel_clock(void)
{
	return PICOS2KHZ(hdmi.cfg.timings.pixclock);
}

int omapdss_hdmi_get_mode(void)
{
	return hdmi.mode;
}

#ifdef CONFIG_OMAP4_HDCP
int omapdss_hdmi_register_hdcp_callbacks(void (*hdmi_start_frame_cb)(void),
					 void (*hdmi_irq_cb)(int status),
					 bool (*hdmi_power_on_cb)(void))
{
	hdmi.hdmi_start_frame_cb = hdmi_start_frame_cb;
	hdmi.hdmi_irq_cb = hdmi_irq_cb;
	hdmi.hdmi_power_on_cb = hdmi_power_on_cb;

	return hdmi_ti_4xxx_wp_get_video_state(&hdmi.hdmi_data);
}
EXPORT_SYMBOL(omapdss_hdmi_register_hdcp_callbacks);
#endif

void omapdss_hdmi_set_deepcolor(int val)
{
	hdmi.deep_color = val;
}

int omapdss_hdmi_get_deepcolor(void)
{
	return hdmi.deep_color;
}

ssize_t omapdss_hdmi_get_edid(char *edid_buffer)
{
	ssize_t size = hdmi.enabled ? HDMI_EDID_MAX_LENGTH : 0;
	memcpy(edid_buffer, hdmi.edid, size);
	return size;
}

void omapdss_hdmi_set_s3d_mode(int val)
{
	hdmi.s3d_mode = val;
}

int omapdss_hdmi_get_s3d_mode(void)
{
	return hdmi.s3d_mode;
}

void omapdss_hdmi_enable_s3d(bool enable)
{
	hdmi.s3d_enable = enable;
	if (hdmi.enabled)
		omapdss_hdmi_display_set_timing(hdmi.dssdev);
}

int omapdss_hdmi_get_s3d_enable(void)
{
	return hdmi.s3d_enable;
}

int hdmi_get_current_hpd()
{
	return gpio_get_value(hdmi.dssdev->hpd_gpio);
}

static irqreturn_t hpd_irq_handler(int irq, void *ptr)
{
#if 1
	int hpd = hdmi_get_current_hpd();
	pr_info("HDMI : hpd %d\n", hpd);
	hdmi_panel_hpd_handler(hpd);
#else
	hdmi_panel_hpd_handler(hdmi_get_current_hpd());
#endif

	return IRQ_HANDLED;
}

static irqreturn_t hdmi_irq_handler(int irq, void *arg)
{
	int r = 0;

	r = hdmi_ti_4xxx_irq_handler(&hdmi.hdmi_data);

	DSSDBG("Received HDMI IRQ = %08x\n", r);

	if (hdmi.hdmi_irq_cb)
		hdmi.hdmi_irq_cb(r);

	return IRQ_HANDLED;
}

int omapdss_hdmi_display_check_timing(struct omap_dss_device *dssdev,
					struct omap_video_timings *timings)
{
	struct fb_videomode t;

	omapfb_dss2fb_timings(timings, &t);

	/* also check interlaced timings */
	if (!hdmi_set_timings(&t, true)) {
		t.yres *= 2;
		t.vmode |= FB_VMODE_INTERLACED;
	}
	if (!hdmi_set_timings(&t, true))
		return -EINVAL;
	return 0;
}

int omapdss_hdmi_display_set_mode(struct omap_dss_device *dssdev,
				  struct fb_videomode *vm)
{
	int r1, r2;
	DSSINFO("Enter omapdss_hdmi_display_set_mode\n");
	/* turn the hdmi off and on to get new timings to use */
	hdmi.set_mode = true;
	dssdev->driver->disable(dssdev);
	hdmi.set_mode = false;
	r1 = hdmi_set_timings(vm, false) ? 0 : -EINVAL;
	hdmi.custom_set = 1;
	hdmi.code = hdmi.cfg.cm.code;
	hdmi.mode = hdmi.cfg.cm.mode;
	r2 = dssdev->driver->enable(dssdev);
	return r1 ? : r2;
}

void omapdss_hdmi_display_set_timing(struct omap_dss_device *dssdev)
{
	struct fb_videomode t;

	omapfb_dss2fb_timings(&dssdev->panel.timings, &t);
	/* also check interlaced timings */
	if (!hdmi_set_timings(&t, true)) {
		t.yres *= 2;
		t.vmode |= FB_VMODE_INTERLACED;
	}

	omapdss_hdmi_display_set_mode(dssdev, &t);
}

int omapdss_hdmi_display_enable(struct omap_dss_device *dssdev)
{
	int r = 0;

	HDMIDBG("ENTER hdmi_display_enable  hdmi.enabled=%d\n", hdmi.enabled);
	DSSINFO("ENTER hdmi_display_enable\n");

	mutex_lock(&hdmi.lock);

	if (hdmi.enabled)
		goto err0;

	r = omap_dss_start_device(dssdev);
	if (r) {
		DSSERR("failed to start device\n");
		HDMIDBG("failed to start device\n");
		goto err0;
	}

	if (dssdev->platform_enable) {
		r = dssdev->platform_enable(dssdev);
		if (r) {
			DSSERR("failed to enable GPIO's\n");
			HDMIDBG("failed to enable GPIO's\n");
			goto err1;
		}
	}

#if 0  //mo2sanghyun.lee 2012.06.07 not used
	hdmi.hdmi_reg = regulator_get(NULL, "hdmi_vref");
	if (IS_ERR_OR_NULL(hdmi.hdmi_reg)) {
		DSSERR("Failed to get hdmi_vref regulator\n");
		HDMIDBG("Failed to get hdmi_vref regulator\n");
		r = PTR_ERR(hdmi.hdmi_reg) ? : -ENODEV;
		goto err2;
	}

	r = regulator_enable(hdmi.hdmi_reg);
	if (r) {
		DSSERR("failed to enable hdmi_vref regulator\n");
		HDMIDBG("failed to enable hdmi_vref regulator\n");
		goto err3;
	}
#endif

	r = hdmi_power_on(dssdev);
	if (r) {
		DSSERR("failed to power on device\n");
		HDMIDBG("failed to power on device r:%d\n", r);
		goto err4;
	}

	hdmi.enabled = true;

	mutex_unlock(&hdmi.lock);
	return 0;

err4:
//	regulator_disable(hdmi.hdmi_reg);  //mo2sanghyun.lee 2012.06.07 not used
//err3:
//	regulator_put(hdmi.hdmi_reg);  //mo2sanghyun.lee 2012.06.07 not used
//err2:
	if (dssdev->platform_disable)
		dssdev->platform_disable(dssdev);
err1:
	omap_dss_stop_device(dssdev);
err0:
	mutex_unlock(&hdmi.lock);
	HDMIDBG("ENTER error:%d\n", r);
	return r;
}

void omapdss_hdmi_display_disable(struct omap_dss_device *dssdev)
{
	DSSINFO("Enter hdmi_display_disable\n");
	HDMIDBG("Enter hdmi_display_disable\n");

	mutex_lock(&hdmi.lock);

	if (!hdmi.enabled)
		goto done;

	hdmi.enabled = false;
	hdmi.wp_reset_done = false;

	hdmi_power_off(dssdev);
	if (dssdev->sync_lost_error == 0)
		if (dssdev->state != OMAP_DSS_DISPLAY_SUSPENDED) {
			/* clear EDID and mode on disable only */
			hdmi.edid_set = false;
			hdmi.custom_set = 0;
			pr_info("hdmi: clearing EDID info\n");
		}
#if 0  //mo2sanghyun.lee 2012.06.07 not used
	regulator_disable(hdmi.hdmi_reg);

	regulator_put(hdmi.hdmi_reg);
#endif

	if (dssdev->platform_disable)
		dssdev->platform_disable(dssdev);

	omap_dss_stop_device(dssdev);
done:
	mutex_unlock(&hdmi.lock);
}

static int hdmi_get_clocks(struct platform_device *pdev)
{
	struct clk *clk;

	clk = clk_get(&pdev->dev, "sys_clk");
	if (IS_ERR(clk)) {
		DSSERR("can't get sys_clk\n");
		return PTR_ERR(clk);
	}

	hdmi.sys_clk = clk;

	clk = clk_get(&pdev->dev, "hdmi_clk");
	if (IS_ERR(clk)) {
		DSSERR("can't get hdmi_clk\n");
		clk_put(hdmi.sys_clk);
		return PTR_ERR(clk);
	}

	hdmi.hdmi_clk = clk;

	return 0;
}

static void hdmi_put_clocks(void)
{
	if (hdmi.sys_clk)
		clk_put(hdmi.sys_clk);
	if (hdmi.hdmi_clk)
		clk_put(hdmi.hdmi_clk);
}

/* HDMI HW IP initialisation */
static int omapdss_hdmihw_probe(struct platform_device *pdev)
{
	struct resource *hdmi_mem;
	struct omap_dss_board_info *board_data;
	int r;
	unsigned int volatile val;

	hdmi.pdata = pdev->dev.platform_data;
	hdmi.pdev = pdev;

	mutex_init(&hdmi.lock);

#ifdef CONFIG_MHL_TX_SII9244_LEGACY  //mo2sanghyun.lee no need in cosmo
// LGE_CHANGE_S [sungho.jung@lge.com] 2012-03-28, Disable pull-up on DDC_SCL/SDA. Because the P2 uses Level-shift.
	if (omap_rev() >= CHIP_IS_OMAP4430ES2_3) 
	{
		val = omap_readl(0x4A100624);
		val = val & 0xEEFFFFFF;
		val = val | 0x11000000;
		omap_writel(val, 0x4A100624);
		val = omap_readl(0x4A100624);
		printk(KERN_INFO " Disable pulls on DDC_SCL/SDA lines%x \n", val);
	}
// LGE_CHANGE_E [sungho.jung@lge.com] 2012-03-28
#endif

	/* save reference to HDMI device */
	board_data = hdmi.pdata->board_data;
	for (r = 0; r < board_data->num_devices; r++) {
		if (board_data->devices[r]->type == OMAP_DISPLAY_TYPE_HDMI)
			hdmi.dssdev = board_data->devices[r];
	}
	if (!hdmi.dssdev) {
		DSSERR("can't get HDMI device\n");
		return -EINVAL;
	}

	hdmi_mem = platform_get_resource(hdmi.pdev, IORESOURCE_MEM, 0);
	if (!hdmi_mem) {
		DSSERR("can't get IORESOURCE_MEM HDMI\n");
		return -EINVAL;
	}

	/* Base address taken from platform */
	hdmi.hdmi_data.base_wp = ioremap(hdmi_mem->start,
						resource_size(hdmi_mem));
	if (!hdmi.hdmi_data.base_wp) {
		DSSERR("can't ioremap WP\n");
		return -ENOMEM;
	}

	r = hdmi_get_clocks(pdev);
	if (r) {
		iounmap(hdmi.hdmi_data.base_wp);
		return r;
	}

	pm_runtime_enable(&pdev->dev);

	r = request_irq(gpio_to_irq(hdmi.dssdev->hpd_gpio), hpd_irq_handler,
			IRQF_DISABLED | IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
			"hpd", NULL);
	if (r < 0) {
		pr_err("hdmi: request_irq %d failed\n",
			gpio_to_irq(hdmi.dssdev->hpd_gpio));
		return -EINVAL;
	}

	hdmi.hdmi_irq = platform_get_irq(pdev, 0);
// LGE_CHANGE_S [sungho.jung@lge.com] 2012-02-20
#if defined(CONFIG_MACH_LGE_CX2)
	r = request_irq(hdmi.hdmi_irq, hdmi_irq_handler, 0, "OMAP HDMI", NULL);
	if (r < 0) {
		pr_err("hdmi: request_irq %s failed\n",
			pdev->name);
		return -EINVAL;
	}
#endif
// LGE_CHANGE_E [sungho.jung@lge.com] 2012-02-20

	hdmi.hdmi_data.hdmi_core_sys_offset = HDMI_CORE_SYS;
	hdmi.hdmi_data.hdmi_core_av_offset = HDMI_CORE_AV;
	hdmi.hdmi_data.hdmi_pll_offset = HDMI_PLLCTRL;
	hdmi.hdmi_data.hdmi_phy_offset = HDMI_PHY;
	hdmi.wp_reset_done = false;

	hdmi_panel_init();

// LGE_CHANGE_S [sungho.jung@lge.com] 2012-03-03. Remove unnessary code for hpd
#if 0
	if(hdmi_get_current_hpd())
		hdmi_panel_hpd_handler(1);
#endif
// LGE_CHANGE_E [sungho.jung@lge.com] 2012-03-03.

	return 0;
}

static int omapdss_hdmihw_remove(struct platform_device *pdev)
{
	hdmi_panel_exit();

	if (hdmi.dssdev)
		free_irq(gpio_to_irq(hdmi.dssdev->hpd_gpio), hpd_irq_handler);
	hdmi.dssdev = NULL;

	pm_runtime_disable(&pdev->dev);

	hdmi_put_clocks();

	iounmap(hdmi.hdmi_data.base_wp);

	return 0;
}

static struct platform_driver omapdss_hdmihw_driver = {
	.probe          = omapdss_hdmihw_probe,
	.remove         = omapdss_hdmihw_remove,
	.driver         = {
		.name   = "omapdss_hdmi",
		.owner  = THIS_MODULE,
	},
};

int hdmi_init_platform_driver(void)
{
	return platform_driver_register(&omapdss_hdmihw_driver);
}

void hdmi_uninit_platform_driver(void)
{
	return platform_driver_unregister(&omapdss_hdmihw_driver);
}

void hdmi_dump_regs(struct seq_file *s)
{
	if (hdmi_runtime_get())
		return;

	hdmi_ti_4xxx_dump_regs(&hdmi.hdmi_data, s);

	hdmi_runtime_put();
}
