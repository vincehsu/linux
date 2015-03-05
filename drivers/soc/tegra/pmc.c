/*
 * drivers/soc/tegra/pmc.c
 *
 * Copyright (c) 2010 Google, Inc
 *
 * Author:
 *	Colin Cross <ccross@google.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/kernel.h>
#include <linux/clk.h>
#include <linux/clk/tegra.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/export.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/reboot.h>
#include <linux/regulator/consumer.h>
#include <linux/reset.h>
#include <linux/sched.h>
#include <linux/seq_file.h>
#include <linux/spinlock.h>

#include <soc/tegra/common.h>
#include <soc/tegra/fuse.h>
#include <soc/tegra/mc.h>
#include <soc/tegra/pmc.h>

#define PMC_CNTRL			0x0
#define  PMC_CNTRL_SYSCLK_POLARITY	(1 << 10)  /* sys clk polarity */
#define  PMC_CNTRL_SYSCLK_OE		(1 << 11)  /* system clock enable */
#define  PMC_CNTRL_SIDE_EFFECT_LP0	(1 << 14)  /* LP0 when CPU pwr gated */
#define  PMC_CNTRL_CPU_PWRREQ_POLARITY	(1 << 15)  /* CPU pwr req polarity */
#define  PMC_CNTRL_CPU_PWRREQ_OE	(1 << 16)  /* CPU pwr req enable */
#define  PMC_CNTRL_INTR_POLARITY	(1 << 17)  /* inverts INTR polarity */

#define DPD_SAMPLE			0x020
#define  DPD_SAMPLE_ENABLE		(1 << 0)
#define  DPD_SAMPLE_DISABLE		(0 << 0)

#define PWRGATE_TOGGLE			0x30
#define  PWRGATE_TOGGLE_START		(1 << 8)

#define REMOVE_CLAMPING			0x34

#define PWRGATE_STATUS			0x38

#define PMC_SCRATCH0			0x50
#define  PMC_SCRATCH0_MODE_RECOVERY	(1 << 31)
#define  PMC_SCRATCH0_MODE_BOOTLOADER	(1 << 30)
#define  PMC_SCRATCH0_MODE_RCM		(1 << 1)
#define  PMC_SCRATCH0_MODE_MASK		(PMC_SCRATCH0_MODE_RECOVERY | \
					 PMC_SCRATCH0_MODE_BOOTLOADER | \
					 PMC_SCRATCH0_MODE_RCM)

#define PMC_CPUPWRGOOD_TIMER		0xc8
#define PMC_CPUPWROFF_TIMER		0xcc

#define PMC_SCRATCH41			0x140

#define PMC_SENSOR_CTRL			0x1b0
#define PMC_SENSOR_CTRL_SCRATCH_WRITE	(1 << 2)
#define PMC_SENSOR_CTRL_ENABLE_RST	(1 << 1)

#define IO_DPD_REQ			0x1b8
#define  IO_DPD_REQ_CODE_IDLE		(0 << 30)
#define  IO_DPD_REQ_CODE_OFF		(1 << 30)
#define  IO_DPD_REQ_CODE_ON		(2 << 30)
#define  IO_DPD_REQ_CODE_MASK		(3 << 30)

#define IO_DPD_STATUS			0x1bc
#define IO_DPD2_REQ			0x1c0
#define IO_DPD2_STATUS			0x1c4
#define SEL_DPD_TIM			0x1c8

#define PMC_SCRATCH54			0x258
#define PMC_SCRATCH54_DATA_SHIFT	8
#define PMC_SCRATCH54_ADDR_SHIFT	0

#define PMC_SCRATCH55			0x25c
#define PMC_SCRATCH55_RESET_TEGRA	(1 << 31)
#define PMC_SCRATCH55_CNTRL_ID_SHIFT	27
#define PMC_SCRATCH55_PINMUX_SHIFT	24
#define PMC_SCRATCH55_16BITOP		(1 << 15)
#define PMC_SCRATCH55_CHECKSUM_SHIFT	16
#define PMC_SCRATCH55_I2CSLV1_SHIFT	0

#define GPU_RG_CNTRL			0x2d4

#define MAX_CLK_NUM		5
#define MAX_RESET_NUM		5
#define MAX_SWGROUP_NUM		5

struct tegra_powergate {
	struct generic_pm_domain base;
	struct tegra_pmc *pmc;
	unsigned int id;
	const char *name;
	struct list_head head;
	struct device_node *of_node;
	struct clk *clk[MAX_CLK_NUM];
	struct reset_control *reset[MAX_RESET_NUM];
	struct tegra_mc_swgroup *swgroup[MAX_SWGROUP_NUM];
	bool is_vdd;
	struct regulator *vdd;
};

static inline struct tegra_powergate *
to_powergate(struct generic_pm_domain *domain)
{
	return container_of(domain, struct tegra_powergate, base);
}

struct tegra_pmc_soc {
	unsigned int num_powergates;
	const char *const *powergates;
	unsigned int num_cpu_powergates;
	const u8 *cpu_powergates;

	bool has_tsense_reset;
	bool has_gpu_clamps;
	bool is_legacy_powergate;
};

/**
 * struct tegra_pmc - NVIDIA Tegra PMC
 * @dev: pointer to parent device
 * @base: pointer to I/O remapped register region
 * @clk: pointer to pclk clock
 * @soc: SoC-specific data
 * @rate: currently configured rate of pclk
 * @suspend_mode: lowest suspend mode available
 * @cpu_good_time: CPU power good time (in microseconds)
 * @cpu_off_time: CPU power off time (in microsecends)
 * @core_osc_time: core power good OSC time (in microseconds)
 * @core_pmu_time: core power good PMU time (in microseconds)
 * @core_off_time: core power off time (in microseconds)
 * @corereq_high: core power request is active-high
 * @sysclkreq_high: system clock request is active-high
 * @combined_req: combined power request for CPU & core
 * @cpu_pwr_good_en: CPU power good signal is enabled
 * @lp0_vec_phys: physical base address of the LP0 warm boot code
 * @lp0_vec_size: size of the LP0 warm boot code
 * @powergates: list of power gates
 * @powergates_lock: mutex for power gate register access
 * @nb: bus notifier for generic power domains
 */
struct tegra_pmc {
	struct device *dev;
	void __iomem *base;
	struct clk *clk;

