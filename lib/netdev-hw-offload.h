#ifndef NETDEV_HW_OFFLOAD_H
#define NETDEV_HW_OFFLOAD_H 1

#ifdef  __cplusplus
extern "C" {
#endif


#define HW_PORT_MASK         0xff
#define MAX_HW_PORT_CNT      0xff
#define INVALID_HW_PORT_ID   MAX_HW_PORT_CNT

#define FLOW_ID_ODP_PORT_BIT   0x00008000
#define FLOW_ID_PORT_MASK      0x00007fff


void netdev_update_flow_stats(struct netdev_rxq *rxq, struct dp_packet_batch *batch);

int netdev_try_hw_flow_offload(struct netdev *netdev, uint32_t hw_port_id,
                                struct match *match, const ovs_u128 *ufid,
                                const struct nlattr *actions, size_t actions_len);

int netdev_hw_get_stats_from_dev(struct netdev_rxq *rxq, struct netdev_flow_stats **aflow_stats);
int netdev_hw_free_stats_from_dev(struct netdev_rxq *rxq, struct netdev_flow_stats **aflow_stats);


#ifdef  __cplusplus
}
#endif

#endif /* netdev-hw-offload.h */
