#include <config.h>
#include "netdev-provider.h"
#include "openvswitch/match.h"
#include "openvswitch/vlog.h"
#include "dp-packet.h"
#include "cmap.h"
#include "netdev-hw-offload.h"

VLOG_DEFINE_THIS_MODULE(netdev_hw_offload);


struct netdev_ufid_map {
	struct cmap_node cmap_node;
	ovs_u128 ufid;
	void    *hw_flow_id;
	uint16_t node_tbl_idx;
	/* Only for netdev providers not able to deliver flow statistics */
	uint64_t packets;
	uint64_t bytes;
};

static struct netdev_ufid_map *ufid_node_tbl[0x10000];
static uint16_t next_node_idx;
static struct ovs_mutex ufid_node_mutex = OVS_MUTEX_INITIALIZER;



static struct netdev_ufid_map *get_ufid_node(const struct cmap *cmap, const ovs_u128 *ufid)
{
	const struct cmap_node *node;
	struct netdev_ufid_map *ufid_node = NULL;

	node = cmap_find(cmap, ufid->u32[0]);

	CMAP_NODE_FOR_EACH(ufid_node, cmap_node, node) {
		VLOG_DBG("ufid map %08x%08x, %lx\n", (unsigned int)ufid_node->ufid.u64.hi,
		        (unsigned int)ufid_node->ufid.u64.lo, (long unsigned int)ufid_node->hw_flow_id);
		if (ufid_node->ufid.u64.hi == ufid->u64.hi && ufid_node->ufid.u64.lo == ufid->u64.lo) {
			/* Found flow in cache. */
			return ufid_node;
		}
	}
	return NULL;
}


void netdev_update_flow_stats(struct netdev_rxq *rxq, struct dp_packet_batch *batch)
{
    uint32_t flow_id, pre_id;
    uint32_t i;
    struct netdev_ufid_map *ufid_node;

    if (rxq->netdev->flow_stat_hw_support == 1) return;

    pre_id = (uint32_t)-1;
    ufid_node = NULL;

    ovs_mutex_lock(&ufid_node_mutex);
    for (i = 0; i < batch->count; i++) {
        flow_id = dp_packet_get_pre_classified_flow_id(batch->packets[i]);

        if (pre_id != flow_id) {
            ufid_node = ufid_node_tbl[(uint16_t)(flow_id >> 16)];
            pre_id = flow_id;
        }
        if (ufid_node != NULL) {
            ufid_node->packets++;
            ufid_node->bytes += dp_packet_size(batch->packets[i]);
        }
    }
    ovs_mutex_unlock(&ufid_node_mutex);
}


int netdev_hw_get_stats_from_dev(struct netdev_rxq *rxq, struct netdev_flow_stats **aflow_stats)
{
	struct netdev_flow_stats *flow_stats = NULL;
	struct netdev_ufid_map *ufid_node = NULL;
	uint32_t i;
	bool hw_support = ((rxq->netdev->flow_stat_hw_support == 1) && netdev_get_flow_stats_supported(rxq));

	*aflow_stats = NULL;
	/* Collect all entries in cmap */
	size_t num = cmap_count(&rxq->netdev->ufid_map);
	if (num == 0) return 0;

	flow_stats = xcalloc(1, sizeof(struct netdev_flow_stats) + num * sizeof(struct flow_stat_elem));
	if (flow_stats == NULL) return -1;

	flow_stats->num = num;
	i = 0;
	CMAP_FOR_EACH(ufid_node, cmap_node, &rxq->netdev->ufid_map) {
		flow_stats->flow_stat[i].flow_id = ufid_node->hw_flow_id;
		flow_stats->flow_stat[i].ufid = ufid_node->ufid;

		if (!hw_support) {
		    flow_stats->flow_stat[i].bytes = ufid_node->bytes;
		    ufid_node->bytes = 0;
            flow_stats->flow_stat[i].packets = ufid_node->packets;
            ufid_node->packets = 0;
		}

		i++;
		if (i > flow_stats->num) break;
	}

	if (flow_stats->num != i) {
		VLOG_WARN("WARNING :: flow_stats->num = %u, i = %i\n", (unsigned int)flow_stats->num, i);
		flow_stats->num = i;
	}

	/* Get all HW flow stats for this device */
	if (hw_support) {
	    int err;
        if ((err = netdev_get_flow_stats(rxq, flow_stats)) != 0) {
            free(flow_stats);
            return err;
        }
	}

	*aflow_stats = flow_stats;
	return 0;
}