	const struct tegra_pmc_soc *soc;

	unsigned long rate;

	enum tegra_suspend_mode suspend_mode;
	u32 cpu_good_time;
	u32 cpu_off_time;
	u32 core_osc_time;
	u32 core_pmu_time;
	u32 core_off_time;
	bool corereq_high;
	bool sysclkreq_high;
	bool combined_req;
	bool cpu_pwr_good_en;
	u32 lp0_vec_phys;
	u32 lp0_vec_size;

	struct tegra_powergate *powergates;
	struct mutex powergates_lock;
	struct notifier_block nb;

	struct list_head powergate_list;
	int power_domain_num;
};

static struct tegra_pmc *pmc = &(struct tegra_pmc) {
	.base = NULL,
	.suspend_mode = TEGRA_SUSPEND_NONE,
};

static u32 tegra_pmc_readl(unsigned long offset)
{
	return readl(pmc->base + offset);
}

static void tegra_pmc_writel(u32 value, unsigned long offset)
{
	writel(value, pmc->base + offset);
}

/**
 * tegra_powergate_set() - set the state of a partition
 * @id: partition ID
 * @new_state: new state of the partition
 */
static int tegra_powergate_set(int id, bool new_state)
{
	bool status;

	mutex_lock(&pmc->powergates_lock);

	status = tegra_pmc_readl(PWRGATE_STATUS) & (1 << id);

	if (status == new_state) {
		mutex_unlock(&pmc->powergates_lock);
		return 0;
	}

	tegra_pmc_writel(PWRGATE_TOGGLE_START | id, PWRGATE_TOGGLE);

	mutex_unlock(&pmc->powergates_lock);

	return 0;
}

/**
 * tegra_powergate_is_powered() - check if partition is powered
 * @id: partition ID
 */
int tegra_powergate_is_powered(int id)
{
	u32 status;

	if (!pmc->soc || id < 0 || id >= pmc->soc->num_powergates)
		return -EINVAL;

	status = tegra_pmc_readl(PWRGATE_STATUS) & (1 << id);
	return !!status;
}

/**
 * tegra_powergate_remove_clamping() - remove power clamps for partition
 * @id: partition ID
 *
 * TODO: make this function static once we get rid of all outside callers
 */
int tegra_powergate_remove_clamping(int id)
{
	u32 mask;

	if (!pmc->soc || id < 0 || id >= pmc->soc->num_powergates)
		return -EINVAL;

	/*
	 * On Tegra124 and later, the clamps for the GPU are controlled by a
	 * separate register (with different semantics).
	 */
	if (id == TEGRA_POWERGATE_3D) {
		if (pmc->soc->has_gpu_clamps) {
			tegra_pmc_writel(0, GPU_RG_CNTRL);
			return 0;
		}
	}

	/*
	 * PCIE and VDE clamping bits are swapped relatively to the partition
	 * ids
	 */
	if (id == TEGRA_POWERGATE_VDEC)
		mask = (1 << TEGRA_POWERGATE_PCIE);
	else if (id == TEGRA_POWERGATE_PCIE)
		mask = (1 << TEGRA_POWERGATE_VDEC);
	else
		mask = (1 << id);

	tegra_pmc_writel(mask, REMOVE_CLAMPING);

	return 0;
}
EXPORT_SYMBOL(tegra_powergate_remove_clamping);

#ifdef CONFIG_SMP
/**
 * tegra_get_cpu_powergate_id() - convert from CPU ID to partition ID
 * @cpuid: CPU partition ID
 *
 * Returns the partition ID corresponding to the CPU partition ID or a
 * negative error code on failure.
 */
static int tegra_get_cpu_powergate_id(int cpuid)
{
	if (pmc->soc && cpuid > 0 && cpuid < pmc->soc->num_cpu_powergates)
		return pmc->soc->cpu_powergates[cpuid];

	return -EINVAL;
}

/**
 * tegra_pmc_cpu_is_powered() - check if CPU partition is powered
 * @cpuid: CPU partition ID
 */
bool tegra_pmc_cpu_is_powered(int cpuid)
{
	int id;

	id = tegra_get_cpu_powergate_id(cpuid);
	if (id < 0)
		return false;

	return tegra_powergate_is_powered(id);
}

/**
 * tegra_pmc_cpu_power_on() - power on CPU partition
 * @cpuid: CPU partition ID
 */
int tegra_pmc_cpu_power_on(int cpuid)
{
	int id;

	id = tegra_get_cpu_powergate_id(cpuid);
	if (id < 0)
		return id;

	return tegra_powergate_set(id, true);
}

/**
 * tegra_pmc_cpu_remove_clamping() - remove power clamps for CPU partition
 * @cpuid: CPU partition ID
 */
int tegra_pmc_cpu_remove_clamping(int cpuid)
{
	int id;

	id = tegra_get_cpu_powergate_id(cpuid);
	if (id < 0)
		return id;

	usleep_range(10, 20);

	return tegra_powergate_remove_clamping(id);
}
#endif /* CONFIG_SMP */

/**
 * tegra_pmc_restart() - reboot the system
 * @mode: which mode to reboot in
 * @cmd: reboot command
 */
void tegra_pmc_restart(enum reboot_mode mode, const char *cmd)
{
	u32 value;

	value = tegra_pmc_readl(PMC_SCRATCH0);
	value &= ~PMC_SCRATCH0_MODE_MASK;

	if (cmd) {
		if (strcmp(cmd, "recovery") == 0)
			value |= PMC_SCRATCH0_MODE_RECOVERY;

		if (strcmp(cmd, "bootloader") == 0)
			value |= PMC_SCRATCH0_MODE_BOOTLOADER;

		if (strcmp(cmd, "forced-recovery") == 0)
			value |= PMC_SCRATCH0_MODE_RCM;
	}

	tegra_pmc_writel(value, PMC_SCRATCH0);

	value = tegra_pmc_readl(0);
	value |= 0x10;
	tegra_pmc_writel(value, 0);
}

static bool tegra_pmc_powergate_is_powered(struct tegra_powergate *powergate)
{
	u32 status = tegra_pmc_readl(PWRGATE_STATUS);

	if (!powergate->is_vdd)
		return (status & BIT(powergate->id)) != 0;

	if (IS_ERR(powergate->vdd))
		return false;
	else
		return regulator_is_enabled(powergate->vdd);
}

