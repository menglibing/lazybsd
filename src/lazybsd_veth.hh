/**
 * @file lazybsd_veth.hh
 * @author mengdemao (mengdemao19951021@gmail.com)
 * @brief 
 * @version 0.1
 * @date 2024-04-30
 * 
 * @copyright Copyright (c) 2024
 * 
 */
#include <lazybsd_veth.h>

#ifndef LAZYBSD_VETH_HH
#define LAZYBSD_VETH_HH

#include <cstdint>

struct lazybsd_port_cfg;
void *lazybsd_veth_attach(struct lazybsd_port_cfg *cfg);
int lazybsd_veth_detach(void *arg);

void *lazybsd_mbuf_gethdr(void *pkt, uint16_t total, void *data,
    uint16_t len, uint8_t rx_csum);
void *lazybsd_mbuf_get(void *p, void *m, void *data, uint16_t len);
void lazybsd_mbuf_free(void *m);

int lazybsd_mbuf_copydata(void *m, void *data, int off, int len);
int lazybsd_next_mbuf(void **mbuf_bsd, void **data, unsigned *len);
void* lazybsd_mbuf_mtod(void* bsd_mbuf);
void* lazybsd_rte_frm_extcl(void* mbuf);

struct lazybsd_tx_offload;
void lazybsd_mbuf_tx_offload(void *m, struct lazybsd_tx_offload *offload);

void lazybsd_veth_process_packet(void *arg, void *m);

void *lazybsd_veth_softc_to_hostc(void *softc);

void lazybsd_mbuf_set_vlan_info(void *hdr, uint16_t vlan_tci);

#endif // LAZYBSD_VETH_HH
