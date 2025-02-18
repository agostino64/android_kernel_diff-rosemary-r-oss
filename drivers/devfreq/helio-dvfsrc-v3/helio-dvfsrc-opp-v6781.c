/*
 * Copyright (C) 2019 MediaTek Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See http://www.gnu.org/licenses/gpl-2.0.html for more details.
 */

#include <linux/kernel.h>
#include <mt-plat/mtk_devinfo.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <helio-dvfsrc-opp.h>
#include <helio-dvfsrc-qos.h>


#ifdef CONFIG_MEDIATEK_DRAMC
#include <dramc.h>
#endif
#ifdef CONFIG_MTK_DRAMC
#include <mtk_dramc.h>
#endif

#define V_VMODE_SHIFT 0
#define V_CT_SHIFT 5
#define V_CT_TEST_SHIFT 6
#define V_CT_OPP2_SHIFT 7

static int opp_min_bin_opp0;
static int opp_min_bin_opp2;


static int dvfsrc_rsrv;


#ifndef CONFIG_MTK_DRAMC
static int dram_steps_freq(unsigned int step)
{
	pr_info("get dram steps_freq fail\n");
	return 4266;
}
#endif

int ddr_level_to_step(int opp)
{
	unsigned int step[] = {0, 1, 3, 5, 7, 9};

	return step[opp];
}

void dvfsrc_opp_level_mapping(void)
{
	set_vcore_opp(VCORE_DVFS_OPP_0, VCORE_OPP_0);
	set_vcore_opp(VCORE_DVFS_OPP_1, VCORE_OPP_0);
	set_vcore_opp(VCORE_DVFS_OPP_2, VCORE_OPP_0);
	set_vcore_opp(VCORE_DVFS_OPP_3, VCORE_OPP_0);
	set_vcore_opp(VCORE_DVFS_OPP_4, VCORE_OPP_0);
	set_vcore_opp(VCORE_DVFS_OPP_5, VCORE_OPP_1);
	set_vcore_opp(VCORE_DVFS_OPP_6, VCORE_OPP_1);
	set_vcore_opp(VCORE_DVFS_OPP_7, VCORE_OPP_1);
	set_vcore_opp(VCORE_DVFS_OPP_8, VCORE_OPP_1);
	set_vcore_opp(VCORE_DVFS_OPP_9,  VCORE_OPP_2);
	set_vcore_opp(VCORE_DVFS_OPP_10, VCORE_OPP_2);
	set_vcore_opp(VCORE_DVFS_OPP_11, VCORE_OPP_2);
	set_vcore_opp(VCORE_DVFS_OPP_12, VCORE_OPP_2);

	set_ddr_opp(VCORE_DVFS_OPP_0, DDR_OPP_0);
	set_ddr_opp(VCORE_DVFS_OPP_1, DDR_OPP_1);
	set_ddr_opp(VCORE_DVFS_OPP_2, DDR_OPP_2);
	set_ddr_opp(VCORE_DVFS_OPP_3, DDR_OPP_3);
	set_ddr_opp(VCORE_DVFS_OPP_4, DDR_OPP_4);
	set_ddr_opp(VCORE_DVFS_OPP_5, DDR_OPP_1);
	set_ddr_opp(VCORE_DVFS_OPP_6, DDR_OPP_2);
	set_ddr_opp(VCORE_DVFS_OPP_7, DDR_OPP_3);
	set_ddr_opp(VCORE_DVFS_OPP_8, DDR_OPP_4);
	set_ddr_opp(VCORE_DVFS_OPP_9,  DDR_OPP_2);
	set_ddr_opp(VCORE_DVFS_OPP_10, DDR_OPP_3);
	set_ddr_opp(VCORE_DVFS_OPP_11, DDR_OPP_4);
	set_ddr_opp(VCORE_DVFS_OPP_12, DDR_OPP_5);
}

void dvfsrc_opp_table_init(void)
{
	int i;
	int vcore_opp, ddr_opp;

	for (i = 0; i < VCORE_DVFS_OPP_NUM; i++) {
		vcore_opp = get_vcore_opp(i);
		ddr_opp = get_ddr_opp(i);

		if (vcore_opp == VCORE_OPP_UNREQ || ddr_opp == DDR_OPP_UNREQ) {
			set_opp_table(i, 0, 0);
			continue;
		}
		set_opp_table(i, get_vcore_uv_table(vcore_opp),

		dram_steps_freq(ddr_level_to_step(ddr_opp)) * 1000);
	}
}

static int get_vb_volt(int vcore_opp)
{
	int idx;
	int ret = 0;
	int ptpod = get_devinfo_with_index(69);

	pr_info("%s: PTPOD: 0x%x\n", __func__, ptpod);

	switch (vcore_opp) {
	case VCORE_OPP_0:
		idx = (ptpod >> 4) & 0xF;
		if (idx >= opp_min_bin_opp0)
			ret = 1;
		break;
	case VCORE_OPP_2:
		idx = ptpod & 0xF;
		if (idx >= opp_min_bin_opp2 && idx < 10)
			ret = 1;
		break;
	default:
		break;
	}

	return ret * 25000;
}