static int tegra_pmc_powergate_set(struct tegra_powergate *powergate,
				   bool new_state)
{
	u32 status, mask = new_state ? BIT(powergate->id) : 0;
	bool state = false;
	unsigned long timeout;
	int err = -ETIMEDOUT;


	mutex_lock(&pmc->powergates_lock);

	/* check the current state of the partition */
	status = tegra_pmc_readl(PWRGATE_STATUS);
	state = !!(status & BIT(powergate->id));

	/* nothing to do */
	if (new_state == state) {
		mutex_unlock(&pmc->powergates_lock);
		return 0;
	}

	/* toggle partition state and wait for state change to finish */
	tegra_pmc_writel(PWRGATE_TOGGLE_START | powergate->id, PWRGATE_TOGGLE);

	timeout = jiffies + msecs_to_jiffies(50);
	while (time_before(jiffies, timeout)) {
		status = tegra_pmc_readl(PWRGATE_STATUS);
		if ((status & BIT(powergate->id)) == mask) {
			err = 0;
			break;
		}

		usleep_range(10, 20);
	}

	mutex_unlock(&pmc->powergates_lock);

	return err;
}

static int tegra_pmc_powergate_enable_clocks(
		struct tegra_powergate *powergate)
{
	int i, err;

	for (i = 0; i < MAX_CLK_NUM; i++) {
		if (!powergate->clk[i])
			break;

		err = clk_prepare_enable(powergate->clk[i]);
		if (err)
			goto out;
	}

	return 0;

out:
	while (i--)
		clk_disable_unprepare(powergate->clk[i]);
	return err;
}

static void tegra_pmc_powergate_disable_clocks(
		struct tegra_powergate *powergate)
{
	int i;

	for (i = 0; i < MAX_CLK_NUM; i++) {
		if (!powergate->clk[i])
			break;

		clk_disable_unprepare(powergate->clk[i]);
	}
}

static int tegra_pmc_powergate_mc_flush(struct tegra_powergate *powergate)
{
	int i, err;

	for (i = 0; i < MAX_SWGROUP_NUM; i++) {
		if (!powergate->swgroup[i])
			break;

		err = tegra_mc_flush(powergate->swgroup[i]);
		if (err)
			return err;
	}

	return 0;
}

static int tegra_pmc_powergate_mc_flush_done(struct tegra_powergate *powergate)
{
	int i, err;

	for (i = 0; i < MAX_SWGROUP_NUM; i++) {
		if (!powergate->swgroup[i])
			break;

		err = tegra_mc_flush_done(powergate->swgroup[i]);
		if (err)
			return err;
	}

	return 0;

}

static int tegra_pmc_powergate_reset_assert(
		struct tegra_powergate *powergate)
{
	int i, err;

	for (i = 0; i < MAX_RESET_NUM; i++) {
		if (!powergate->reset[i])
			break;

		err = reset_control_assert(powergate->reset[i]);
		if (err)
			return err;
	}

	return 0;
}

static int tegra_pmc_powergate_reset_deassert(
		struct tegra_powergate *powergate)
{
	int i, err;

	for (i = 0; i < MAX_RESET_NUM; i++) {
		if (!powergate->reset[i])
			break;

		err = reset_control_deassert(powergate->reset[i]);
		if (err)
			return err;
	}

	return 0;
}

static int tegra_powergate_get_regulator(struct tegra_powergate *powergate)
{
	struct platform_device *pdev;

	if (!powergate->is_vdd)
		return -EINVAL;

	if (powergate->vdd && !IS_ERR(powergate->vdd))
		return 0;

	pdev = of_find_device_by_node(powergate->of_node);
	if (!pdev)
		return -EINVAL;

	powergate->vdd = devm_regulator_get_optional(&pdev->dev, "vdd");
	if (IS_ERR(powergate->vdd))
		return -EINVAL;

	return 0;
}

static int tegra_pmc_powergate_power_on(struct generic_pm_domain *domain)
{
	struct tegra_powergate *powergate = to_powergate(domain);
	struct tegra_pmc *pmc = powergate->pmc;
	int err;

	dev_dbg(pmc->dev, "> %s(domain=%p)\n", __func__, domain);
	dev_dbg(pmc->dev, "  name: %s\n", domain->name);

	if (powergate->is_vdd) {
		err = tegra_powergate_get_regulator(powergate);
		if (!err)
			err = regulator_enable(powergate->vdd);
	} else {
		err = tegra_pmc_powergate_set(powergate, true);
	}
	if (err < 0)
		goto out;
	udelay(10);

	if (pmc->soc->is_legacy_powergate) {
		err = tegra_pmc_powergate_reset_assert(powergate);
		if (err)
			goto out;
		udelay(10);
	}

	/*
	 * Some PCIe PLLs depend on external power supplies, and the power
	 * supplies are enabled in driver. So we don't touch PCIe clocks
	 * here. Refer to:
	 * Documentation/devicetree/bindings/pci/nvidia,tegra20-pcie.txt
	 */
	if (powergate->id != TEGRA_POWERGATE_PCIE) {
		err = tegra_pmc_powergate_enable_clocks(powergate);
		if (err)
			goto out;
		udelay(10);
	}

	err = tegra_powergate_remove_clamping(powergate->id);
	if (err)
		goto out;
	udelay(10);

	err = tegra_pmc_powergate_reset_deassert(powergate);
	if (err)
		goto out;
	udelay(10);

	err = tegra_pmc_powergate_mc_flush_done(powergate);
	if (err)
		goto out;
	udelay(10);

	if (powergate->id != TEGRA_POWERGATE_PCIE)
		tegra_pmc_powergate_disable_clocks(powergate);

	return 0;

out:
	dev_dbg(pmc->dev, "< %s() = %d\n", __func__, err);
	return err;
}

