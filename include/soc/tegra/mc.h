/*
 * Copyright (C) 2014 NVIDIA Corporation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef __SOC_TEGRA_MC_H__
#define __SOC_TEGRA_MC_H__

#include <linux/types.h>

struct clk;
struct device;
struct page;

struct tegra_smmu_enable {
	unsigned int reg;
	unsigned int bit;
};

/* latency allowance */
struct tegra_mc_la {
	unsigned int reg;
	unsigned int shift;
	unsigned int mask;
	unsigned int def;
};

struct tegra_mc_client {
	unsigned int id;
	const char *name;
	unsigned int swgroup;

	unsigned int fifo_size;

	struct tegra_smmu_enable smmu;
	struct tegra_mc_la la;

	struct list_head head;
};

struct tegra_mc;

/* hot reset */
struct tegra_mc_hotreset {
	unsigned int swgroup;
	unsigned int ctrl;
	unsigned int status;
	unsigned int bit;
};

struct tegra_mc_swgroup {
	unsigned int id;
	struct tegra_mc *mc;
	struct list_head head;
	struct list_head clients;
};

struct tegra_mc_ops {
	int (*flush)(struct tegra_mc *mc,
			const struct tegra_mc_hotreset *hotreset);
	int (*flush_done)(struct tegra_mc *mc,
			const struct tegra_mc_hotreset *hotreset);
};

struct tegra_smmu_swgroup {
	unsigned int swgroup;
	unsigned int reg;
};

struct tegra_smmu_ops {
	void (*flush_dcache)(struct page *page, unsigned long offset,
			     size_t size);
};

struct tegra_smmu_soc {
	const struct tegra_mc_client *clients;
	unsigned int num_clients;

	const struct tegra_smmu_swgroup *swgroups;
	unsigned int num_swgroups;

	bool supports_round_robin_arbitration;
	bool supports_request_limit;

	unsigned int num_asids;

	const struct tegra_smmu_ops *ops;
};

struct tegra_smmu;

#ifdef CONFIG_TEGRA_IOMMU_SMMU
struct tegra_smmu *tegra_smmu_probe(struct device *dev,
				    const struct tegra_smmu_soc *soc,
				    struct tegra_mc *mc);
#else
static inline struct tegra_smmu *
tegra_smmu_probe(struct device *dev, const struct tegra_smmu_soc *soc,
		 struct tegra_mc *mc)
{
	return NULL;
}
#endif

struct tegra_mc_soc {
	struct tegra_mc_client *clients;
	unsigned int num_clients;

	const struct tegra_mc_hotreset *hotresets;
	unsigned int num_hotresets;

	const struct tegra_mc_ops *ops;

	const unsigned int *emem_regs;
	unsigned int num_emem_regs;

	unsigned int num_address_bits;
	unsigned int atom_size;

	const struct tegra_smmu_soc *smmu;
};

struct tegra_mc {
	struct device *dev;
	struct tegra_smmu *smmu;
	void __iomem *regs;
	struct clk *clk;
	int irq;

	const struct tegra_mc_soc *soc;
	unsigned long tick;

	struct list_head swgroups;

	struct mutex lock;
};

typedef int (*tegra_mc_op)(struct tegra_mc *mc,
				 const struct tegra_mc_hotreset *hotreset);

struct tegra_mc_swgroup *tegra_mc_find_swgroup(struct device_node *node,
					int index);
int tegra_mc_flush(struct tegra_mc_swgroup *sg);
int tegra_mc_flush_done(struct tegra_mc_swgroup *sg);

#endif /* __SOC_TEGRA_MC_H__ */