int netdev_hw_free_stats_from_dev(struct netdev_rxq *rxq OVS_UNUSED, struct netdev_flow_stats **aflow_stats)
{
	if (aflow_stats == NULL || *aflow_stats) return 0;
	free(*aflow_stats);
	return 0;
}



int netdev_try_hw_flow_offload(struct netdev *netdev, uint32_t hw_port_id, struct match *match, const ovs_u128 *ufid,
		const struct nlattr *actions, size_t actions_len)
{
	int delete_match = 0;
	struct netdev_ufid_map *ufid_node = NULL;

	if (netdev == NULL || !netdev_has_hw_flow_offload(netdev)) return 0;

	ovs_mutex_lock(&netdev->mutex);

	ufid_node = get_ufid_node(&netdev->ufid_map, ufid);

	if (match == NULL) {
		/* Used for deletion of flow */
		match = xcalloc(1, sizeof(struct match));
		if (!match) {
			ovs_mutex_unlock(&netdev->mutex);
			return -1;
		}
		delete_match = 1;
	}

	uint64_t flow_handle;
	int flow_stat_support = netdev->flow_stat_hw_support;

	if (ufid_node != NULL) {
		flow_handle = (uint64_t)ufid_node->hw_flow_id;
	} else {
		flow_handle = (uint64_t)-1;
	}

	ovs_mutex_lock(&ufid_node_mutex);

	/* Find next empty slot for next time ufid node lookup idx. */
    uint16_t idx_for_next_node = (uint16_t)(next_node_idx + 1);
    while ((ufid_node_tbl[idx_for_next_node] != NULL) && (idx_for_next_node != next_node_idx)) {
        idx_for_next_node++;
    }
    if (ufid_node_tbl[idx_for_next_node]) {
        /* ufid node lookup table full */
        if (match && delete_match)
            free(match);
        ovs_mutex_unlock(&ufid_node_mutex);
        ovs_mutex_unlock(&netdev->mutex);
        return 0;
    }


    netdev_hw_flow_offload(netdev, match, hw_port_id, actions, actions_len, &flow_stat_support, next_node_idx, &flow_handle);

	if (flow_handle == (uint64_t)-1) {
		/* HW action failed or not supported by this device */
		if (ufid_node) {
			if (delete_match)
				VLOG_ERR("deletion of flow %08x%08x in HW failed!", (unsigned int)ufid_node->ufid.u64.hi,
				        (unsigned int)ufid_node->ufid.u64.lo);
			VLOG_ERR("ERROR flow removed after failed in HW\n");
			ufid_node_tbl[ufid_node->node_tbl_idx] = NULL;
			cmap_remove(&netdev->ufid_map, &ufid_node->cmap_node, ufid->u32[0]);
		}
		VLOG_DBG("Flow could not be HW accelerated or not supported by this device\n");
	} else {
		/* HW action succeeded */
		if (delete_match) {
			if (ufid_node != NULL) {
	            ufid_node_tbl[ufid_node->node_tbl_idx] = NULL;
				cmap_remove(&netdev->ufid_map, &ufid_node->cmap_node, ufid->u32[0]);

				VLOG_DBG("removed from ufid map %08x%08x, %lx\n", (unsigned int)ufid_node->ufid.u64.hi,
				        (unsigned int)ufid_node->ufid.u64.lo, flow_handle);
			}
		} else {
			VLOG_DBG("Flow added to HW with flow handle: %lx\n", flow_handle);

			if (ufid_node == NULL) {
				/* Create new entry */
				ufid_node = xmalloc(sizeof(struct netdev_ufid_map));
				cmap_insert(&netdev->ufid_map, &ufid_node->cmap_node, ufid->u32[0]);
			} /* otherwise reuse */
			ufid_node->ufid = *ufid;
			ufid_node->hw_flow_id = (void *)flow_handle;
			/* Add unique flow id */
			ufid_node->node_tbl_idx = next_node_idx;

			ufid_node_tbl[next_node_idx] = ufid_node;
			next_node_idx = idx_for_next_node;

			VLOG_DBG("insert into ufid map %08x%08x, %lx\n", (unsigned int)ufid_node->ufid.u64.hi,
			        (unsigned int)ufid_node->ufid.u64.lo, (long unsigned int)ufid_node->hw_flow_id);
		}

		/* Confirm support for flow statistics support by NIC */
		if (netdev->flow_stat_hw_support < 0)
		    netdev->flow_stat_hw_support = flow_stat_support;
	}
    ovs_mutex_unlock(&ufid_node_mutex);

	if (match && delete_match)
		free(match);

	ovs_mutex_unlock(&netdev->mutex);
	return 0;
}