static int tegra_pmc_powergate_power_off(struct generic_pm_domain *domain)
{
	struct tegra_powergate *powergate = to_powergate(domain);
	struct tegra_pmc *pmc = powergate->pmc;
	int err;

	dev_dbg(pmc->dev, "> %s(domain=%p)\n", __func__, domain);
	dev_dbg(pmc->dev, "  name: %s\n", domain->name);

	/* never turn off these partitions */
	switch (powergate->id) {
	case TEGRA_POWERGATE_CPU:
	case TEGRA_POWERGATE_CPU1:
	case TEGRA_POWERGATE_CPU2:
	case TEGRA_POWERGATE_CPU3:
	case TEGRA_POWERGATE_CPU0:
	case TEGRA_POWERGATE_C0NC:
	case TEGRA_POWERGATE_IRAM:
		dev_dbg(pmc->dev, "not disabling always-on partition %s\n",
			domain->name);
		err = -EINVAL;
		goto out;
	}

	if (!pmc->soc->is_legacy_powergate) {
		err = tegra_pmc_powergate_enable_clocks(powergate);
		if (err)
			goto out;
		udelay(10);

		err = tegra_pmc_powergate_mc_flush(powergate);
		if (err)
			goto out;
		udelay(10);
	}

	err = tegra_pmc_powergate_reset_assert(powergate);
	if (err)
		goto out;
	udelay(10);

	if (!pmc->soc->is_legacy_powergate) {
		tegra_pmc_powergate_disable_clocks(powergate);
		udelay(10);
	}

	if (powergate->vdd)
		err = regulator_disable(powergate->vdd);
	else
		err = tegra_pmc_powergate_set(powergate, false);
	if (err)
		goto out;

	return 0;

out:
	dev_dbg(pmc->dev, "< %s() = %d\n", __func__, err);
	return err;
}

static int powergate_show(struct seq_file *s, void *data)
{
	unsigned int i;

	seq_printf(s, " powergate powered\n");
	seq_printf(s, "------------------\n");

	for (i = 0; i < pmc->soc->num_powergates; i++) {
		if (!pmc->soc->powergates[i])
			continue;

		seq_printf(s, " %9s %7s\n", pmc->soc->powergates[i],
			   tegra_powergate_is_powered(i) ? "yes" : "no");
	}

	return 0;
}

static int powergate_open(struct inode *inode, struct file *file)
{
	return single_open(file, powergate_show, inode->i_private);
}

