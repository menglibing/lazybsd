/**
 * @file lazybsd_veth.cc
 * @author mengdemao (mengdemao19951021@163.com)
 * @brief
 * @version 0.1
 * @date 2024-03-11
 *
 * @copyright Copyright (c) 2024
 *
 */
#include <lazybsd.h>
#include <sys/ctype.h>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/socketvar.h>
#include <sys/module.h>
#include <sys/kernel.h>
#include <sys/proc.h>
#include <sys/kthread.h>
#include <sys/sched.h>
#include <sys/sockio.h>
#include <sys/ck.h>

#include <net/if.h>
#include <net/if_var.h>
#include <net/if_types.h>
#include <net/ethernet.h>
#include <net/if_arp.h>
#include <net/if_tap.h>
#include <net/if_dl.h>
#include <net/route.h>
#include <net/route/route_ctl.h>

#include <netinet/in.h>
#include <netinet/in_var.h>
#include <netinet6/nd6.h>

#include <machine/atomic.h>

#include "lazybsd_dpdk.h"
#include "lazybsd_veth.h"

struct lazybsd_dpdk_if_context;
struct lazybsd_veth_softc {
    struct ifnet *ifp;
    uint8_t mac[ETHER_ADDR_LEN];
    char host_ifname[IF_NAMESIZE];

    in_addr_t ip;
    in_addr_t netmask;
    in_addr_t broadcast;
    in_addr_t gateway;

    uint8_t nb_vip;
    in_addr_t vip[VIP_MAX_NUM];

#ifdef INET6
    struct in6_addr ip6;
    struct in6_addr gateway6;
    uint8_t prefix_length;

    uint8_t nb_vip6;
    uint8_t vip_prefix_length;
    struct in6_addr vip6[VIP_MAX_NUM];
#endif /* INET6 */

    struct lazybsd_dpdk_if_context *host_ctx;
};

static void
lazybsd_veth_init(void *arg)
{
    struct lazybsd_veth_softc *sc = arg;
    struct ifnet *ifp = sc->ifp;

    ifp->if_drv_flags |= IFF_DRV_RUNNING;
    ifp->if_drv_flags &= ~IFF_DRV_OACTIVE;
}

static void
lazybsd_veth_start(struct ifnet *ifp)
{
    /* nothing to do */
}

static void
lazybsd_veth_stop(struct lazybsd_veth_softc *sc)
{
    struct ifnet *ifp = sc->ifp;

    ifp->if_drv_flags &= ~(IFF_DRV_RUNNING|IFF_DRV_OACTIVE);
}

static int
lazybsd_veth_ioctl(struct ifnet *ifp, u_long cmd, caddr_t data)
{
    int error = 0;
    struct lazybsd_veth_softc *sc = ifp->if_softc;

    switch (cmd) {
    case SIOCSIFFLAGS:
        if (ifp->if_flags & IFF_UP) {
            lazybsd_veth_init(sc);
        } else if (ifp->if_drv_flags & IFF_DRV_RUNNING)
            lazybsd_veth_stop(sc);
        break;
    default:
        error = ether_ioctl(ifp, cmd, data);
        break;
    }

    return (error);
}

int
lazybsd_mbuf_copydata(void *m, void *data, int off, int len)
{
    int ret;
    struct mbuf *mb = (struct mbuf *)m;

    if (off + len > mb->m_pkthdr.len) {
        return -1;
    }

    m_copydata(mb, off, len, data);

    return 0;
}

void
lazybsd_mbuf_tx_offload(void *m, struct lazybsd_tx_offload *offload)
{
    struct mbuf *mb = (struct mbuf *)m;
    if (mb->m_pkthdr.csum_flags & CSUM_IP) {
        offload->ip_csum = 1;
    }

    if (mb->m_pkthdr.csum_flags & CSUM_TCP) {
        offload->tcp_csum = 1;
    }

    if (mb->m_pkthdr.csum_flags & CSUM_UDP) {
        offload->udp_csum = 1;
    }

    if (mb->m_pkthdr.csum_flags & CSUM_SCTP) {
        offload->sctp_csum = 1;
    }

    if (mb->m_pkthdr.csum_flags & CSUM_TSO) {
        offload->tso_seg_size = mb->m_pkthdr.tso_segsz;
    }
}

void
lazybsd_mbuf_free(void *m)
{
    m_freem((struct mbuf *)m);
}

static void
lazybsd_mbuf_ext_free(struct mbuf *m)
{
    lazybsd_dpdk_pktmbuf_free(lazybsd_rte_frm_extcl(m));
}

