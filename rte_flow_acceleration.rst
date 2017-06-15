HW acceleration applied to OVS+DPDK via RTE_FLOW
================================================

**Note**: This document is a WIP as is the implementation.

The drawing below show the architecture of OVS+DPDK. The main addition made is the **HW-offload** module, which handle the Mega flow offload to HW. The offload is transparent to the user, if the HW-offload module detect that a newly created Mega flow can be offloaded it will be and if not it will just we handled by the normal OVS+DPDK datapath.

.. image:: https://raw.githubusercontent.com/napatech/ovs/master/rte_flow.png


The following describe in a bit more details the what happens during normal operation:

- In dpif-netdev (3) a lcore is reading from each in-port netdev instance. It receives a burst of 32 packets, which is then sorted in pre-classified batches and un-classified batches.
- The un-classified batches are handled normally.
- On a match returned from the Open Flow tables, an entry in Mega flow cache is added/updated and this flow is then propagated to HW for possible offload. This is done in the hw-offload module (4), by calling netdev-dpdk (netdev_class) config function (2). If it succeeds, a hw-offloaded flow cache is updated with the new flow. If not succeeded, it just return and normal sw handling applies.
- When an netdev-class instance receives a hw-offload request (2), it will try to offload the specified Mega flow to the NIC using RTE_FLOW extension to DPDK. It uses RTE_FLOW_ACTION_TYPE_MARK to add a flow-unique pre-classification metadata. If RTE_FLOW_ACTION_TYPE_COUNT is supported (probed at creation of port), it is then used to request hw for flow statistics to be used to update OVS flow statistics at a certain interval.
- Handling of the pre-classified batches, received by dpif-netdev (3) on port read, is consulting the hw-offload flow cache for a match for statistics purposes, if needed (no support for RTE_FLOW_ACTION_TYPE_COUNT), otherwise transmit the batch based on the flow metadata, containing the output port.
- Flow statistics are updated on a certain interval. It may be hw-offloaded by supporting the RTE_FLOW_ACTION_TYPE_COUNT, of sw handled in hw-offload module (4). The statistics are updated using the OVS UFID.
- When a Mega flow item is deleted, the associated hw-offloaded flow is then also deleted.

Next step
---------
- Improved batch handling in dpif-netdev (3) when transmitting pre-classified batches, using specific queues to ensure entire batches to same destination.
- Introducing a virtual port concept in NIC to enable East-West hw-offload.
- Contiguous batched mbufs.

Preliminary performance results
-------------------------------
Approx. 20% acceleration measured using vanilla mbufs. However, using contiguous batched mbufs a performance improvement of 200-300% has been achieved.