static const struct file_operations powergate_fops = {
	.open = powergate_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int tegra_powergate_debugfs_init(void)
{
	struct dentry *d;

	d = debugfs_create_file("powergate", S_IRUGO, NULL, NULL,
				&powergate_fops);
	if (!d)
		return -ENOMEM;

	return 0;
}

static struct generic_pm_domain *
tegra_powergate_of_xlate(struct of_phandle_args *args, void *data)
{
	struct tegra_pmc *pmc = data;
	struct tegra_powergate *powergate;

	dev_dbg(pmc->dev, "> %s(args=%p, data=%p)\n", __func__, args, data);

	list_for_each_entry(powergate, &pmc->powergate_list, head) {
		if (!powergate->base.name)
			continue;

		if (powergate->id == args->args[0]) {
			dev_dbg(pmc->dev, "< %s() = %p\n", __func__, powergate);
			return &powergate->base;
		}
	}

	dev_dbg(pmc->dev, "< %s() = -ENOENT\n", __func__);
	return ERR_PTR(-ENOENT);
}

static int tegra_powergate_of_get_clks(struct tegra_powergate *powergate)
{
	struct clk *clk;
	int i, err;

	for (i = 0; i < MAX_CLK_NUM; i++) {
		clk = of_clk_get(powergate->of_node, i);
		if (IS_ERR(clk)) {
			if (PTR_ERR(clk) == -ENOENT)
				break;
			else
				goto err_clks;
		}

		powergate->clk[i] = clk;
	}

	return 0;

err_clks:
	err = PTR_ERR(clk);
	while (--i >= 0)
		clk_put(powergate->clk[i]);
	return err;
}

static int tegra_powergate_of_get_resets(struct tegra_powergate *powergate)
{
	struct reset_control *reset;
	int i;

	for (i = 0; i < MAX_RESET_NUM; i++) {
		reset = of_reset_control_get_by_index(powergate->of_node, i);
		if (IS_ERR(reset)) {
			if (PTR_ERR(reset) == -ENOENT)
				break;
			else
				return PTR_ERR(reset);
		}

		powergate->reset[i] = reset;
	}

	return 0;
}

static int tegra_powergate_of_get_swgroups(struct tegra_powergate *powergate)
{
	struct tegra_mc_swgroup *sg;
	int i;

	for (i = 0; i < MAX_SWGROUP_NUM; i++) {
		sg = tegra_mc_find_swgroup(powergate->of_node, i);
		if (IS_ERR_OR_NULL(sg)) {
			if (PTR_ERR(sg) == -ENOENT)
				break;
			else
				return -EINVAL;
		}

		powergate->swgroup[i] = sg;
	}

	return 0;
}

static int tegra_pmc_powergate_init_powerdomain(struct tegra_pmc *pmc)
{
	struct device_node *np;

	for_each_compatible_node(np, NULL, "nvidia,power-domains") {
		struct tegra_powergate *powergate;
		const char *name;
		int err;
		u32 id;
		bool off;

		err = of_property_read_string(np, "name", &name);
		if (err) {
			dev_err(pmc->dev, "no significant name for domain\n");
			return err;
		}

		err = of_property_read_u32(np, "domain", &id);
		if (err) {
			dev_err(pmc->dev, "no powergate ID for domain\n");
			return err;
		}

		powergate = devm_kzalloc(pmc->dev, sizeof(*powergate),
						GFP_KERNEL);
		if (!powergate) {
			dev_err(pmc->dev, "failed to allocate memory for domain %s\n",
					name);
			return -ENOMEM;
		}

		if (of_property_read_bool(np, "external-power-rail")) {
			powergate->is_vdd = true;
			err = tegra_powergate_get_regulator(powergate);
			if (err) {
				/*
				 * The regulator might not be ready yet, so just
				 * give a warning instead of failing the whole
				 * init.
				 */
				dev_warn(pmc->dev, "couldn't locate regulator\n");
			}
		}

		powergate->of_node = np;
		powergate->name = name;
		powergate->id = id;
		powergate->base.name = kstrdup(powergate->name, GFP_KERNEL);
		powergate->base.power_off = tegra_pmc_powergate_power_off;
		powergate->base.power_on = tegra_pmc_powergate_power_on;
		powergate->pmc = pmc;

		err = tegra_powergate_of_get_clks(powergate);
		if (err)
			return err;

		err = tegra_powergate_of_get_resets(powergate);
		if (err)
			return err;

		err = tegra_powergate_of_get_swgroups(powergate);
		if (err)
			return err;

		list_add_tail(&powergate->head, &pmc->powergate_list);

		if ((powergate->is_vdd && !IS_ERR(powergate->vdd)) ||
			!powergate->is_vdd)
			tegra_pmc_powergate_power_off(&powergate->base);

		off = !tegra_pmc_powergate_is_powered(powergate);
		pm_genpd_init(&powergate->base, NULL, off);

		pmc->power_domain_num++;

		dev_info(pmc->dev, "added power domain %s\n", powergate->name);
	}

	dev_info(pmc->dev, "%d power domains added\n", pmc->power_domain_num);
	return 0;
}

static int tegra_pmc_powergate_init_subdomain(struct tegra_pmc *pmc)
{
	struct tegra_powergate *powergate;

	list_for_each_entry(powergate, &pmc->powergate_list, head) {
		struct device_node *pdn;
		struct tegra_powergate *parent = NULL;
		struct tegra_powergate *temp;
		int err;

		pdn = of_parse_phandle(powergate->of_node, "depend-on", 0);
		if (!pdn)
			continue;

		list_for_each_entry(temp, &pmc->powergate_list, head) {
			if (temp->of_node == pdn) {
				parent = temp;
				break;
			}
		}

		if (!parent)
			return -EINVAL;

		err = pm_genpd_add_subdomain_names(parent->name,
				powergate->name);
		if (err)
			return err;
	}

	return 0;
}

static int tegra_powergate_init(struct tegra_pmc *pmc)
{
	struct device_node *np = pmc->dev->of_node;
	int err = 0;

	dev_dbg(pmc->dev, "> %s(pmc=%p)\n", __func__, pmc);

	INIT_LIST_HEAD(&pmc->powergate_list);
	err = tegra_pmc_powergate_init_powerdomain(pmc);
	if (err)
		goto out;

	err = tegra_pmc_powergate_init_subdomain(pmc);
	if (err < 0)
		return err;

	err = __of_genpd_add_provider(np, tegra_powergate_of_xlate, pmc);
	if (err < 0)
		return err;

out:
	dev_dbg(pmc->dev, "< %s() = %d\n", __func__, err);
	return err;
}

static int tegra_io_rail_prepare(int id, unsigned long *request,
				 unsigned long *status, unsigned int *bit)
{
	unsigned long rate, value;
	struct clk *clk;

	*bit = id % 32;

	/*
	 * There are two sets of 30 bits to select IO rails, but bits 30 and
	 * 31 are control bits rather than IO rail selection bits.
	 */
	if (id > 63 || *bit == 30 || *bit == 31)
		return -EINVAL;

	if (id < 32) {
		*status = IO_DPD_STATUS;
		*request = IO_DPD_REQ;
	} else {
		*status = IO_DPD2_STATUS;
		*request = IO_DPD2_REQ;
	}

	clk = clk_get_sys(NULL, "pclk");
	if (IS_ERR(clk))
		return PTR_ERR(clk);

	rate = clk_get_rate(clk);
	clk_put(clk);

	tegra_pmc_writel(DPD_SAMPLE_ENABLE, DPD_SAMPLE);

	/* must be at least 200 ns, in APB (PCLK) clock cycles */
	value = DIV_ROUND_UP(1000000000, rate);
	value = DIV_ROUND_UP(200, value);
	tegra_pmc_writel(value, SEL_DPD_TIM);

	return 0;
}

static int tegra_io_rail_poll(unsigned long offset, unsigned long mask,
			      unsigned long val, unsigned long timeout)
{
	unsigned long value;

	timeout = jiffies + msecs_to_jiffies(timeout);

	while (time_after(timeout, jiffies)) {
		value = tegra_pmc_readl(offset);
		if ((value & mask) == val)
			return 0;

		usleep_range(250, 1000);
	}

	return -ETIMEDOUT;
}

static void tegra_io_rail_unprepare(void)
{
	tegra_pmc_writel(DPD_SAMPLE_DISABLE, DPD_SAMPLE);
}

int tegra_io_rail_power_on(int id)
{
	unsigned long request, status, value;
	unsigned int bit, mask;
	int err;

	err = tegra_io_rail_prepare(id, &request, &status, &bit);
	if (err < 0)
		return err;

	mask = 1 << bit;

	value = tegra_pmc_readl(request);
	value |= mask;
	value &= ~IO_DPD_REQ_CODE_MASK;
	value |= IO_DPD_REQ_CODE_OFF;
	tegra_pmc_writel(value, request);

	err = tegra_io_rail_poll(status, mask, 0, 250);
	if (err < 0)
		return err;

	tegra_io_rail_unprepare();

	return 0;
}
EXPORT_SYMBOL(tegra_io_rail_power_on);

int tegra_io_rail_power_off(int id)
{
	unsigned long request, status, value;
	unsigned int bit, mask;
	int err;

	err = tegra_io_rail_prepare(id, &request, &status, &bit);
	if (err < 0)
		return err;

	mask = 1 << bit;

	value = tegra_pmc_readl(request);
	value |= mask;
	value &= ~IO_DPD_REQ_CODE_MASK;
	value |= IO_DPD_REQ_CODE_ON;
	tegra_pmc_writel(value, request);

	err = tegra_io_rail_poll(status, mask, mask, 250);
	if (err < 0)
		return err;

	tegra_io_rail_unprepare();

	return 0;
}
EXPORT_SYMBOL(tegra_io_rail_power_off);

#ifdef CONFIG_PM_SLEEP
enum tegra_suspend_mode tegra_pmc_get_suspend_mode(void)
{
	return pmc->suspend_mode;
}

void tegra_pmc_set_suspend_mode(enum tegra_suspend_mode mode)
{
	if (mode < TEGRA_SUSPEND_NONE || mode >= TEGRA_MAX_SUSPEND_MODE)
		return;

	pmc->suspend_mode = mode;
}

void tegra_pmc_enter_suspend_mode(enum tegra_suspend_mode mode)
{
	unsigned long long rate = 0;
	u32 value;

	switch (mode) {
	case TEGRA_SUSPEND_LP1:
		rate = 32768;
		break;

	case TEGRA_SUSPEND_LP2:
		rate = clk_get_rate(pmc->clk);
		break;

	default:
		break;
	}

	if (WARN_ON_ONCE(rate == 0))
		rate = 100000000;

	if (rate != pmc->rate) {
		u64 ticks;

		ticks = pmc->cpu_good_time * rate + USEC_PER_SEC - 1;
		do_div(ticks, USEC_PER_SEC);
		tegra_pmc_writel(ticks, PMC_CPUPWRGOOD_TIMER);

		ticks = pmc->cpu_off_time * rate + USEC_PER_SEC - 1;
		do_div(ticks, USEC_PER_SEC);
		tegra_pmc_writel(ticks, PMC_CPUPWROFF_TIMER);

		wmb();

		pmc->rate = rate;
	}

	value = tegra_pmc_readl(PMC_CNTRL);
	value &= ~PMC_CNTRL_SIDE_EFFECT_LP0;
	value |= PMC_CNTRL_CPU_PWRREQ_OE;
	tegra_pmc_writel(value, PMC_CNTRL);
}
#endif

static int tegra_pmc_parse_dt(struct tegra_pmc *pmc, struct device_node *np)
{
	u32 value, values[2];

	if (of_property_read_u32(np, "nvidia,suspend-mode", &value)) {
	} else {
		switch (value) {
		case 0:
			pmc->suspend_mode = TEGRA_SUSPEND_LP0;
			break;

		case 1:
			pmc->suspend_mode = TEGRA_SUSPEND_LP1;
			break;

		case 2:
			pmc->suspend_mode = TEGRA_SUSPEND_LP2;
			break;

		default:
			pmc->suspend_mode = TEGRA_SUSPEND_NONE;
			break;
		}
	}

	pmc->suspend_mode = tegra_pm_validate_suspend_mode(pmc->suspend_mode);

	if (of_property_read_u32(np, "nvidia,cpu-pwr-good-time", &value))
		pmc->suspend_mode = TEGRA_SUSPEND_NONE;

	pmc->cpu_good_time = value;

	if (of_property_read_u32(np, "nvidia,cpu-pwr-off-time", &value))
		pmc->suspend_mode = TEGRA_SUSPEND_NONE;

	pmc->cpu_off_time = value;

	if (of_property_read_u32_array(np, "nvidia,core-pwr-good-time",
				       values, ARRAY_SIZE(values)))
		pmc->suspend_mode = TEGRA_SUSPEND_NONE;

	pmc->core_osc_time = values[0];
	pmc->core_pmu_time = values[1];

	if (of_property_read_u32(np, "nvidia,core-pwr-off-time", &value))
		pmc->suspend_mode = TEGRA_SUSPEND_NONE;

	pmc->core_off_time = value;

	pmc->corereq_high = of_property_read_bool(np,
				"nvidia,core-power-req-active-high");

	pmc->sysclkreq_high = of_property_read_bool(np,
				"nvidia,sys-clock-req-active-high");

	pmc->combined_req = of_property_read_bool(np,
				"nvidia,combined-power-req");

	pmc->cpu_pwr_good_en = of_property_read_bool(np,
				"nvidia,cpu-pwr-good-en");

	if (of_property_read_u32_array(np, "nvidia,lp0-vec", values,
				       ARRAY_SIZE(values)))
		if (pmc->suspend_mode == TEGRA_SUSPEND_LP0)
			pmc->suspend_mode = TEGRA_SUSPEND_LP1;

	pmc->lp0_vec_phys = values[0];
	pmc->lp0_vec_size = values[1];

	return 0;
}

static void tegra_pmc_init(struct tegra_pmc *pmc)
{
	u32 value;

	/* Always enable CPU power request */
	value = tegra_pmc_readl(PMC_CNTRL);
	value |= PMC_CNTRL_CPU_PWRREQ_OE;
	tegra_pmc_writel(value, PMC_CNTRL);

	value = tegra_pmc_readl(PMC_CNTRL);

	if (pmc->sysclkreq_high)
		value &= ~PMC_CNTRL_SYSCLK_POLARITY;
	else
		value |= PMC_CNTRL_SYSCLK_POLARITY;

	/* configure the output polarity while the request is tristated */
	tegra_pmc_writel(value, PMC_CNTRL);

	/* now enable the request */
	value = tegra_pmc_readl(PMC_CNTRL);
	value |= PMC_CNTRL_SYSCLK_OE;
	tegra_pmc_writel(value, PMC_CNTRL);
}

void tegra_pmc_init_tsense_reset(struct tegra_pmc *pmc)
{
	static const char disabled[] = "emergency thermal reset disabled";
	u32 pmu_addr, ctrl_id, reg_addr, reg_data, pinmux;
	struct device *dev = pmc->dev;
	struct device_node *np;
	u32 value, checksum;

	if (!pmc->soc->has_tsense_reset)
		goto out;

	np = of_find_node_by_name(pmc->dev->of_node, "i2c-thermtrip");
	if (!np) {
		dev_warn(dev, "i2c-thermtrip node not found, %s.\n", disabled);
		goto out;
	}

	if (of_property_read_u32(np, "nvidia,i2c-controller-id", &ctrl_id)) {
		dev_err(dev, "I2C controller ID missing, %s.\n", disabled);
		goto out;
	}

	if (of_property_read_u32(np, "nvidia,bus-addr", &pmu_addr)) {
		dev_err(dev, "nvidia,bus-addr missing, %s.\n", disabled);
		goto out;
	}

	if (of_property_read_u32(np, "nvidia,reg-addr", &reg_addr)) {
		dev_err(dev, "nvidia,reg-addr missing, %s.\n", disabled);
		goto out;
	}

	if (of_property_read_u32(np, "nvidia,reg-data", &reg_data)) {
		dev_err(dev, "nvidia,reg-data missing, %s.\n", disabled);
		goto out;
	}

	if (of_property_read_u32(np, "nvidia,pinmux-id", &pinmux))
		pinmux = 0;

	value = tegra_pmc_readl(PMC_SENSOR_CTRL);
	value |= PMC_SENSOR_CTRL_SCRATCH_WRITE;
	tegra_pmc_writel(value, PMC_SENSOR_CTRL);

	value = (reg_data << PMC_SCRATCH54_DATA_SHIFT) |
		(reg_addr << PMC_SCRATCH54_ADDR_SHIFT);
	tegra_pmc_writel(value, PMC_SCRATCH54);

	value = PMC_SCRATCH55_RESET_TEGRA;
	value |= ctrl_id << PMC_SCRATCH55_CNTRL_ID_SHIFT;
	value |= pinmux << PMC_SCRATCH55_PINMUX_SHIFT;
	value |= pmu_addr << PMC_SCRATCH55_I2CSLV1_SHIFT;

	/*
	 * Calculate checksum of SCRATCH54, SCRATCH55 fields. Bits 23:16 will
	 * contain the checksum and are currently zero, so they are not added.
	 */
	checksum = reg_addr + reg_data + (value & 0xff) + ((value >> 8) & 0xff)
		+ ((value >> 24) & 0xff);
	checksum &= 0xff;
	checksum = 0x100 - checksum;

	value |= checksum << PMC_SCRATCH55_CHECKSUM_SHIFT;

	tegra_pmc_writel(value, PMC_SCRATCH55);

	value = tegra_pmc_readl(PMC_SENSOR_CTRL);
	value |= PMC_SENSOR_CTRL_ENABLE_RST;
	tegra_pmc_writel(value, PMC_SENSOR_CTRL);

	dev_info(pmc->dev, "emergency thermal reset enabled\n");

out:
	of_node_put(np);
	return;
}

static int tegra_pmc_probe(struct platform_device *pdev)
{
	void __iomem *base = pmc->base;
	struct resource *res;
	int err;

	dev_dbg(&pdev->dev, "> %s(pdev=%p)\n", __func__, pdev);

	err = tegra_pmc_parse_dt(pmc, pdev->dev.of_node);
	if (err < 0)
		return err;

	/* take over the memory region from the early initialization */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	pmc->base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(pmc->base))
		return PTR_ERR(pmc->base);

	iounmap(base);

	pmc->clk = devm_clk_get(&pdev->dev, "pclk");
	if (IS_ERR(pmc->clk)) {
		err = PTR_ERR(pmc->clk);
		dev_err(&pdev->dev, "failed to get pclk: %d\n", err);
		return err;
	}

	pmc->dev = &pdev->dev;

	tegra_pmc_init(pmc);

	tegra_pmc_init_tsense_reset(pmc);

	if (IS_ENABLED(CONFIG_PM_GENERIC_DOMAINS)) {
		err = tegra_powergate_init(pmc);
		if (err < 0)
			return err;
	}

	if (IS_ENABLED(CONFIG_DEBUG_FS)) {
		err = tegra_powergate_debugfs_init();
		if (err < 0)
			return err;
	}

	dev_dbg(&pdev->dev, "< %s()\n", __func__);
	return 0;
}

