/*******************************************************************************
  STMMAC Ethernet Driver -- MDIO bus implementation
  Provides Bus interface for MII registers

  Copyright (C) 2007-2009  STMicroelectronics Ltd

  This program is free software; you can redistribute it and/or modify it
  under the terms and conditions of the GNU General Public License,
  version 2, as published by the Free Software Foundation.

  This program is distributed in the hope it will be useful, but WITHOUT
  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
  FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
  more details.

  The full GNU General Public License is included in this distribution in
  the file called "COPYING".

  Author: Carl Shaw <carl.shaw@st.com>
  Maintainer: Giuseppe Cavallaro <peppe.cavallaro@st.com>
*******************************************************************************/

#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/mii.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/of_mdio.h>
#include <linux/phy.h>
#include <linux/slab.h>

#include "stmmac.h"

#define MII_BUSY 0x00000001
#define MII_WRITE 0x00000002
#define MII_DATA_MASK GENMASK(15, 0)

/* GMAC4 defines */
#define MII_GMAC4_GOC_SHIFT		2
#define MII_GMAC4_REG_ADDR_SHIFT	16
#define MII_GMAC4_WRITE			(1 << MII_GMAC4_GOC_SHIFT)
#define MII_GMAC4_READ			(3 << MII_GMAC4_GOC_SHIFT)
#define MII_GMAC4_C45E			BIT(1)

static void stmmac_mdio_c45_setup(struct stmmac_priv *priv, int phyreg,
				  u32 *val, u32 *data)
{
	unsigned int reg_shift = priv->hw->mii.reg_shift;
	unsigned int reg_mask = priv->hw->mii.reg_mask;

	*val |= MII_GMAC4_C45E;
	*val &= ~reg_mask;
	*val |= ((phyreg >> MII_DEVADDR_C45_SHIFT) << reg_shift) & reg_mask;

	*data |= (phyreg & MII_REGADDR_C45_MASK) << MII_GMAC4_REG_ADDR_SHIFT;
}

/**
 * stmmac_mdio_read
 * @bus: points to the mii_bus structure
 * @phyaddr: MII addr
 * @phyreg: MII reg
 * Description: it reads data from the MII register from within the phy device.
 * For the 7111 GMAC, we must set the bit 0 in the MII address register while
 * accessing the PHY registers.
 * Fortunately, it seems this has no drawback for the 7109 MAC.
 */
static int stmmac_mdio_read(struct mii_bus *bus, int phyaddr, int phyreg)
{
	struct net_device *ndev = bus->priv;
	struct stmmac_priv *priv = netdev_priv(ndev);
	unsigned int mii_address = priv->hw->mii.addr;
	unsigned int mii_data = priv->hw->mii.data;
	u32 v;
	int data = 0;
	u32 value = MII_BUSY;

	value |= (phyaddr << priv->hw->mii.addr_shift)
		& priv->hw->mii.addr_mask;
	value |= (phyreg << priv->hw->mii.reg_shift) & priv->hw->mii.reg_mask;
	value |= (priv->clk_csr << priv->hw->mii.clk_csr_shift)
		& priv->hw->mii.clk_csr_mask;
	if (priv->plat->has_gmac4) {
		value |= MII_GMAC4_READ;
		if (phyreg & MII_ADDR_C45)
			stmmac_mdio_c45_setup(priv, phyreg, &value, &data);
	}

	if (priv->plat->update_ahb_clk_cfg)
		priv->plat->update_ahb_clk_cfg(priv, 1, 0);

	if (readl_poll_timeout(priv->ioaddr + mii_address, v, !(v & MII_BUSY),
			       100, 10000))
		goto out;

	writel_relaxed(data, priv->ioaddr + mii_data);
	writel(value, priv->ioaddr + mii_address);

	if (readl_poll_timeout(priv->ioaddr + mii_address, v, !(v & MII_BUSY),
			       100, 10000))
		goto out;

	if (priv->plat->update_ahb_clk_cfg)
		priv->plat->update_ahb_clk_cfg(priv, 0, 0);

	/* Read the data from the MII data register */
	data = (int)readl_relaxed(priv->ioaddr + mii_data) & MII_DATA_MASK;

	return data;

out:
	if (priv->plat->update_ahb_clk_cfg)
		priv->plat->update_ahb_clk_cfg(priv, 0, 0);
	return -EBUSY;
}

/**
 * stmmac_mdio_write
 * @bus: points to the mii_bus structure
 * @phyaddr: MII addr
 * @phyreg: MII reg
 * @phydata: phy data
 * Description: it writes the data into the MII register from within the device.
 */
