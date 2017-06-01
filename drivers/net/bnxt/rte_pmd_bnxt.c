/*-
 *   BSD LICENSE
 *
 *   Copyright(c) 2017 Broadcom Limited.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Broadcom Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <inttypes.h>
#include <stdbool.h>
#include <unistd.h>

#include <rte_dev.h>
#include <rte_ethdev.h>
#include <rte_malloc.h>
#include <rte_cycles.h>
#include <rte_byteorder.h>

#include "bnxt.h"
#include "bnxt_filter.h"
#include "bnxt_hwrm.h"
#include "bnxt_vnic.h"
#include "rte_pmd_bnxt.h"
#include "hsi_struct_def_dpdk.h"

int rte_pmd_bnxt_set_tx_loopback(uint8_t port, uint8_t on)
{
	struct rte_eth_dev *eth_dev;
	struct bnxt *bp;
	int rc;

	RTE_ETH_VALID_PORTID_OR_ERR_RET(port, -ENODEV);

	if (on > 1)
		return -EINVAL;

	eth_dev = &rte_eth_devices[port];
	if (!is_bnxt_supported(eth_dev))
		return -ENOTSUP;

	bp = (struct bnxt *)eth_dev->data->dev_private;

	if (!BNXT_PF(bp)) {
		RTE_LOG(ERR, PMD,
			"Attempt to set Tx loopback on non-PF port %d!\n",
			port);
		return -ENOTSUP;
	}

	if (on)
		bp->pf.evb_mode = BNXT_EVB_MODE_VEB;
	else
		bp->pf.evb_mode = BNXT_EVB_MODE_VEPA;

	rc = bnxt_hwrm_pf_evb_mode(bp);

	return rc;
}

static void
rte_pmd_bnxt_set_all_queues_drop_en_cb(struct bnxt_vnic_info *vnic, void *onptr)
{
	uint8_t *on = onptr;
	vnic->bd_stall = !(*on);
}

int rte_pmd_bnxt_set_all_queues_drop_en(uint8_t port, uint8_t on)
{
	struct rte_eth_dev *eth_dev;
	struct bnxt *bp;
	uint32_t i;
	int rc;

	RTE_ETH_VALID_PORTID_OR_ERR_RET(port, -ENODEV);

	if (on > 1)
		return -EINVAL;

	eth_dev = &rte_eth_devices[port];
	if (!is_bnxt_supported(eth_dev))
		return -ENOTSUP;

	bp = (struct bnxt *)eth_dev->data->dev_private;

	if (!BNXT_PF(bp)) {
		RTE_LOG(ERR, PMD,
			"Attempt to set all queues drop on non-PF port!\n");
		return -ENOTSUP;
	}

	if (bp->vnic_info == NULL)
		return -ENODEV;

	/* Stall PF */
	for (i = 0; i < bp->nr_vnics; i++) {
		bp->vnic_info[i].bd_stall = !on;
		rc = bnxt_hwrm_vnic_cfg(bp, &bp->vnic_info[i]);
		if (rc) {
			RTE_LOG(ERR, PMD, "Failed to update PF VNIC %d.\n", i);
			return rc;
		}
	}

	/* Stall all active VFs */
	for (i = 0; i < bp->pf.active_vfs; i++) {
		rc = bnxt_hwrm_func_vf_vnic_query_and_config(bp, i,
				rte_pmd_bnxt_set_all_queues_drop_en_cb, &on,
				bnxt_hwrm_vnic_cfg);
		if (rc) {
			RTE_LOG(ERR, PMD, "Failed to update VF VNIC %d.\n", i);
			break;
		}
	}

	return rc;
}

int rte_pmd_bnxt_set_vf_mac_addr(uint8_t port, uint16_t vf,
				struct ether_addr *mac_addr)
{
	struct rte_eth_dev *dev;
	struct rte_eth_dev_info dev_info;
	struct bnxt *bp;
	int rc;

	RTE_ETH_VALID_PORTID_OR_ERR_RET(port, -ENODEV);

	dev = &rte_eth_devices[port];
	if (!is_bnxt_supported(dev))
		return -ENOTSUP;

	rte_eth_dev_info_get(port, &dev_info);
	bp = (struct bnxt *)dev->data->dev_private;

	if (vf >= dev_info.max_vfs || mac_addr == NULL)
		return -EINVAL;

	if (!BNXT_PF(bp)) {
		RTE_LOG(ERR, PMD,
			"Attempt to set VF %d mac address on non-PF port %d!\n",
			vf, port);
		return -ENOTSUP;
	}

	rc = bnxt_hwrm_func_vf_mac(bp, vf, (uint8_t *)mac_addr);

	return rc;
}