#if defined(CONFIG_PM_SLEEP) && defined(CONFIG_ARM)
static int tegra_pmc_suspend(struct device *dev)
{
	tegra_pmc_writel(virt_to_phys(tegra_resume), PMC_SCRATCH41);

	return 0;
}

static int tegra_pmc_resume(struct device *dev)
{
	tegra_pmc_writel(0x0, PMC_SCRATCH41);

	return 0;
}

static SIMPLE_DEV_PM_OPS(tegra_pmc_pm_ops, tegra_pmc_suspend, tegra_pmc_resume);

#endif

static const char * const tegra20_powergates[] = {
	[TEGRA_POWERGATE_CPU] = "cpu",
	[TEGRA_POWERGATE_3D] = "3d",
	[TEGRA_POWERGATE_VENC] = "venc",
	[TEGRA_POWERGATE_VDEC] = "vdec",
	[TEGRA_POWERGATE_PCIE] = "pcie",
	[TEGRA_POWERGATE_L2] = "l2",
	[TEGRA_POWERGATE_MPE] = "mpe",
};

static const struct tegra_pmc_soc tegra20_pmc_soc = {
	.num_powergates = ARRAY_SIZE(tegra20_powergates),
	.powergates = tegra20_powergates,
	.num_cpu_powergates = 0,
	.cpu_powergates = NULL,
	.has_tsense_reset = false,
	.has_gpu_clamps = false,
	.is_legacy_powergate = true,
};