int lazybsd_zc_mbuf_get(struct lazybsd_zc_mbuf *m, int len) {
    struct mbuf *mb;

    if (m == NULL) {
        return -1;
    }

    mb = m_getm2(NULL, max(len, 1), M_WAITOK, MT_DATA, 0);
    if (mb == NULL) {
        return -1;
    }

    m->bsd_mbuf = m->bsd_mbuf_off = mb;
    m->off = 0;
    m->len = len;

    return 0;
}

int
lazybsd_zc_mbuf_write(struct lazybsd_zc_mbuf *zm, const char *data, int len)
{
    int ret, length, progress = 0;
    struct mbuf *m, *mb;

    if (zm == NULL) {
        return -1;
    }
    m = (struct mbuf *)zm->bsd_mbuf_off;

    if (zm->off + len > zm->len) {
        return -1;
    }

    for (mb = m; mb != NULL; mb = mb->m_next) {
        length = min(M_TRAILINGSPACE(mb), len - progress);
        bcopy(data + progress, mtod(mb, char *) + mb->m_len, length);

        mb->m_len += length;
        progress += length;
        if (len == progress) {
            break;
        }
        //if (flags & M_PKTHDR)
        //    m->m_pkthdr.len += length;
    }
    zm->off += len;
    zm->bsd_mbuf_off = mb;

    return len;
}

int
lazybsd_zc_mbuf_read(struct lazybsd_zc_mbuf *m, const char *data, int len)
{
    // DOTO: Support read zero copy
    return 0;
}

void *
lazybsd_mbuf_gethdr(void *pkt, uint16_t total, void *data,
    uint16_t len, uint8_t rx_csum)
{
    struct mbuf *m = m_gethdr(M_NOWAIT, MT_DATA);
    if (m == NULL) {
        return NULL;
    }

    if (m_pkthdr_init(m, M_NOWAIT) != 0) {
        return NULL;
    }

    m_extadd(m, data, len, lazybsd_mbuf_ext_free, pkt, NULL, 0, EXT_DISPOSABLE);

    m->m_pkthdr.len = total;
    m->m_len = len;
    m->m_next = NULL;
    m->m_nextpkt = NULL;

    if (rx_csum) {
        m->m_pkthdr.csum_flags = CSUM_IP_CHECKED | CSUM_IP_VALID |
            CSUM_DATA_VALID | CSUM_PSEUDO_HDR;
        m->m_pkthdr.csum_data = 0xffff;
    }
    return (void *)m;
}

void *
lazybsd_mbuf_get(void *p, void *m, void *data, uint16_t len)
{
    struct mbuf *prev = (struct mbuf *)p;
    struct mbuf *mb = m_get(M_NOWAIT, MT_DATA);

    if (mb == NULL) {
        return NULL;
    }

    m_extadd(mb, data, len, lazybsd_mbuf_ext_free, m, NULL, 0, EXT_DISPOSABLE);

    mb->m_next = NULL;
    mb->m_nextpkt = NULL;
    mb->m_len = len;

    if (prev != NULL) {
        prev->m_next = mb;
    }

    return (void *)mb;
}

void
lazybsd_veth_process_packet(void *arg, void *m)
{
    struct ifnet *ifp = (struct ifnet *)arg;
    struct mbuf *mb = (struct mbuf *)m;

    mb->m_pkthdr.rcvif = ifp;

    ifp->if_input(ifp, mb);
}

static int
lazybsd_veth_transmit(struct ifnet *ifp, struct mbuf *m)
{
    struct lazybsd_veth_softc *sc = (struct lazybsd_veth_softc *)ifp->if_softc;
    return lazybsd_dpdk_if_send(sc->host_ctx, (void*)m, m->m_pkthdr.len);
}

static void
lazybsd_veth_qflush(struct ifnet *ifp)
{

}