int rte_pmd_bnxt_set_vf_rate_limit(uint8_t port, uint16_t vf,
				uint16_t tx_rate, uint64_t q_msk)
{
	struct rte_eth_dev *eth_dev;
	struct rte_eth_dev_info dev_info;
	struct bnxt *bp;
	uint16_t tot_rate = 0;
	uint64_t idx;
	int rc;

	RTE_ETH_VALID_PORTID_OR_ERR_RET(port, -ENODEV);

	eth_dev = &rte_eth_devices[port];
	if (!is_bnxt_supported(eth_dev))
		return -ENOTSUP;

	rte_eth_dev_info_get(port, &dev_info);
	bp = (struct bnxt *)eth_dev->data->dev_private;

	if (!bp->pf.active_vfs)
		return -EINVAL;

	if (vf >= bp->pf.max_vfs)
		return -EINVAL;

	/* Add up the per queue BW and configure MAX BW of the VF */
	for (idx = 0; idx < 64; idx++) {
		if ((1ULL << idx) & q_msk)
			tot_rate += tx_rate;
	}

	/* Requested BW can't be greater than link speed */
	if (tot_rate > eth_dev->data->dev_link.link_speed) {
		RTE_LOG(ERR, PMD, "Rate > Link speed. Set to %d\n", tot_rate);
		return -EINVAL;
	}

	/* Requested BW already configured */
	if (tot_rate == bp->pf.vf_info[vf].max_tx_rate)
		return 0;

	rc = bnxt_hwrm_func_bw_cfg(bp, vf, tot_rate,
				HWRM_FUNC_CFG_INPUT_ENABLES_MAX_BW);

	if (!rc)
		bp->pf.vf_info[vf].max_tx_rate = tot_rate;

	return rc;
}

int rte_pmd_bnxt_set_vf_mac_anti_spoof(uint8_t port, uint16_t vf, uint8_t on)
{
	struct rte_eth_dev_info dev_info;
	struct rte_eth_dev *dev;
	uint32_t func_flags;
	struct bnxt *bp;
	int rc;

	RTE_ETH_VALID_PORTID_OR_ERR_RET(port, -ENODEV);

	if (on > 1)
		return -EINVAL;

	dev = &rte_eth_devices[port];
	if (!is_bnxt_supported(dev))
		return -ENOTSUP;

	rte_eth_dev_info_get(port, &dev_info);
	bp = (struct bnxt *)dev->data->dev_private;

	if (!BNXT_PF(bp)) {
		RTE_LOG(ERR, PMD,
			"Attempt to set mac spoof on non-PF port %d!\n", port);
		return -EINVAL;
	}

	if (vf >= dev_info.max_vfs)
		return -EINVAL;

	/* Prev setting same as new setting. */
	if (on == bp->pf.vf_info[vf].mac_spoof_en)
		return 0;

	func_flags = bp->pf.vf_info[vf].func_cfg_flags;

	if (on)
		func_flags |=
			HWRM_FUNC_CFG_INPUT_FLAGS_SRC_MAC_ADDR_CHECK_ENABLE;
	else
		func_flags |=
			HWRM_FUNC_CFG_INPUT_FLAGS_SRC_MAC_ADDR_CHECK_DISABLE;

	bp->pf.vf_info[vf].func_cfg_flags = func_flags;

	rc = bnxt_hwrm_func_cfg_vf_set_flags(bp, vf);
	if (!rc)
		bp->pf.vf_info[vf].mac_spoof_en = on;

	return rc;
}

int rte_pmd_bnxt_set_vf_vlan_anti_spoof(uint8_t port, uint16_t vf, uint8_t on)
{
	struct rte_eth_dev_info dev_info;
	struct rte_eth_dev *dev;
	struct bnxt *bp;
	int rc;
	int dflt_vnic;
	struct bnxt_vnic_info vnic;

	RTE_ETH_VALID_PORTID_OR_ERR_RET(port, -ENODEV);

	if (on > 1)
		return -EINVAL;

	dev = &rte_eth_devices[port];
	if (!is_bnxt_supported(dev))
		return -ENOTSUP;

	rte_eth_dev_info_get(port, &dev_info);
	bp = (struct bnxt *)dev->data->dev_private;

	if (!BNXT_PF(bp)) {
		RTE_LOG(ERR, PMD,
			"Attempt to set mac spoof on non-PF port %d!\n", port);
		return -EINVAL;
	}

	if (vf >= dev_info.max_vfs)
		return -EINVAL;

	rc = bnxt_hwrm_func_cfg_vf_set_vlan_anti_spoof(bp, vf, on);
	if (!rc) {
		bp->pf.vf_info[vf].vlan_spoof_en = on;
		if (on) {
			dflt_vnic = bnxt_hwrm_func_qcfg_vf_dflt_vnic_id(bp, vf);
			if (dflt_vnic < 0) {
				/*
				 * This simply indicates there's no driver
				 * loaded.  This is not an error.
				 */
				RTE_LOG(ERR, PMD,
				      "Unable to get default VNIC for VF %d\n",
					vf);
			} else {
				vnic.fw_vnic_id = dflt_vnic;
				if (bnxt_hwrm_vnic_qcfg(bp,
					&vnic, bp->pf.first_vf_id + vf) == 0) {
					if (bnxt_hwrm_cfa_l2_set_rx_mask(bp,
					   &vnic, bp->pf.vf_info[vf].vlan_count,
					   bp->pf.vf_info[vf].vlan_table))
						rc = -1;
				}
			}
		}
	} else {
		RTE_LOG(ERR, PMD, "Failed to update VF VNIC %d.\n", vf);
	}

	return rc;
}