static const char * const tegra30_powergates[] = {
	[TEGRA_POWERGATE_CPU] = "cpu0",
	[TEGRA_POWERGATE_3D] = "3d0",
	[TEGRA_POWERGATE_VENC] = "venc",
	[TEGRA_POWERGATE_VDEC] = "vdec",
	[TEGRA_POWERGATE_PCIE] = "pcie",
	[TEGRA_POWERGATE_L2] = "l2",
	[TEGRA_POWERGATE_MPE] = "mpe",
	[TEGRA_POWERGATE_HEG] = "heg",
	[TEGRA_POWERGATE_SATA] = "sata",
	[TEGRA_POWERGATE_CPU1] = "cpu1",
	[TEGRA_POWERGATE_CPU2] = "cpu2",
	[TEGRA_POWERGATE_CPU3] = "cpu3",
	[TEGRA_POWERGATE_CELP] = "celp",
	[TEGRA_POWERGATE_3D1] = "3d1",
};

static const u8 tegra30_cpu_powergates[] = {
	TEGRA_POWERGATE_CPU,
	TEGRA_POWERGATE_CPU1,
	TEGRA_POWERGATE_CPU2,
	TEGRA_POWERGATE_CPU3,
};

static const struct tegra_pmc_soc tegra30_pmc_soc = {
	.num_powergates = ARRAY_SIZE(tegra30_powergates),
	.powergates = tegra30_powergates,
	.num_cpu_powergates = ARRAY_SIZE(tegra30_cpu_powergates),
	.cpu_powergates = tegra30_cpu_powergates,
	.has_tsense_reset = true,
	.has_gpu_clamps = false,
	.is_legacy_powergate = true,
};