static int stmmac_mdio_write(struct mii_bus *bus, int phyaddr, int phyreg,
			     u16 phydata)
{
	struct net_device *ndev = bus->priv;
	struct stmmac_priv *priv = netdev_priv(ndev);
	unsigned int mii_address = priv->hw->mii.addr;
	unsigned int mii_data = priv->hw->mii.data;
	u32 v;
	u32 value = MII_BUSY;
	int data = phydata;
	int ret;

	value |= (phyaddr << priv->hw->mii.addr_shift)
		& priv->hw->mii.addr_mask;
	value |= (phyreg << priv->hw->mii.reg_shift) & priv->hw->mii.reg_mask;

	value |= (priv->clk_csr << priv->hw->mii.clk_csr_shift)
		& priv->hw->mii.clk_csr_mask;
	if (priv->plat->has_gmac4) {
		value |= MII_GMAC4_WRITE;
		if (phyreg & MII_ADDR_C45)
			stmmac_mdio_c45_setup(priv, phyreg, &value, &data);
	} else {
		value |= MII_WRITE;
	}

	if (priv->plat->update_ahb_clk_cfg)
		priv->plat->update_ahb_clk_cfg(priv, 1, 0);

	/* Wait until any existing MII operation is complete */
	if (readl_poll_timeout(priv->ioaddr + mii_address, v, !(v & MII_BUSY),
			       100, 10000))
		goto out;

	/* Set the MII address register to write */
	writel_relaxed(data, priv->ioaddr + mii_data);
	writel(value, priv->ioaddr + mii_address);

	/* Wait until any existing MII operation is complete */
	ret = readl_poll_timeout(priv->ioaddr + mii_address, v, !(v & MII_BUSY),
				 100, 10000);
	if (ret)
		goto out;

	if (priv->plat->update_ahb_clk_cfg)
		priv->plat->update_ahb_clk_cfg(priv, 0, 0);

	return ret;

out:
	if (priv->plat->update_ahb_clk_cfg)
		priv->plat->update_ahb_clk_cfg(priv, 0, 0);
	return -EBUSY;
}

/**
 * stmmac_mdio_reset
 * @bus: points to the mii_bus structure
 * Description: reset the MII bus
 */
int stmmac_mdio_reset(struct mii_bus *bus)
{
#if IS_ENABLED(CONFIG_STMMAC_PLATFORM)
	struct net_device *ndev = bus->priv;
	struct stmmac_priv *priv = netdev_priv(ndev);
	unsigned int mii_address = priv->hw->mii.addr;
	struct stmmac_mdio_bus_data *data = priv->plat->mdio_bus_data;

#ifdef CONFIG_OF
	if (priv->device->of_node) {
		if (data->reset_gpio < 0) {
			struct device_node *np = priv->device->of_node;

			if (!np)
				return 0;

			data->reset_gpio = of_get_named_gpio(np,
						"snps,reset-gpio", 0);
			if (data->reset_gpio < 0)
				return 0;

			data->active_low = of_property_read_bool(np,
						"snps,reset-active-low");
			of_property_read_u32_array(np,
				"snps,reset-delays-us", data->delays, 3);

			if (devm_gpio_request(priv->device, data->reset_gpio,
					      "mdio-reset"))
				return 0;
		}

		gpio_direction_output(data->reset_gpio,
				      data->active_low ? 1 : 0);
		if (data->delays[0])
			msleep(DIV_ROUND_UP(data->delays[0], 1000));

		gpio_set_value(data->reset_gpio, data->active_low ? 0 : 1);
		if (data->delays[1])
			msleep(DIV_ROUND_UP(data->delays[1], 1000));

		gpio_set_value(data->reset_gpio, data->active_low ? 1 : 0);
		if (data->delays[2])
			msleep(DIV_ROUND_UP(data->delays[2], 1000));
	}
#endif

	if (data->phy_reset) {
		netdev_dbg(ndev, "stmmac_mdio_reset: calling phy_reset\n");
		data->phy_reset(priv->plat->bsp_priv);
	}

	/* This is a workaround for problems with the STE101P PHY.
	 * It doesn't complete its reset until at least one clock cycle
	 * on MDC, so perform a dummy mdio read. To be updated for GMAC4
	 * if needed.
	 */
	if (!priv->plat->has_gmac4)
		writel(0, priv->ioaddr + mii_address);
#endif
	return 0;
}

/**
 * stmmac_mdio_register
 * @ndev: net device structure
 * Description: it registers the MII bus
 */