static int is_rising_need(void)
{
#if 0
	int idx;
	int ptpod = get_devinfo_with_index(69);

	pr_info("%s: PTPOD: 0x%x\n", __func__, ptpod);

	idx = ptpod & 0xF;
	if (idx == 1 || idx == 2)
		return idx;
#endif
	return 0;
}

static int __init dvfsrc_opp_init(void)
{
	struct device_node *dvfsrc_node = NULL;
	int vcore_opp_0_uv, vcore_opp_1_uv, vcore_opp_2_uv;
	int is_vcore_ct = 0;
	int dvfs_v_mode = 0;
	int ct_test = 0;
	int ct_opp2_en = 0;
	void __iomem *dvfsrc_base;

#if defined(CONFIG_MTK_DVFSRC_MT6781_PRETEST)
	set_pwrap_cmd(VCORE_OPP_0, 3);
	set_pwrap_cmd(VCORE_OPP_1, 1);
	set_pwrap_cmd(VCORE_OPP_2, 0);

	vcore_opp_0_uv = 825000;
	vcore_opp_1_uv = 725000;
	vcore_opp_2_uv = 650000;

	/* meta vcore opp */
	spm_dvfs_pwrap_cmd(2,
		vcore_uv_to_pmic((vcore_opp_0_uv + vcore_opp_1_uv) >> 1));

#else
	set_pwrap_cmd(VCORE_OPP_0, 0);
	set_pwrap_cmd(VCORE_OPP_1, 2);
	set_pwrap_cmd(VCORE_OPP_2, 3);

	vcore_opp_0_uv = 800000;
	vcore_opp_1_uv = 700000;
	vcore_opp_2_uv = 650000;
#endif

	dvfsrc_node =
		of_find_compatible_node(NULL, NULL, "mediatek,dvfsrc");

	/* For Doe */
	if (dvfsrc_node) {
		dvfsrc_base = of_iomap(dvfsrc_node, 0);
		if (dvfsrc_base) {
			dvfsrc_rsrv = readl(dvfsrc_base + 0x610);
			iounmap(dvfsrc_base);
		}
		pr_info("%s: vcore_arg = %08x\n", __func__, dvfsrc_rsrv);
		dvfs_v_mode = (dvfsrc_rsrv >> V_VMODE_SHIFT) & 0x3;
		is_vcore_ct = (dvfsrc_rsrv >> V_CT_SHIFT) & 0x1;
		ct_test = (dvfsrc_rsrv >> V_CT_TEST_SHIFT) & 0x1;
		ct_opp2_en = (dvfsrc_rsrv >> V_CT_OPP2_SHIFT) & 0x1;
	}

	if (is_vcore_ct) {
		if (ct_test) {
			opp_min_bin_opp0 = 2;
			opp_min_bin_opp2 = 4;
		} else {
			opp_min_bin_opp0 = 3;
			opp_min_bin_opp2 = 5;
	}
		vcore_opp_0_uv -= get_vb_volt(VCORE_OPP_0);
		if (ct_opp2_en)
			vcore_opp_2_uv -= get_vb_volt(VCORE_OPP_2);
	}

	if (is_rising_need() == 2)
		vcore_opp_2_uv = 675000;
	else if (is_rising_need() == 1)
		vcore_opp_2_uv = 700000;

	if (dvfs_v_mode == 3) {
		/* LV */
		vcore_opp_0_uv = rounddown((vcore_opp_0_uv * 95) / 100, 6250);
		vcore_opp_1_uv = rounddown((vcore_opp_1_uv * 95) / 100, 6250);
		vcore_opp_2_uv = rounddown((vcore_opp_2_uv * 95) / 100, 6250);
	} else if (dvfs_v_mode == 1) {
		/* HV */
		vcore_opp_0_uv = roundup((vcore_opp_0_uv * 105) / 100, 6250);
		vcore_opp_1_uv = roundup((vcore_opp_1_uv * 105) / 100, 6250);
		vcore_opp_2_uv = roundup((vcore_opp_2_uv * 105) / 100, 6250);
	}

	pr_info("%s: CT=%d, VMODE=%d, RSV4=%x\n",
		__func__,
		is_vcore_ct,
			dvfs_v_mode,
			dvfsrc_rsrv);

	pr_info("%s: FINAL vcore_opp_uv: %d, %d, %d\n",
		__func__,
		vcore_opp_0_uv,
		vcore_opp_1_uv,
		vcore_opp_2_uv);

	set_vcore_uv_table(VCORE_OPP_0, vcore_opp_0_uv);
	set_vcore_uv_table(VCORE_OPP_1, vcore_opp_1_uv);
	set_vcore_uv_table(VCORE_OPP_2, vcore_opp_2_uv);

		/* meta vcore opp*/
	spm_dvfs_pwrap_cmd(1,
		vcore_uv_to_pmic((vcore_opp_0_uv + vcore_opp_1_uv) >> 1));

	return 0;
}

fs_initcall_sync(dvfsrc_opp_init)

static int __init dvfsrc_dram_opp_init(void)
{
	int i;

	for (i = 0; i < DDR_OPP_NUM; i++) {
		set_opp_ddr_freq(i,
			dram_steps_freq(ddr_level_to_step(i)) * 1000);
	}

	return 0;
}

device_initcall_sync(dvfsrc_dram_opp_init)