static int
lazybsd_veth_setaddr(struct lazybsd_veth_softc *sc)
{
    struct in_aliasreq req;
    bzero(&req, sizeof req);
    strcpy(req.ifra_name, sc->ifp->if_dname);

    struct sockaddr_in sa;
    bzero(&sa, sizeof(sa));
    sa.sin_len = sizeof(sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = sc->ip;
    bcopy(&sa, &req.ifra_addr, sizeof(sa));

    sa.sin_addr.s_addr = sc->netmask;
    bcopy(&sa, &req.ifra_mask, sizeof(sa));

    sa.sin_addr.s_addr = sc->broadcast;
    bcopy(&sa, &req.ifra_broadaddr, sizeof(sa));

    struct socket *so = NULL;
    socreate(AF_INET, &so, SOCK_DGRAM, 0, curthread->td_ucred, curthread);
    int ret = ifioctl(so, SIOCAIFADDR, (caddr_t)&req, curthread);

    sofree(so);

    return ret;
}

static int
lazybsd_veth_set_gateway(struct lazybsd_veth_softc *sc)
{
    struct rt_addrinfo info;
    struct rib_cmd_info rci;

    bzero((caddr_t)&info, sizeof(info));
    info.rti_flags = RTF_GATEWAY;

    struct sockaddr_in gw;
    bzero(&gw, sizeof(gw));
    gw.sin_len = sizeof(gw);
    gw.sin_family = AF_INET;
    gw.sin_addr.s_addr = sc->gateway;
    info.rti_info[RTAX_GATEWAY] = (struct sockaddr *)&gw;

    struct sockaddr_in dst;
    bzero(&dst, sizeof(dst));
    dst.sin_len = sizeof(dst);
    dst.sin_family = AF_INET;
    dst.sin_addr.s_addr = 0;
    info.rti_info[RTAX_DST] = (struct sockaddr *)&dst;

    struct sockaddr_in nm;
    bzero(&nm, sizeof(nm));
    nm.sin_len = sizeof(nm);
    nm.sin_family = AF_INET;
    nm.sin_addr.s_addr = 0;
    info.rti_info[RTAX_NETMASK] = (struct sockaddr *)&nm;

    return rib_action(RT_DEFAULT_FIB, RTM_ADD, &info, &rci);
}

static int
lazybsd_veth_setvaddr(struct lazybsd_veth_softc *sc, struct lazybsd_port_cfg *cfg)
{
    struct in_aliasreq req;
    bzero(&req, sizeof req);

    if (cfg->vip_ifname) {
        strlcpy(req.ifra_name, cfg->vip_ifname, IFNAMSIZ);
    } else {
        strlcpy(req.ifra_name, sc->ifp->if_dname, IFNAMSIZ);
    }

    struct sockaddr_in sa;
    bzero(&sa, sizeof(sa));
    sa.sin_len = sizeof(sa);
    sa.sin_family = AF_INET;

    int i, ret;
    struct socket *so = NULL;
    socreate(AF_INET, &so, SOCK_DGRAM, 0, curthread->td_ucred, curthread);

    for (i = 0; i < sc->nb_vip; ++i) {
        sa.sin_addr.s_addr = sc->vip[i];
        bcopy(&sa, &req.ifra_addr, sizeof(sa));

        // Only support '255.255.255.255' netmask now
        sa.sin_addr.s_addr = 0xFFFFFFFF;
        bcopy(&sa, &req.ifra_mask, sizeof(sa));

        // Only support 'x.x.x.255' broadaddr now
        sa.sin_addr.s_addr = sc->vip[i] | 0xFF000000;
        bcopy(&sa, &req.ifra_broadaddr, sizeof(sa));

        ret = ifioctl(so, SIOCAIFADDR, (caddr_t)&req, curthread);
        if (ret < 0) {
            printf("lazybsd_veth_setvaddr ifioctl SIOCAIFADDR error\n");
            goto done;
        }
    }

done:
    sofree(so);

    return ret;
}

#ifdef INET6
static int
lazybsd_veth_setaddr6(struct lazybsd_veth_softc *sc)
{
    struct in6_aliasreq ifr6;
    bzero(&ifr6, sizeof(ifr6));
    strcpy(ifr6.ifra_name, sc->ifp->if_dname);

    ifr6.ifra_addr.sin6_len = sizeof ifr6.ifra_addr;
    ifr6.ifra_addr.sin6_family = AF_INET6;
    ifr6.ifra_addr.sin6_addr = sc->ip6;

    ifr6.ifra_prefixmask.sin6_len = sizeof ifr6.ifra_prefixmask;
    memset(&ifr6.ifra_prefixmask.sin6_addr, 0xff, sc->prefix_length / 8);
    uint8_t mask_size_mod = sc->prefix_length % 8;
    if (mask_size_mod)
    {
        ifr6.ifra_prefixmask.sin6_addr.__u6_addr.__u6_addr8[sc->prefix_length / 8] = \
            ((1 << mask_size_mod) - 1) << (8 - mask_size_mod);
    }

    ifr6.ifra_lifetime.ia6t_pltime = ifr6.ifra_lifetime.ia6t_vltime = ND6_INFINITE_LIFETIME;

    struct socket *so = NULL;
    socreate(AF_INET6, &so, SOCK_DGRAM, 0, curthread->td_ucred, curthread);
    int ret = ifioctl(so, SIOCAIFADDR_IN6, (caddr_t)&ifr6, curthread);

    sofree(so);

    return ret;
}

static int
lazybsd_veth_set_gateway6(struct lazybsd_veth_softc *sc)
{
    struct sockaddr_in6 gw, dst, nm;;
    struct rt_addrinfo info;
    struct rib_cmd_info rci;

    bzero((caddr_t)&info, sizeof(info));
    info.rti_flags = RTF_GATEWAY;

    bzero(&gw, sizeof(gw));
    bzero(&dst, sizeof(dst));
    bzero(&nm, sizeof(nm));

    gw.sin6_len = dst.sin6_len = nm.sin6_len = sizeof(struct sockaddr_in6);
    gw.sin6_family = dst.sin6_family = nm.sin6_family = AF_INET6;

    gw.sin6_addr = sc->gateway6;
    //dst.sin6_addr = nm.sin6_addr = 0;

    info.rti_info[RTAX_GATEWAY] = (struct sockaddr *)&gw;
    info.rti_info[RTAX_DST] = (struct sockaddr *)&dst;
    info.rti_info[RTAX_NETMASK] = (struct sockaddr *)&nm;

    return rib_action(RT_DEFAULT_FIB, RTM_ADD, &info, &rci);
}

static int
lazybsd_veth_setvaddr6(struct lazybsd_veth_softc *sc, struct lazybsd_port_cfg *cfg)
{
    struct in6_aliasreq ifr6;
    bzero(&ifr6, sizeof(ifr6));

    if (cfg->vip_ifname) {
        strlcpy(ifr6.ifra_name, cfg->vip_ifname, IFNAMSIZ);
    } else {
        strlcpy(ifr6.ifra_name, sc->ifp->if_dname, IFNAMSIZ);
    }

    ifr6.ifra_addr.sin6_len = sizeof ifr6.ifra_addr;
    ifr6.ifra_addr.sin6_family = AF_INET6;

    ifr6.ifra_prefixmask.sin6_len = sizeof ifr6.ifra_prefixmask;
    memset(&ifr6.ifra_prefixmask.sin6_addr, 0xff, sc->prefix_length / 8);
    uint8_t mask_size_mod = sc->prefix_length % 8;
    if (mask_size_mod)
    {
        ifr6.ifra_prefixmask.sin6_addr.__u6_addr.__u6_addr8[sc->prefix_length / 8] = \
            ((1 << mask_size_mod) - 1) << (8 - mask_size_mod);
    }

    ifr6.ifra_lifetime.ia6t_pltime = ifr6.ifra_lifetime.ia6t_vltime = ND6_INFINITE_LIFETIME;

    struct socket *so = NULL;
    socreate(AF_INET6, &so, SOCK_DGRAM, 0, curthread->td_ucred, curthread);

    int i, ret;
    for (i = 0; i < sc->nb_vip6; ++i) {
        ifr6.ifra_addr.sin6_addr = sc->vip6[i];

        ret = ifioctl(so, SIOCAIFADDR_IN6, (caddr_t)&ifr6, curthread);
        if (ret < 0) {
            printf("lazybsd_veth_setvaddr6 ifioctl SIOCAIFADDR error\n");
            goto done;
        }
    }

done:
    sofree(so);

    return ret;
}
#endif /* INET6 */

static int
lazybsd_veth_setup_interface(struct lazybsd_veth_softc *sc, struct lazybsd_port_cfg *cfg)
{
    struct ifnet *ifp;

    ifp = sc->ifp = if_alloc(IFT_ETHER);

    ifp->if_init = lazybsd_veth_init;
    ifp->if_softc = sc;

    if_initname(ifp, sc->host_ifname, IF_DUNIT_NONE);
    ifp->if_flags = IFF_BROADCAST | IFF_SIMPLEX | IFF_MULTICAST;
    ifp->if_ioctl = lazybsd_veth_ioctl;
    ifp->if_start = lazybsd_veth_start;
    ifp->if_transmit = lazybsd_veth_transmit;
    ifp->if_qflush = lazybsd_veth_qflush;
    ether_ifattach(ifp, sc->mac);

    if (cfg->hw_features.rx_csum) {
        ifp->if_capabilities |= IFCAP_RXCSUM;
    }
    if (cfg->hw_features.tx_csum_ip) {
        ifp->if_capabilities |= IFCAP_TXCSUM;
        ifp->if_hwassist |= CSUM_IP;
    }
    if (cfg->hw_features.tx_csum_l4) {
        ifp->if_hwassist |= CSUM_DELAY_DATA;
    }
    if (cfg->hw_features.tx_tso) {
        ifp->if_capabilities |= IFCAP_TSO;
        ifp->if_hwassist |= CSUM_TSO;
    }

    ifp->if_capenable = ifp->if_capabilities;

    sc->host_ctx = lazybsd_dpdk_register_if((void *)sc, (void *)sc->ifp, cfg);
    if (sc->host_ctx == NULL) {
        printf("%s: Failed to register dpdk interface\n", sc->host_ifname);
        return -1;
    }

    // Set ip
    int ret = lazybsd_veth_setaddr(sc);
    if (ret != 0) {
        printf("lazybsd_veth_setaddr failed\n");
    }
    ret = lazybsd_veth_set_gateway(sc);
    if (ret != 0) {
        printf("lazybsd_veth_set_gateway failed\n");
    }

    if (sc->nb_vip) {
        ret = lazybsd_veth_setvaddr(sc, cfg);
    }

#ifdef INET6
    // Set IPv6
    if (cfg->addr6_str) {
        ret = lazybsd_veth_setaddr6(sc);
        if (ret != 0) {
            printf("lazybsd_veth_setaddr6 failed\n");
        }

        if (cfg->gateway6_str) {
            ret = lazybsd_veth_set_gateway6(sc);
            if (ret != 0) {
                printf("lazybsd_veth_set_gateway6 failed\n");
            }
        }
    }

    if (sc->nb_vip6) {
        ret = lazybsd_veth_setvaddr6(sc, cfg);
    }
#endif /* INET6 */

    return (0);
}

void *
lazybsd_veth_attach(struct lazybsd_port_cfg *cfg)
{
    struct lazybsd_veth_softc *sc = NULL;
    int error;

    sc = malloc(sizeof(struct lazybsd_veth_softc), M_DEVBUF, M_WAITOK);
    if (NULL == sc) {
        printf("lazybsd_veth_softc allocation failed\n");
        goto fail;
    }
    memset(sc, 0, sizeof(struct lazybsd_veth_softc));

    if(cfg->ifname){
        snprintf(sc->host_ifname, sizeof(sc->host_ifname), "%s", cfg->ifname);
    } else {
        snprintf(sc->host_ifname, sizeof(sc->host_ifname), LAZYBSD_IF_NAME, cfg->port_id);
    }

    error = lazybsd_veth_config(sc, cfg);
    if (0 != error) {
        goto fail;
    }

    if (0 != lazybsd_veth_setup_interface(sc, cfg)) {
        goto fail;
    }

    return sc->host_ctx;

fail:
    if (sc) {
        if (sc->host_ctx)
            lazybsd_dpdk_deregister_if(sc->host_ctx);

        free(sc, M_DEVBUF);
    }

    return NULL;
}

int
lazybsd_veth_detach(void *arg)
{
    struct lazybsd_veth_softc *sc = (struct lazybsd_veth_softc *)arg;
    if (sc) {
        lazybsd_dpdk_deregister_if(sc->host_ctx);
        free(sc, M_DEVBUF);
    }

    return (0);
}

void *
lazybsd_veth_softc_to_hostc(void *softc)
{
    struct lazybsd_veth_softc *sc = (struct lazybsd_veth_softc *)softc;
    return (void *)sc->host_ctx;
}

/********************
*  get next mbuf's addr, current mbuf's data and datalen.
*
********************/
int lazybsd_next_mbuf(void **mbuf_bsd, void **data, unsigned *len)
{
    struct mbuf *mb = *(struct mbuf **)mbuf_bsd;

    *len = mb->m_len;
    *data = mb->m_data;

    if (mb->m_next)
        *mbuf_bsd = mb->m_next;
    else
        *mbuf_bsd = NULL;
    return 0;
}

void * lazybsd_mbuf_mtod(void* bsd_mbuf)
{
    if ( !bsd_mbuf )
        return NULL;
    return (void*)((struct mbuf *)bsd_mbuf)->m_data;
}

// get source rte_mbuf from ext cluster, which carry rte_mbuf while recving pkt, such as arp.
void* lazybsd_rte_frm_extcl(void* mbuf)
{
    struct mbuf *bsd_mbuf = mbuf;

    if ( (bsd_mbuf->m_flags & M_EXT) &&
        bsd_mbuf->m_ext.ext_type == EXT_DISPOSABLE && bsd_mbuf->m_ext.ext_free == lazybsd_mbuf_ext_free ) {
        return bsd_mbuf->m_ext.ext_arg1;
    }
    else
        return NULL;
}

void
lazybsd_mbuf_set_vlan_info(void *hdr, uint16_t vlan_tci) {
    struct mbuf *m = (struct mbuf *)hdr;
    m->m_pkthdr.ether_vtag = vlan_tci;
    m->m_flags |= M_VLANTAG;
    return;
}