int stmmac_mdio_register(struct net_device *ndev)
{
	int err = 0;
	struct mii_bus *new_bus;
	struct stmmac_priv *priv = netdev_priv(ndev);
	struct stmmac_mdio_bus_data *mdio_bus_data = priv->plat->mdio_bus_data;
	struct device_node *mdio_node = priv->plat->mdio_node;
	struct device_node *np = priv->device->of_node;
	struct device *dev = ndev->dev.parent;
	struct phy_device *phydev;
	int addr, found, skip_phy_detect = 0;
	unsigned int phyaddr;

	if (!mdio_bus_data)
		return 0;

	new_bus = mdiobus_alloc();
	if (!new_bus)
		return -ENOMEM;

	if (mdio_bus_data->irqs)
		memcpy(new_bus->irq, mdio_bus_data->irqs, sizeof(new_bus->irq));

#ifdef CONFIG_OF
	if (priv->device->of_node)
		mdio_bus_data->reset_gpio = -1;
#endif

	new_bus->name = "stmmac";
	new_bus->read = &stmmac_mdio_read;
	new_bus->write = &stmmac_mdio_write;

	new_bus->reset = &stmmac_mdio_reset;
	snprintf(new_bus->id, MII_BUS_ID_SIZE, "%s-%x",
		 new_bus->name, priv->plat->bus_id);
	new_bus->priv = ndev;

	err = of_property_read_u32(np, "emac-phy-addr", &phyaddr);
	if (err) {
		new_bus->phy_mask = mdio_bus_data->phy_mask;
	} else {
		err = new_bus->read(new_bus, phyaddr, MII_BMSR);
		if (err == -EBUSY || !err || err == 0xffff) {
			dev_warn(dev, "Invalid PHY address read from dtsi: %d",
				 phyaddr);
			new_bus->phy_mask = mdio_bus_data->phy_mask;
		} else {
			new_bus->phy_mask = ~(1 << phyaddr);
			skip_phy_detect = 1;
		}
	}

	new_bus->parent = priv->device;

	if (mdio_node)
		err = of_mdiobus_register(new_bus, mdio_node);
	else
		err = mdiobus_register(new_bus);
	if (err != 0) {
		dev_err(dev, "Cannot register the MDIO bus\n");
		goto bus_register_fail;
	}

	if (skip_phy_detect) {
		phydev = mdiobus_get_phy(new_bus, phyaddr);
		if (!phydev || phydev->phy_id == 0xffff) {
			dev_err(dev, "Cannot attach phy addr %d from dtsi",
				phyaddr);
		} else {
			priv->plat->phy_addr = phyaddr;
			priv->phydev = phydev;
			phy_attached_info(phydev);
			goto bus_register_done;
		}
	}

	if (priv->plat->phy_node || mdio_node)
		goto bus_register_done;

	found = 0;
	for (addr = 0; addr < PHY_MAX_ADDR; addr++) {
		phydev = mdiobus_get_phy(new_bus, addr);

		if (!phydev || phydev->phy_id == 0xffff)
			continue;

		/*
		 * If an IRQ was provided to be assigned after
		 * the bus probe, do it here.
		 */
		if (!mdio_bus_data->irqs &&
		    (mdio_bus_data->probed_phy_irq > 0)) {
			new_bus->irq[addr] = mdio_bus_data->probed_phy_irq;
			phydev->irq = mdio_bus_data->probed_phy_irq;
		}

		/*
		 * If we're going to bind the MAC to this PHY bus,
		 * and no PHY number was provided to the MAC,
		 * use the one probed here.
		 */
		if (priv->plat->phy_addr == -1)
			priv->plat->phy_addr = addr;

		priv->phydev = phydev;
		phy_attached_info(phydev);
		found = 1;
		break;
	}

	if (!found && !mdio_node) {
		dev_warn(dev, "No PHY found\n");
		mdiobus_unregister(new_bus);
		mdiobus_free(new_bus);
		return -ENODEV;
	}

bus_register_done:
	priv->mii = new_bus;

	return 0;

bus_register_fail:
	mdiobus_free(new_bus);
	return err;
}

/**
 * stmmac_mdio_unregister
 * @ndev: net device structure
 * Description: it unregisters the MII bus
 */
int stmmac_mdio_unregister(struct net_device *ndev)
{
	struct stmmac_priv *priv = netdev_priv(ndev);

	if (!priv->mii)
		return 0;

	mdiobus_unregister(priv->mii);
	priv->mii->priv = NULL;
	mdiobus_free(priv->mii);
	priv->mii = NULL;

	return 0;
}