static const char * const tegra114_powergates[] = {
	[TEGRA_POWERGATE_CPU] = "crail",
	[TEGRA_POWERGATE_3D] = "3d",
	[TEGRA_POWERGATE_VENC] = "venc",
	[TEGRA_POWERGATE_VDEC] = "vdec",
	[TEGRA_POWERGATE_MPE] = "mpe",
	[TEGRA_POWERGATE_HEG] = "heg",
	[TEGRA_POWERGATE_CPU1] = "cpu1",
	[TEGRA_POWERGATE_CPU2] = "cpu2",
	[TEGRA_POWERGATE_CPU3] = "cpu3",
	[TEGRA_POWERGATE_CELP] = "celp",
	[TEGRA_POWERGATE_CPU0] = "cpu0",
	[TEGRA_POWERGATE_C0NC] = "c0nc",
	[TEGRA_POWERGATE_C1NC] = "c1nc",
	[TEGRA_POWERGATE_DIS] = "dis",
	[TEGRA_POWERGATE_DISB] = "disb",
	[TEGRA_POWERGATE_XUSBA] = "xusba",
	[TEGRA_POWERGATE_XUSBB] = "xusbb",
	[TEGRA_POWERGATE_XUSBC] = "xusbc",
};

static const u8 tegra114_cpu_powergates[] = {
	TEGRA_POWERGATE_CPU0,
	TEGRA_POWERGATE_CPU1,
	TEGRA_POWERGATE_CPU2,
	TEGRA_POWERGATE_CPU3,
};

static const struct tegra_pmc_soc tegra114_pmc_soc = {
	.num_powergates = ARRAY_SIZE(tegra114_powergates),
	.powergates = tegra114_powergates,
	.num_cpu_powergates = ARRAY_SIZE(tegra114_cpu_powergates),
	.cpu_powergates = tegra114_cpu_powergates,
	.has_tsense_reset = true,
	.has_gpu_clamps = false,
	.is_legacy_powergate = false,
};

static const char * const tegra124_powergates[] = {
	[TEGRA_POWERGATE_CPU] = "crail",
	[TEGRA_POWERGATE_3D] = "3d",
	[TEGRA_POWERGATE_VENC] = "venc",
	[TEGRA_POWERGATE_PCIE] = "pcie",
	[TEGRA_POWERGATE_VDEC] = "vdec",
	[TEGRA_POWERGATE_L2] = "l2",
	[TEGRA_POWERGATE_MPE] = "mpe",
	[TEGRA_POWERGATE_HEG] = "heg",
	[TEGRA_POWERGATE_SATA] = "sata",
	[TEGRA_POWERGATE_CPU1] = "cpu1",
	[TEGRA_POWERGATE_CPU2] = "cpu2",
	[TEGRA_POWERGATE_CPU3] = "cpu3",
	[TEGRA_POWERGATE_CELP] = "celp",
	[TEGRA_POWERGATE_CPU0] = "cpu0",
	[TEGRA_POWERGATE_C0NC] = "c0nc",
	[TEGRA_POWERGATE_C1NC] = "c1nc",
	[TEGRA_POWERGATE_SOR] = "sor",
	[TEGRA_POWERGATE_DIS] = "dis",
	[TEGRA_POWERGATE_DISB] = "disb",
	[TEGRA_POWERGATE_XUSBA] = "xusba",
	[TEGRA_POWERGATE_XUSBB] = "xusbb",
	[TEGRA_POWERGATE_XUSBC] = "xusbc",
	[TEGRA_POWERGATE_VIC] = "vic",
	[TEGRA_POWERGATE_IRAM] = "iram",
};

static const u8 tegra124_cpu_powergates[] = {
	TEGRA_POWERGATE_CPU0,
	TEGRA_POWERGATE_CPU1,
	TEGRA_POWERGATE_CPU2,
	TEGRA_POWERGATE_CPU3,
};

static const struct tegra_pmc_soc tegra124_pmc_soc = {
	.num_powergates = ARRAY_SIZE(tegra124_powergates),
	.powergates = tegra124_powergates,
	.num_cpu_powergates = ARRAY_SIZE(tegra124_cpu_powergates),
	.cpu_powergates = tegra124_cpu_powergates,
	.has_tsense_reset = true,
	.has_gpu_clamps = true,
	.is_legacy_powergate = false,
};

static const struct of_device_id tegra_pmc_match[] = {
	{ .compatible = "nvidia,tegra124-pmc", .data = &tegra124_pmc_soc },
	{ .compatible = "nvidia,tegra114-pmc", .data = &tegra114_pmc_soc },
	{ .compatible = "nvidia,tegra30-pmc", .data = &tegra30_pmc_soc },
	{ .compatible = "nvidia,tegra20-pmc", .data = &tegra20_pmc_soc },
	{ }
};

static struct platform_driver tegra_pmc_driver = {
	.driver = {
		.name = "tegra-pmc",
		.suppress_bind_attrs = true,
		.of_match_table = tegra_pmc_match,
#if defined(CONFIG_PM_SLEEP) && defined(CONFIG_ARM)
		.pm = &tegra_pmc_pm_ops,
#endif
	},
	.probe = tegra_pmc_probe,
};
module_platform_driver(tegra_pmc_driver);

/*
 * Early initialization to allow access to registers in the very early boot
 * process.
 */
static int __init tegra_pmc_early_init(void)
{
	const struct of_device_id *match;
	struct device_node *np;
	struct resource regs;
	bool invert;
	u32 value;

	if (!soc_is_tegra())
		return 0;

	np = of_find_matching_node_and_match(NULL, tegra_pmc_match, &match);
	if (!np) {
		pr_warn("PMC device node not found, disabling powergating\n");

		regs.start = 0x7000e400;
		regs.end = 0x7000e7ff;
		regs.flags = IORESOURCE_MEM;

		pr_warn("Using memory region %pR\n", &regs);
	} else {
		pmc->soc = match->data;
	}

	if (of_address_to_resource(np, 0, &regs) < 0) {
		pr_err("failed to get PMC registers\n");
		return -ENXIO;
	}

	pmc->base = ioremap_nocache(regs.start, resource_size(&regs));
	if (!pmc->base) {
		pr_err("failed to map PMC registers\n");
		return -ENXIO;
	}

	mutex_init(&pmc->powergates_lock);

	invert = of_property_read_bool(np, "nvidia,invert-interrupt");

	value = tegra_pmc_readl(PMC_CNTRL);

	if (invert)
		value |= PMC_CNTRL_INTR_POLARITY;
	else
		value &= ~PMC_CNTRL_INTR_POLARITY;

	tegra_pmc_writel(value, PMC_CNTRL);

	return 0;
}
early_initcall(tegra_pmc_early_init);