static void
rte_pmd_bnxt_set_vf_vlan_stripq_cb(struct bnxt_vnic_info *vnic, void *onptr)
{
	uint8_t *on = onptr;
	vnic->vlan_strip = *on;
}

int
rte_pmd_bnxt_set_vf_vlan_stripq(uint8_t port, uint16_t vf, uint8_t on)
{
	struct rte_eth_dev *dev;
	struct rte_eth_dev_info dev_info;
	struct bnxt *bp;
	int rc;

	RTE_ETH_VALID_PORTID_OR_ERR_RET(port, -ENODEV);

	dev = &rte_eth_devices[port];
	if (!is_bnxt_supported(dev))
		return -ENOTSUP;

	rte_eth_dev_info_get(port, &dev_info);
	bp = (struct bnxt *)dev->data->dev_private;

	if (vf >= dev_info.max_vfs)
		return -EINVAL;

	if (!BNXT_PF(bp)) {
		RTE_LOG(ERR, PMD,
			"Attempt to set VF %d stripq on non-PF port %d!\n",
			vf, port);
		return -ENOTSUP;
	}

	rc = bnxt_hwrm_func_vf_vnic_query_and_config(bp, vf,
				rte_pmd_bnxt_set_vf_vlan_stripq_cb, &on,
				bnxt_hwrm_vnic_cfg);
	if (rc)
		RTE_LOG(ERR, PMD, "Failed to update VF VNIC %d.\n", vf);

	return rc;
}

int rte_pmd_bnxt_set_vf_vlan_filter(uint8_t port, uint16_t vlan,
				    uint64_t vf_mask, uint8_t vlan_on)
{
	struct bnxt_vlan_table_entry *ve;
	struct rte_eth_dev *dev;
	struct bnxt *bp;
	uint16_t cnt;
	int rc = 0;
	int i, j;

	RTE_ETH_VALID_PORTID_OR_ERR_RET(port, -ENODEV);

	dev = &rte_eth_devices[port];
	if (!is_bnxt_supported(dev))
		return -ENOTSUP;

	bp = (struct bnxt *)dev->data->dev_private;
	if (!bp->pf.vf_info)
		return -EINVAL;

	for (i = 0; vf_mask; i++, vf_mask >>= 1) {
		cnt = bp->pf.vf_info[i].vlan_count;
		if (vf_mask & 1) {
			if (bp->pf.vf_info[i].vlan_table == NULL) {
				rc = -1;
				continue;
			}
			if (vlan_on) {
				/* First, search for a duplicate... */
				for (j = 0; j < cnt; j++) {
					if (rte_be_to_cpu_16(
					 bp->pf.vf_info[i].vlan_table[j].vid) ==
					    vlan)
						break;
				}
				if (j == cnt) {
					/* Now check that there's space */
					if (cnt == getpagesize() /
					 sizeof(struct bnxt_vlan_table_entry)) {
						RTE_LOG(ERR, PMD,
						  "VF %d VLAN table is full\n",
						  i);
						RTE_LOG(ERR, PMD,
							"cannot add VLAN %u\n",
							vlan);
						rc = -1;
						continue;
					}

					cnt = bp->pf.vf_info[i].vlan_count++;
					/*
					 * And finally, add to the
					 * end of the table
					 */
					ve = &bp->pf.vf_info[i].vlan_table[cnt];
					/* TODO: Hardcoded TPID */
					ve->tpid = rte_cpu_to_be_16(0x8100);
					ve->vid = rte_cpu_to_be_16(vlan);
				}
			} else {
				for (j = 0; cnt; j++) {
					if (rte_be_to_cpu_16(
					bp->pf.vf_info[i].vlan_table[j].vid) !=
					    vlan)
						continue;
					memmove(
					 &bp->pf.vf_info[i].vlan_table[j],
					 &bp->pf.vf_info[i].vlan_table[j + 1],
					 getpagesize() -
					 ((j + 1) *
					 sizeof(struct bnxt_vlan_table_entry)));
					j--;
					cnt = bp->pf.vf_info[i].vlan_count--;
				}
			}
			rte_pmd_bnxt_set_vf_vlan_anti_spoof(dev->data->port_id,
					 i, bp->pf.vf_info[i].vlan_spoof_en);
		}
	}

	return rc;
}
