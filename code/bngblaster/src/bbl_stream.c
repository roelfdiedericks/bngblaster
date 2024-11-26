/*
 * BNG Blaster (BBL) - Streams
 *
 * Christian Giese, March 2021
 *
 * Copyright (C) 2020-2024, RtBrick, Inc.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "bbl.h"
#include "bbl_session.h"
#include "bbl_stream.h"
#include "bbl_stats.h"

extern volatile bool g_teardown;
extern bool g_init_phase;
extern bool g_traffic;

const char g_multicast_traffic[] = "multicast";
const char g_session_traffic_ipv4[] = "session-ipv4";
const char g_session_traffic_ipv6[] = "session-ipv6";
const char g_session_traffic_ipv6pd[] = "session-ipv6pd";
endpoint_state_t g_endpoint = ENDPOINT_ACTIVE;

/**
 * bbl_stream_fast_index_get
 *
 * @param flow_id flow-id
 * @return stream or NULL if stream not found
 */
bbl_stream_s *
bbl_stream_index_get(uint64_t flow_id)
{
    if(g_ctx->stream_index && flow_id <= g_ctx->streams && flow_id > 0) {
        return g_ctx->stream_index[flow_id-1];
    }
    return NULL;
}

/**
 * bbl_stream_index_init
 */
bool
bbl_stream_index_init()
{
    uint64_t flow_id;
    bbl_stream_s *stream = g_ctx->stream_head;

    g_ctx->stream_index = calloc(g_ctx->streams, sizeof(bbl_stream_s*));
    
    while(stream) {
        flow_id = stream->flow_id;
        if(flow_id > g_ctx->streams || flow_id < 1) {
            return false;
        }
        g_ctx->stream_index[flow_id-1] = stream;
        stream = stream->next;
    }
    return true;
}

static void
bbl_stream_delay(bbl_stream_s *stream, struct timespec *rx_timestamp, struct timespec *bbl_timestamp)
{
    struct timespec delay;
    uint64_t delay_us;
    timespec_sub(&delay, rx_timestamp, bbl_timestamp);
    
    delay_us = (delay.tv_sec * 1000000) + (delay.tv_nsec / 1000);
    if(delay_us == 0) delay_us = 1;

    if(delay_us > stream->rx_max_delay_us) {
        stream->rx_max_delay_us = delay_us;
    }
    if(stream->rx_min_delay_us) {
        if(delay_us < stream->rx_min_delay_us) {
            stream->rx_min_delay_us = delay_us;
        }
    } else {
        stream->rx_min_delay_us = delay_us;
    }
}

static bool
bbl_stream_build_access_pppoe_packet(bbl_stream_s *stream)
{
    bbl_session_s *session = stream->session;
    bbl_stream_config_s *config = stream->config;

    uint16_t buf_len = 0;
    uint16_t tx_len = 0;

    bbl_ethernet_header_s eth = {0};
    bbl_pppoe_session_s pppoe = {0};
    bbl_ipv4_s ipv4 = {0};
    bbl_ipv6_s ipv6 = {0};
    bbl_udp_s udp = {0};
    bbl_bbl_s bbl = {0};

    /* *
     * The corresponding network interfaces will be selected
     * in the following order:
     * - "network-interface" from stream section
     * - "network-interface" from access interface section
     * - first network interface from network section (default)
     */
    bbl_network_interface_s *network_interface;
    if(config->network_interface) {
        network_interface = bbl_network_interface_get(config->network_interface);
    } else {
        network_interface = session->network_interface;
    }
    if(!network_interface) {
        return false;
    }

    eth.dst = session->server_mac;
    eth.src = session->client_mac;
    eth.qinq = session->access_config->qinq;
    eth.vlan_outer = session->vlan_key.outer_vlan_id;
    eth.vlan_outer_priority = config->vlan_priority;
    eth.vlan_inner = session->vlan_key.inner_vlan_id;
    eth.vlan_inner_priority = config->vlan_priority;
    eth.vlan_three = session->access_third_vlan;
    eth.type = ETH_TYPE_PPPOE_SESSION;
    eth.next = &pppoe;
    pppoe.session_id = session->pppoe_session_id;
    udp.src = config->src_port;
    udp.dst = config->dst_port;
    udp.protocol = UDP_PROTOCOL_BBL;
    udp.next = &bbl;
    bbl.type = stream->type;
    bbl.sub_type = stream->sub_type;
    bbl.session_id = session->session_id;
    bbl.ifindex = session->vlan_key.ifindex;
    bbl.outer_vlan_id = session->vlan_key.outer_vlan_id;
    bbl.inner_vlan_id = session->vlan_key.inner_vlan_id;
    bbl.flow_id = stream->flow_id;
    bbl.tos = config->priority;
    bbl.direction = BBL_DIRECTION_UP;
    switch(stream->sub_type) {
        case BBL_SUB_TYPE_IPV4:
            pppoe.protocol = PROTOCOL_IPV4;
            pppoe.next = &ipv4;
            /* Source address */
            if(stream->config->ipv4_access_src_address) {
                ipv4.src = stream->config->ipv4_access_src_address;
            } else {
                ipv4.src = session->ip_address;
            }
            /* Destination address */
            if(stream->config->ipv4_destination_address) {
                ipv4.dst = stream->config->ipv4_destination_address;
            } else {
                if(session->l2tp_session) {
                    ipv4.dst = MOCK_IP_LOCAL;
                } else if(stream->config->ipv4_network_address) {
                    ipv4.dst = stream->config->ipv4_network_address;
                } else {
                    ipv4.dst = network_interface->ip.address;
                }
            }
            if(config->ipv4_df) {
                ipv4.offset = IPV4_DF;
            }
            ipv4.ttl = config->ttl;
            ipv4.tos = config->priority;
            if(stream->tcp) {
                ipv4.protocol = PROTOCOL_IPV4_TCP;
            } else {
                ipv4.protocol = PROTOCOL_IPV4_UDP;
            }
            ipv4.next = &udp;
            if(config->length > 76) {
                bbl.padding = config->length - 76;
            }
            break;
        case BBL_SUB_TYPE_IPV6:
        case BBL_SUB_TYPE_IPV6PD:
            pppoe.protocol = PROTOCOL_IPV6;
            pppoe.next = &ipv6;
            /* Source address */
            if(*(uint64_t*)stream->config->ipv6_access_src_address) {
                ipv6.src = stream->config->ipv6_access_src_address;
            } else {
                if(stream->sub_type == BBL_SUB_TYPE_IPV6) {
                    ipv6.src = session->ipv6_address;
                } else {
                    ipv6.src = session->delegated_ipv6_address;
                }
            }
            /* Destination address */
            if(*(uint64_t*)stream->config->ipv6_destination_address) {
                ipv6.dst = stream->config->ipv6_destination_address;
            } else {
                if(*(uint64_t*)stream->config->ipv6_network_address) {
                    ipv6.dst = stream->config->ipv6_network_address;
                } else {
                    ipv6.dst = network_interface->ip6.address;
                }
            }
            ipv6.ttl = config->ttl;
            ipv6.tos = config->priority;
            if(stream->tcp) {
                ipv6.protocol = IPV6_NEXT_HEADER_TCP;
            } else {
                ipv6.protocol = IPV6_NEXT_HEADER_UDP;
            }
            ipv6.next = &udp;
            if(config->length > 96) {
                bbl.padding = config->length - 96;
            }
            break;
        default:
            return false;
    }

    buf_len = config->length + BBL_MAX_STREAM_OVERHEAD;
    if(buf_len < 256) buf_len = 256;
    stream->tx_buf = malloc(buf_len);
    stream->tx_bbl_hdr_len = bbl.padding+BBL_HEADER_LEN;
    stream->ipv4_src = ipv4.src;
    stream->ipv4_dst = ipv4.dst;
    stream->ipv6_src = ipv6.src;
    stream->ipv6_dst = ipv6.dst;
    if(encode_ethernet(stream->tx_buf, &tx_len, &eth) != PROTOCOL_SUCCESS) {
        free(stream->tx_buf);
        stream->tx_buf = NULL;
        return false;
    }
    stream->tx_len = tx_len;
    return true;
}

static bool
bbl_stream_build_a10nsp_pppoe_packet(bbl_stream_s *stream)
{
    bbl_session_s *session = stream->session;
    bbl_a10nsp_session_s *a10nsp_session = session->a10nsp_session;
    bbl_a10nsp_interface_s *a10nsp_interface;
    bbl_stream_config_s *config = stream->config;

    uint16_t buf_len = 0;
    uint16_t tx_len = 0;

    bbl_ethernet_header_s eth = {0};
    bbl_pppoe_session_s pppoe = {0};
    bbl_ipv4_s ipv4 = {0};
    bbl_ipv6_s ipv6 = {0};
    bbl_udp_s udp = {0};
    bbl_bbl_s bbl = {0};

    a10nsp_interface = bbl_a10nsp_interface_get(config->a10nsp_interface);
    if(!(a10nsp_interface && a10nsp_session)) {
        return false;
    }

    if(stream->direction == BBL_DIRECTION_UP) {
        bbl.direction = BBL_DIRECTION_UP;
        eth.dst = session->server_mac;
        eth.src = session->client_mac;
        eth.qinq = session->access_config->qinq;
        eth.vlan_outer = session->vlan_key.outer_vlan_id;
        udp.src = config->src_port;
        udp.dst = config->dst_port;
    } else {
        bbl.direction = BBL_DIRECTION_DOWN;
        eth.dst = session->client_mac;
        eth.src = session->server_mac;
        eth.qinq = a10nsp_interface->qinq;
        eth.vlan_outer = a10nsp_session->s_vlan;
        if(stream->reverse) {
            udp.src = config->dst_port;
            udp.dst = config->src_port;
        } else {
            udp.src = config->src_port;
            udp.dst = config->dst_port;
        }
    }
    eth.vlan_inner = session->vlan_key.inner_vlan_id;
    eth.vlan_three = session->access_third_vlan;
    eth.vlan_outer_priority = config->vlan_priority;
    eth.vlan_inner_priority = config->vlan_priority;
    eth.type = ETH_TYPE_PPPOE_SESSION;
    eth.next = &pppoe;
    pppoe.session_id = session->pppoe_session_id;
    udp.protocol = UDP_PROTOCOL_BBL;
    udp.next = &bbl;
    bbl.type = stream->type;
    bbl.sub_type = stream->sub_type;
    bbl.session_id = session->session_id;
    bbl.ifindex = session->vlan_key.ifindex;
    bbl.outer_vlan_id = session->vlan_key.outer_vlan_id;
    bbl.inner_vlan_id = session->vlan_key.inner_vlan_id;
    bbl.flow_id = stream->flow_id;
    bbl.tos = config->priority;
    switch(stream->sub_type) {
        case BBL_SUB_TYPE_IPV4:
            pppoe.protocol = PROTOCOL_IPV4;
            pppoe.next = &ipv4;
            if(stream->direction == BBL_DIRECTION_UP) {
                ipv4.src = session->ip_address;
                ipv4.dst = MOCK_IP_LOCAL;
            } else {
                ipv4.src = MOCK_IP_LOCAL;
                ipv4.dst = session->ip_address;
            }
            if(stream->config->ipv4_destination_address) {
                ipv4.dst = stream->config->ipv4_destination_address;
            }
            if(config->ipv4_df) {
                ipv4.offset = IPV4_DF;
            }
            ipv4.ttl = config->ttl;
            ipv4.tos = config->priority;
            if(stream->tcp) {
                ipv4.protocol = PROTOCOL_IPV4_TCP;
            } else {
                ipv4.protocol = PROTOCOL_IPV4_UDP;
            }
            ipv4.next = &udp;
            if(config->length > 76) {
                bbl.padding = config->length - 76;
            }
            break;
        case BBL_SUB_TYPE_IPV6:
        case BBL_SUB_TYPE_IPV6PD:
            pppoe.protocol = PROTOCOL_IPV6;
            pppoe.next = &ipv6;
            if(stream->direction == BBL_DIRECTION_UP) {
                ipv6.src = session->link_local_ipv6_address;
                ipv6.dst = (void*)ipv6_link_local_address;
            } else {
                ipv6.src = (void*)ipv6_link_local_address;
                ipv6.dst = session->link_local_ipv6_address;
            }
            /* Destination address */
            if(*(uint64_t*)stream->config->ipv6_destination_address) {
                ipv6.dst = stream->config->ipv6_destination_address;
            }
            ipv6.ttl = config->ttl;
            ipv6.tos = config->priority;
            if(stream->tcp) {
                ipv6.protocol = IPV6_NEXT_HEADER_TCP;
            } else {
                ipv6.protocol = IPV6_NEXT_HEADER_UDP;
            }
            ipv6.next = &udp;
            if(config->length > 96) {
                bbl.padding = config->length - 96;
            }
            break;
        default:
            return false;
    }

    buf_len = config->length + BBL_MAX_STREAM_OVERHEAD;
    if(buf_len < 256) buf_len = 256;
    stream->tx_buf = malloc(buf_len);
    stream->tx_bbl_hdr_len = bbl.padding+BBL_HEADER_LEN;
    stream->ipv4_src = ipv4.src;
    stream->ipv4_dst = ipv4.dst;
    stream->ipv6_src = ipv6.src;
    stream->ipv6_dst = ipv6.dst;
    if(encode_ethernet(stream->tx_buf, &tx_len, &eth) != PROTOCOL_SUCCESS) {
        free(stream->tx_buf);
        stream->tx_buf = NULL;
        return false;
    }
    stream->tx_len = tx_len;
    return true;
}

static bool
bbl_stream_build_a10nsp_ipoe_packet(bbl_stream_s *stream)
{
    bbl_session_s *session = stream->session;
    bbl_a10nsp_session_s *a10nsp_session = session->a10nsp_session;
    bbl_a10nsp_interface_s *a10nsp_interface;
    bbl_stream_config_s *config = stream->config;

    uint16_t buf_len = 0;
    uint16_t tx_len = 0;

    bbl_ethernet_header_s eth = {0};
    bbl_ipv4_s ipv4 = {0};
    bbl_ipv6_s ipv6 = {0};
    bbl_udp_s udp = {0};
    bbl_bbl_s bbl = {0};

    a10nsp_interface = bbl_a10nsp_interface_get(config->a10nsp_interface);
    if(!(a10nsp_interface && a10nsp_session)) {
        return false;
    }

    if(stream->direction == BBL_DIRECTION_UP) {
        bbl.direction = BBL_DIRECTION_UP;
        eth.dst = session->server_mac;
        eth.src = session->client_mac;
        eth.qinq = session->access_config->qinq;
        eth.vlan_outer = session->vlan_key.outer_vlan_id;
        udp.src = config->src_port;
        udp.dst = config->dst_port;
    } else {
        bbl.direction = BBL_DIRECTION_DOWN;
        eth.dst = session->client_mac;
        eth.src = session->server_mac;
        eth.qinq = a10nsp_interface->qinq;
        eth.vlan_outer = a10nsp_session->s_vlan;
        if(stream->reverse) {
            udp.src = config->dst_port;
            udp.dst = config->src_port;
        } else {
            udp.src = config->src_port;
            udp.dst = config->dst_port;
        }
    }
    eth.vlan_inner = session->vlan_key.inner_vlan_id;
    eth.vlan_three = session->access_third_vlan;
    eth.vlan_outer_priority = config->vlan_priority;
    eth.vlan_inner_priority = config->vlan_priority;
    udp.protocol = UDP_PROTOCOL_BBL;
    udp.next = &bbl;
    bbl.type = stream->type;
    bbl.sub_type = stream->sub_type;
    bbl.session_id = session->session_id;
    bbl.ifindex = session->vlan_key.ifindex;
    bbl.outer_vlan_id = session->vlan_key.outer_vlan_id;
    bbl.inner_vlan_id = session->vlan_key.inner_vlan_id;
    bbl.flow_id = stream->flow_id;
    bbl.tos = config->priority;
    switch(stream->sub_type) {
        case BBL_SUB_TYPE_IPV4:
            eth.type = ETH_TYPE_IPV4;
            eth.next = &ipv4;
            if(stream->direction == BBL_DIRECTION_UP) {
                ipv4.src = session->ip_address;
                ipv4.dst = MOCK_IP_LOCAL;
            } else {
                ipv4.src = MOCK_IP_LOCAL;
                ipv4.dst = session->ip_address;
            }
            if(stream->config->ipv4_destination_address) {
                ipv4.dst = stream->config->ipv4_destination_address;
            }
            if(config->ipv4_df) {
                ipv4.offset = IPV4_DF;
            }
            ipv4.ttl = config->ttl;
            ipv4.tos = config->priority;
            if(stream->tcp) {
                ipv4.protocol = PROTOCOL_IPV4_TCP;
            } else {
                ipv4.protocol = PROTOCOL_IPV4_UDP;
            }
            ipv4.next = &udp;
            if(config->length > 76) {
                bbl.padding = config->length - 76;
            }
            break;
        case BBL_SUB_TYPE_IPV6:
        case BBL_SUB_TYPE_IPV6PD:
            eth.type = ETH_TYPE_IPV6;
            eth.next = &ipv6;
            if(stream->direction == BBL_DIRECTION_UP) {
                ipv6.src = session->link_local_ipv6_address;
                ipv6.dst = (void*)ipv6_link_local_address;
            } else {
                ipv6.src = (void*)ipv6_link_local_address;
                ipv6.dst = session->link_local_ipv6_address;
            }
            /* Destination address */
            if(*(uint64_t*)stream->config->ipv6_destination_address) {
                ipv6.dst = stream->config->ipv6_destination_address;
            }
            ipv6.ttl = config->ttl;
            ipv6.tos = config->priority;
            if(stream->tcp) {
                ipv6.protocol = IPV6_NEXT_HEADER_TCP;
            } else {
                ipv6.protocol = IPV6_NEXT_HEADER_UDP;
            }
            ipv6.next = &udp;
            if(config->length > 96) {
                bbl.padding = config->length - 96;
            }
            break;
        default:
            return false;
    }

    buf_len = config->length + BBL_MAX_STREAM_OVERHEAD;
    if(buf_len < 256) buf_len = 256;
    stream->tx_buf = malloc(buf_len);
    stream->tx_bbl_hdr_len = bbl.padding+BBL_HEADER_LEN;
    stream->ipv4_src = ipv4.src;
    stream->ipv4_dst = ipv4.dst;
    stream->ipv6_src = ipv6.src;
    stream->ipv6_dst = ipv6.dst;
    if(encode_ethernet(stream->tx_buf, &tx_len, &eth) != PROTOCOL_SUCCESS) {
        free(stream->tx_buf);
        stream->tx_buf = NULL;
        return false;
    }
    stream->tx_len = tx_len;
    return true;
}

static bool
bbl_stream_build_access_ipoe_packet(bbl_stream_s *stream)
{
    bbl_session_s *session = stream->session;
    bbl_stream_config_s *config = stream->config;

    uint16_t buf_len = 0;
    uint16_t tx_len = 0;

    bbl_ethernet_header_s eth = {0};
    bbl_ipv4_s ipv4 = {0};
    bbl_ipv6_s ipv6 = {0};
    bbl_udp_s udp = {0};
    bbl_bbl_s bbl = {0};

    /* *
     * The corresponding network interfaces will be selected
     * in the following order:
     * - "network-interface" from stream section
     * - "network-interface" from access interface section
     * - first network interface from network section (default)
     */
    bbl_network_interface_s *network_interface;
    if(config->network_interface) {
        network_interface = bbl_network_interface_get(config->network_interface);
    } else {
        network_interface = session->network_interface;
    }
    if(!network_interface) {
        return false;
    }

    eth.dst = session->server_mac;
    eth.src = session->client_mac;
    eth.qinq = session->access_config->qinq;
    eth.vlan_outer = session->vlan_key.outer_vlan_id;
    eth.vlan_inner = session->vlan_key.inner_vlan_id;
    eth.vlan_three = session->access_third_vlan;
    eth.vlan_inner_priority = config->vlan_priority;
    eth.vlan_outer_priority = config->vlan_priority;

    udp.src = config->src_port;
    udp.dst = config->dst_port;
    udp.protocol = UDP_PROTOCOL_BBL;
    udp.next = &bbl;
    bbl.type = stream->type;
    bbl.sub_type = stream->sub_type;
    bbl.session_id = session->session_id;
    bbl.ifindex = session->vlan_key.ifindex;
    bbl.outer_vlan_id = session->vlan_key.outer_vlan_id;
    bbl.inner_vlan_id = session->vlan_key.inner_vlan_id;
    bbl.flow_id = stream->flow_id;
    bbl.tos = config->priority;
    bbl.direction = BBL_DIRECTION_UP;
    switch(stream->sub_type) {
        case BBL_SUB_TYPE_IPV4:
            eth.type = ETH_TYPE_IPV4;
            eth.next = &ipv4;
            /* Source address */
            if(stream->config->ipv4_access_src_address) {
                ipv4.src = stream->config->ipv4_access_src_address;
            } else {
                ipv4.src = session->ip_address;
            }
            /* Destination address */
            if(stream->config->ipv4_destination_address) {
                ipv4.dst = stream->config->ipv4_destination_address;
            } else {
                if(stream->config->ipv4_network_address) {
                    ipv4.dst = stream->config->ipv4_network_address;
                } else {
                    ipv4.dst = network_interface->ip.address;
                }
            }
            if(config->ipv4_df) {
                ipv4.offset = IPV4_DF;
            }
            ipv4.ttl = config->ttl;
            ipv4.tos = config->priority;
            if(stream->tcp) {
                ipv4.protocol = PROTOCOL_IPV4_TCP;
            } else {
                ipv4.protocol = PROTOCOL_IPV4_UDP;
            }
            ipv4.next = &udp;
            if(config->length > 76) {
                bbl.padding = config->length - 76;
            }
            break;
        case BBL_SUB_TYPE_IPV6:
        case BBL_SUB_TYPE_IPV6PD:
            eth.type = ETH_TYPE_IPV6;
            eth.next = &ipv6;
            /* Source address */
            if(*(uint64_t*)stream->config->ipv6_access_src_address) {
                ipv6.src = stream->config->ipv6_access_src_address;
            } else {
                if(stream->sub_type == BBL_SUB_TYPE_IPV6) {
                    ipv6.src = session->ipv6_address;
                } else {
                    ipv6.src = session->delegated_ipv6_address;
                }
            }
            /* Destination address */
            if(*(uint64_t*)stream->config->ipv6_destination_address) {
                ipv6.dst = stream->config->ipv6_destination_address;
            } else {
                if(*(uint64_t*)stream->config->ipv6_network_address) {
                    ipv6.dst = stream->config->ipv6_network_address;
                } else {
                    ipv6.dst = network_interface->ip6.address;
                }
            }
            ipv6.ttl = config->ttl;
            ipv6.tos = config->priority;
            if(stream->tcp) {
                ipv6.protocol = IPV6_NEXT_HEADER_TCP;
            } else {
                ipv6.protocol = IPV6_NEXT_HEADER_UDP;
            }
            ipv6.next = &udp;
            if(config->length > 96) {
                bbl.padding = config->length - 96;
            }
            break;
        default:
            return false;
    }

    buf_len = config->length + BBL_MAX_STREAM_OVERHEAD;
    if(buf_len < 256) buf_len = 256;
    stream->tx_buf = malloc(buf_len);
    stream->tx_bbl_hdr_len = bbl.padding+BBL_HEADER_LEN;
    stream->ipv4_src = ipv4.src;
    stream->ipv4_dst = ipv4.dst;
    stream->ipv6_src = ipv6.src;
    stream->ipv6_dst = ipv6.dst;
    if(encode_ethernet(stream->tx_buf, &tx_len, &eth) != PROTOCOL_SUCCESS) {
        free(stream->tx_buf);
        stream->tx_buf = NULL;
        return false;
    }
    stream->tx_len = tx_len;
    return true;
}

static bool
bbl_stream_build_network_packet(bbl_stream_s *stream)
{
    bbl_session_s *session = stream->session;
    bbl_stream_config_s *config = stream->config;

    uint16_t buf_len = 0;
    uint16_t tx_len = 0;

    bbl_ethernet_header_s eth = {0};
    bbl_mpls_s mpls1 = {0};
    bbl_mpls_s mpls2 = {0};
    bbl_ipv4_s ipv4 = {0};
    bbl_ipv6_s ipv6 = {0};
    bbl_udp_s udp = {0};
    bbl_bbl_s bbl = {0};

    uint8_t mac[ETH_ADDR_LEN] = {0};

    bbl_network_interface_s *network_interface = stream->tx_network_interface;

    if(!network_interface) {
        return false;
    }

    eth.dst = network_interface->gateway_mac;
    eth.src = network_interface->mac;
    eth.vlan_outer = network_interface->vlan;
    eth.vlan_outer_priority = config->vlan_priority;
    eth.vlan_inner = 0;

    /* Add MPLS labels */
    if(config->tx_mpls1 || stream->ldp_entry) {
        eth.mpls = &mpls1;
        if(stream->ldp_entry) {
            mpls1.label = stream->ldp_entry->label;
        } else {
            mpls1.label = config->tx_mpls1_label;
        }
        mpls1.exp = config->tx_mpls1_exp;
        mpls1.ttl = config->tx_mpls1_ttl;
        if(config->tx_mpls2) {
            mpls1.next = &mpls2;
            mpls2.label = config->tx_mpls2_label;
            mpls2.exp = config->tx_mpls2_exp;
            mpls2.ttl = config->tx_mpls2_ttl;
        }
    }

    if(stream->reverse) {
        udp.src = config->dst_port;
        udp.dst = config->src_port;
    } else {
        udp.src = config->src_port;
        udp.dst = config->dst_port;
    }

    udp.protocol = UDP_PROTOCOL_BBL;
    udp.next = &bbl;
    bbl.type = stream->type;
    bbl.sub_type = stream->sub_type;
    if(session) {
        bbl.session_id = session->session_id;
        bbl.ifindex = session->vlan_key.ifindex;
        bbl.outer_vlan_id = session->vlan_key.outer_vlan_id;
        bbl.inner_vlan_id = session->vlan_key.inner_vlan_id;
    }
    bbl.flow_id = stream->flow_id;
    bbl.tos = config->priority;
    bbl.direction = BBL_DIRECTION_DOWN;
    switch(stream->sub_type) {
        case BBL_SUB_TYPE_IPV4:
            eth.type = ETH_TYPE_IPV4;
            eth.next = &ipv4;

            /* Source address */
            if(stream->config->ipv4_network_address) {
                ipv4.src = stream->config->ipv4_network_address;
            } else {
                ipv4.src = network_interface->ip.address;
            }
            /* Destination address */
            if(stream->nat && stream->reverse) {
                ipv4.dst = stream->reverse->rx_source_ip;
                udp.dst = stream->reverse->rx_source_port;
            } else if(stream->config->ipv4_destination_address) {
                ipv4.dst = stream->config->ipv4_destination_address;
            } else {
                if(session) {
                    ipv4.dst = session->ip_address;
                    if(session->access_type == ACCESS_TYPE_IPOE && 
                       ipv4_addr_in_network(ipv4.dst, &network_interface->ip)) {
                        eth.dst = session->client_mac;       
                    }
                } else {
                    return false;
                }
            }
            if(config->ipv4_df) {
                ipv4.offset = IPV4_DF;
            }
            ipv4.ttl = config->ttl;
            ipv4.tos = config->priority;
            if(stream->tcp) {
                ipv4.protocol = PROTOCOL_IPV4_TCP;
            } else {
                ipv4.protocol = PROTOCOL_IPV4_UDP;
            }
            ipv4.next = &udp;
            if(config->length > 76) {
                bbl.padding = config->length - 76;
            }
            /* Generate multicast destination MAC */
            if(stream->type == BBL_TYPE_MULTICAST) {
                ipv4_multicast_mac(ipv4.dst, mac);
                eth.dst = mac;
                bbl.mc_source = ipv4.src;
                bbl.mc_group = ipv4.dst;
            }
            break;
        case BBL_SUB_TYPE_IPV6:
        case BBL_SUB_TYPE_IPV6PD:
            eth.type = ETH_TYPE_IPV6;
            eth.next = &ipv6;
            /* Source address */
            if(*(uint64_t*)stream->config->ipv6_network_address) {
                ipv6.src = stream->config->ipv6_network_address;
            } else {
                ipv6.src = network_interface->ip6.address;
            }
            /* Destination address */
            if(*(uint64_t*)stream->config->ipv6_destination_address) {
                ipv6.dst = stream->config->ipv6_destination_address;
            } else {
                if(session) {
                    if(stream->sub_type == BBL_SUB_TYPE_IPV6) {
                        ipv6.dst = session->ipv6_address;
                    } else {
                        ipv6.dst = session->delegated_ipv6_address;
                    }
                } else {
                    return false;
                }
            }
            ipv6.ttl = config->ttl;
            ipv6.tos = config->priority;
            if(stream->tcp) {
                ipv6.protocol = IPV6_NEXT_HEADER_TCP;
            } else {
                ipv6.protocol = IPV6_NEXT_HEADER_UDP;
            }
            ipv6.next = &udp;
            if(config->length > 96) {
                bbl.padding = config->length - 96;
            }
            break;
        default:
            return false;
    }

    buf_len = config->length + BBL_MAX_STREAM_OVERHEAD;
    if(buf_len < 256) buf_len = 256;
    stream->tx_buf = malloc(buf_len);
    stream->tx_bbl_hdr_len = bbl.padding+BBL_HEADER_LEN;
    stream->ipv4_src = ipv4.src;
    stream->ipv4_dst = ipv4.dst;
    stream->ipv6_src = ipv6.src;
    stream->ipv6_dst = ipv6.dst;
    if(encode_ethernet(stream->tx_buf, &tx_len, &eth) != PROTOCOL_SUCCESS) {
        free(stream->tx_buf);
        stream->tx_buf = NULL;
        return false;
    }
    stream->tx_len = tx_len;
    return true;
}

static bool
bbl_stream_build_l2tp_packet(bbl_stream_s *stream)
{
    bbl_session_s *session = stream->session;
    bbl_stream_config_s *config = stream->config;

    bbl_l2tp_session_s *l2tp_session = stream->session->l2tp_session;
    bbl_l2tp_tunnel_s *l2tp_tunnel = l2tp_session->tunnel;

    bbl_network_interface_s *network_interface = l2tp_tunnel->interface;

    uint16_t buf_len = 0;
    uint16_t tx_len = 0;

    bbl_ethernet_header_s eth = {0};
    bbl_ipv4_s l2tp_ipv4 = {0};
    bbl_udp_s l2tp_udp = {0};
    bbl_l2tp_s l2tp = {0};
    bbl_ipv4_s ipv4 = {0};
    bbl_udp_s udp = {0};
    bbl_bbl_s bbl = {0};

    if(stream->sub_type != BBL_SUB_TYPE_IPV4) {
        return false;
    }

    eth.dst = network_interface->gateway_mac;
    eth.src = network_interface->mac;
    eth.vlan_outer = network_interface->vlan;
    eth.vlan_inner = 0;
    eth.type = ETH_TYPE_IPV4;
    eth.next = &l2tp_ipv4;
    l2tp_ipv4.dst = l2tp_tunnel->peer_ip;
    l2tp_ipv4.src = l2tp_tunnel->server->ip;
    l2tp_ipv4.ttl = config->ttl;
    l2tp_ipv4.tos = config->priority;
    l2tp_ipv4.protocol = PROTOCOL_IPV4_UDP;
    l2tp_ipv4.next = &l2tp_udp;
    l2tp_udp.src = L2TP_UDP_PORT;
    l2tp_udp.dst = L2TP_UDP_PORT;
    l2tp_udp.protocol = UDP_PROTOCOL_L2TP;
    l2tp_udp.next = &l2tp;
    l2tp.type = L2TP_MESSAGE_DATA;
    l2tp.tunnel_id = l2tp_tunnel->peer_tunnel_id;
    l2tp.session_id = l2tp_session->peer_session_id;
    l2tp.protocol = PROTOCOL_IPV4;
    l2tp.with_length = l2tp_tunnel->server->data_length;
    l2tp.with_offset = l2tp_tunnel->server->data_offset;
    l2tp.next = &ipv4;
    ipv4.dst = session->ip_address;
    ipv4.src = MOCK_IP_LOCAL;
    if(config->ipv4_df) {
        ipv4.offset = IPV4_DF;
    }
    ipv4.ttl = config->ttl;
    ipv4.tos = config->priority;
    if(stream->tcp) {
        ipv4.protocol = PROTOCOL_IPV4_TCP;
    } else {
        ipv4.protocol = PROTOCOL_IPV4_UDP;
    }
    ipv4.next = &udp;
    if(stream->reverse) {
        udp.src = config->dst_port;
        udp.dst = config->src_port;
    } else {
        udp.src = config->src_port;
        udp.dst = config->dst_port;
    }
    udp.protocol = UDP_PROTOCOL_BBL;
    udp.next = &bbl;
    bbl.type = BBL_TYPE_UNICAST;
    bbl.sub_type = BBL_SUB_TYPE_IPV4;
    bbl.session_id = session->session_id;
    bbl.ifindex = session->vlan_key.ifindex;
    bbl.outer_vlan_id = session->vlan_key.outer_vlan_id;
    bbl.inner_vlan_id = session->vlan_key.inner_vlan_id;
    bbl.flow_id = stream->flow_id;
    bbl.tos = config->priority;
    bbl.direction = BBL_DIRECTION_DOWN;
    if(config->length > 76) {
        bbl.padding = config->length - 76;
    }
    buf_len = config->length + BBL_MAX_STREAM_OVERHEAD;
    if(buf_len < 256) buf_len = 256;
    stream->tx_buf = malloc(buf_len);
    stream->tx_bbl_hdr_len = bbl.padding+BBL_HEADER_LEN;
    stream->ipv4_src = ipv4.src;
    stream->ipv4_dst = ipv4.dst;
    if(encode_ethernet(stream->tx_buf, &tx_len, &eth) != PROTOCOL_SUCCESS) {
        free(stream->tx_buf);
        stream->tx_buf = NULL;
        return false;
    }
    stream->tx_len = tx_len;
    return true;
}

static bool
bbl_stream_build_packet(bbl_stream_s *stream)
{
    if(stream->config->stream_group_id == 0) {
        /* RAW stream */
        return bbl_stream_build_network_packet(stream);
    }
    if(stream->session) {
        if(stream->session->access_type == ACCESS_TYPE_PPPOE) {
            if(stream->session->l2tp_session) {
                if(stream->direction == BBL_DIRECTION_UP) {
                    return bbl_stream_build_access_pppoe_packet(stream);
                } else {
                    return bbl_stream_build_l2tp_packet(stream);
                }
            } else if(stream->session->a10nsp_session) {
                return bbl_stream_build_a10nsp_pppoe_packet(stream);
            } else {
                switch(stream->sub_type) {
                    case BBL_SUB_TYPE_IPV4:
                    case BBL_SUB_TYPE_IPV6:
                    case BBL_SUB_TYPE_IPV6PD:
                        if(stream->direction == BBL_DIRECTION_UP) {
                            return bbl_stream_build_access_pppoe_packet(stream);
                        } else {
                            return bbl_stream_build_network_packet(stream);
                        }
                    default:
                        break;
                }
            }
        } else if(stream->session->access_type == ACCESS_TYPE_IPOE) {
            if(stream->session->a10nsp_session) {
                return bbl_stream_build_a10nsp_ipoe_packet(stream);
            } else {
                if(stream->direction == BBL_DIRECTION_UP) {
                    return bbl_stream_build_access_ipoe_packet(stream);
                } else {
                    return bbl_stream_build_network_packet(stream);
                }
            }
        }
    }
    return false;
}

static void
bbl_stream_tx_stats(bbl_stream_s *stream, uint64_t packets, uint64_t bytes)
{
    bbl_session_s *session = stream->session;
    bbl_access_interface_s *access_interface;
    bbl_network_interface_s *network_interface;
    bbl_a10nsp_interface_s *a10nsp_interface;

    if(packets == 0) return;
    if(stream->direction == BBL_DIRECTION_UP) {
        access_interface = stream->tx_access_interface;
        session = stream->session;
        if(access_interface) {
            access_interface->stats.packets_tx += packets;
            access_interface->stats.bytes_tx += bytes;
            access_interface->stats.stream_tx += packets;
            if(session) {
                session->stats.packets_tx += packets;
                session->stats.bytes_tx += bytes;
                session->stats.accounting_packets_tx += packets;
                session->stats.accounting_bytes_tx += bytes;
                if(stream->session_traffic) {
                    switch(stream->sub_type) {
                        case BBL_SUB_TYPE_IPV4:
                            access_interface->stats.session_ipv4_tx += packets;
                            break;
                        case BBL_SUB_TYPE_IPV6:
                            access_interface->stats.session_ipv6_tx += packets;
                            break;
                        case BBL_SUB_TYPE_IPV6PD:
                            access_interface->stats.session_ipv6pd_tx += packets;
                            break;
                        default:
                            break;
                    }
                }
            }
        }
    } else {
        if(stream->tx_network_interface) {
            network_interface = stream->tx_network_interface;
            network_interface->stats.packets_tx += packets;
            network_interface->stats.bytes_tx += bytes;
            network_interface->stats.stream_tx += packets;
            if(stream->type == BBL_TYPE_MULTICAST) {
                network_interface->stats.mc_tx += packets;
            }
            if(session) {
                if(session->l2tp_session) {
                    network_interface->stats.l2tp_data_tx += packets;
                    session->l2tp_session->tunnel->stats.data_tx += packets;
                    session->l2tp_session->stats.data_tx += packets;
                    if(stream->sub_type == BBL_SUB_TYPE_IPV4) {
                        session->l2tp_session->stats.data_ipv4_tx += packets;
                    }
                }
                if(stream->session_traffic) {
                    switch(stream->sub_type) {
                        case BBL_SUB_TYPE_IPV4:
                            network_interface->stats.session_ipv4_tx += packets;
                            break;
                        case BBL_SUB_TYPE_IPV6:
                            network_interface->stats.session_ipv6_tx += packets;
                            break;
                        case BBL_SUB_TYPE_IPV6PD:
                            network_interface->stats.session_ipv6pd_tx += packets;
                            break;
                        default:
                            break;
                    }
                }
            }
        } else if(stream->tx_a10nsp_interface) {
            a10nsp_interface = stream->tx_a10nsp_interface;
            a10nsp_interface->stats.packets_tx += packets;
            a10nsp_interface->stats.bytes_tx += bytes;
            a10nsp_interface->stats.stream_tx += packets;
            if(session) {
                if(session->a10nsp_session) {
                    session->a10nsp_session->stats.packets_tx += packets;
                }
                if(stream->session_traffic) {
                    switch(stream->sub_type) {
                        case BBL_SUB_TYPE_IPV4:
                            a10nsp_interface->stats.session_ipv4_tx += packets;
                            break;
                        case BBL_SUB_TYPE_IPV6:
                            a10nsp_interface->stats.session_ipv6_tx += packets;
                            break;
                        case BBL_SUB_TYPE_IPV6PD:
                            a10nsp_interface->stats.session_ipv6pd_tx += packets;
                            break;
                        default:
                            break;
                    }
                }
            }
        }
    }
}

static void
bbl_stream_rx_stats(bbl_stream_s *stream, uint64_t packets, uint64_t bytes, uint64_t loss)
{
    bbl_session_s *session = stream->session;
    bbl_access_interface_s *access_interface;
    bbl_network_interface_s *network_interface;
    bbl_a10nsp_interface_s *a10nsp_interface;

    if(packets == 0) return;
    if(stream->rx_access_interface) {
        access_interface = stream->rx_access_interface;
        access_interface->stats.stream_rx += packets;
        access_interface->stats.stream_loss += loss;
        if(!stream->rx_fragments) {
            access_interface->stats.packets_rx += packets;
            access_interface->stats.bytes_rx += bytes;
        }
        if(session) {
            session->stats.packets_rx += packets;
            session->stats.bytes_rx += bytes;
            session->stats.accounting_packets_rx += packets;
            session->stats.accounting_bytes_rx += bytes;
            if(stream->session_traffic) {
                switch(stream->sub_type) {
                    case BBL_SUB_TYPE_IPV4:
                        access_interface->stats.session_ipv4_rx += packets;
                        access_interface->stats.session_ipv4_loss += loss;
                        break;
                    case BBL_SUB_TYPE_IPV6:
                        access_interface->stats.session_ipv6_rx += packets;
                        access_interface->stats.session_ipv6_loss += loss;
                        break;
                    case BBL_SUB_TYPE_IPV6PD:
                        access_interface->stats.session_ipv6pd_rx += packets;
                        access_interface->stats.session_ipv6pd_loss += loss;
                        break;
                    default:
                        break;
                }
            }
        }
    } else if(stream->rx_network_interface) {
        network_interface = stream->rx_network_interface;
        network_interface->stats.stream_rx += packets;
        network_interface->stats.stream_loss += loss;
        if(!stream->rx_fragments) {
            network_interface->stats.packets_rx += packets;
            network_interface->stats.bytes_rx += bytes;
        }
        if(session) {
            if(session->l2tp_session) {
                network_interface->stats.l2tp_data_rx += packets;
                session->l2tp_session->tunnel->stats.data_rx += packets;
                session->l2tp_session->stats.data_rx += packets;
                if(stream->type == BBL_SUB_TYPE_IPV4) {
                    session->l2tp_session->stats.data_ipv4_rx += packets;
                }
            }
            if(stream->session_traffic) {
                switch(stream->sub_type) {
                    case BBL_SUB_TYPE_IPV4:
                        network_interface->stats.session_ipv4_rx += packets;
                        network_interface->stats.session_ipv4_loss += loss;
                        break;
                    case BBL_SUB_TYPE_IPV6:
                        network_interface->stats.session_ipv6_rx += packets;
                        network_interface->stats.session_ipv6_loss += loss;
                        break;
                    case BBL_SUB_TYPE_IPV6PD:
                        network_interface->stats.session_ipv6pd_rx += packets;
                        network_interface->stats.session_ipv6pd_loss += loss;
                        break;
                    default:
                        break;
                }
            }
        }
    } else if(stream->rx_a10nsp_interface) {
        a10nsp_interface = stream->rx_a10nsp_interface;
        a10nsp_interface->stats.packets_rx += packets;
        a10nsp_interface->stats.bytes_rx += bytes;
        a10nsp_interface->stats.stream_rx += packets;
        a10nsp_interface->stats.stream_loss += loss;
        if(session) {
            if(session->a10nsp_session) {
                session->a10nsp_session->stats.packets_rx += packets;
            }
            if(stream->session_traffic) {
                switch(stream->sub_type) {
                    case BBL_SUB_TYPE_IPV4:
                        a10nsp_interface->stats.session_ipv4_rx += packets;
                        a10nsp_interface->stats.session_ipv4_loss += loss;
                        break;
                    case BBL_SUB_TYPE_IPV6:
                        a10nsp_interface->stats.session_ipv6_rx += packets;
                        a10nsp_interface->stats.session_ipv6_loss += loss;
                        break;
                    case BBL_SUB_TYPE_IPV6PD:
                        a10nsp_interface->stats.session_ipv6pd_rx += packets;
                        a10nsp_interface->stats.session_ipv6pd_loss += loss;
                        break;
                    default:
                        break;
                }
            }
        }
    }
}

static void
bbl_stream_rx_wrong_session(bbl_stream_s *stream) 
{
    uint64_t packets;
    uint64_t packets_delta;

    packets = stream->rx_wrong_session;
    packets_delta = packets - stream->last_sync_wrong_session;
    stream->last_sync_wrong_session = packets;

    if(stream->rx_access_interface) {
        switch(stream->sub_type) {
            case BBL_SUB_TYPE_IPV4:
                stream->rx_access_interface->stats.session_ipv4_wrong_session += packets_delta;
                break;
            case BBL_SUB_TYPE_IPV6:
                stream->rx_access_interface->stats.session_ipv6_wrong_session += packets_delta;
                break;
            case BBL_SUB_TYPE_IPV6PD:
                stream->rx_access_interface->stats.session_ipv6pd_wrong_session += packets_delta;
                break;
            default:
                break;
        }
    }
}

static void
bbl_stream_setup_established(bbl_stream_s *stream)
{
    if(stream->setup) {
        if(stream->reverse) {
            if(stream->reverse->verified) {
                stream->reverse->setup = false;
                stream->setup = false;
            }
        } else {
            stream->setup = false;
        }
    }
}

static void
bbl_stream_ctrl(bbl_stream_s *stream)
{
    bbl_session_s *session = stream->session;

    uint64_t packets;
    uint64_t loss;
    uint64_t packets_delta;
    uint64_t bytes_delta;
    uint64_t loss_delta;

    /* Calculate TX packets/bytes since last sync. */
    packets = stream->tx_packets;
    packets_delta = packets - stream->last_sync_packets_tx;
    if(packets_delta) {
        bytes_delta = packets_delta * stream->tx_len;
        stream->last_sync_packets_tx = packets;
        bbl_stream_tx_stats(stream, packets_delta, bytes_delta);
    }
    if(g_ctx->config.stream_rate_calc && stream->pps >= 1) {
        bbl_compute_avg_rate(&stream->rate_packets_tx, packets);
    }
    if(unlikely(stream->type == BBL_TYPE_MULTICAST)) {
        return;
    }
    /* Calculate RX packets/bytes since last sync. */
    packets = stream->rx_packets;
    packets_delta = packets - stream->last_sync_packets_rx;
    if(packets_delta) {
        bytes_delta = packets_delta * stream->rx_len;
        stream->last_sync_packets_rx = packets;
        /* Calculate RX loss since last sync. */
        loss = stream->rx_loss;
        loss_delta = loss - stream->last_sync_loss;
        stream->last_sync_loss = loss;
        bbl_stream_rx_stats(stream, packets_delta, bytes_delta, loss_delta);
        if(unlikely(stream->rx_wrong_session)) {
            bbl_stream_rx_wrong_session(stream);
        }
        if(unlikely(!stream->verified)) {
            if(stream->rx_first_seq) {
                if(stream->session_traffic) {
                    if(session) {
                        stream->verified = true;
                        bbl_stream_setup_established(stream);
                        session->session_traffic.flows_verified++;
                        g_ctx->stats.session_traffic_flows_verified++;
                        if(g_ctx->stats.session_traffic_flows_verified == g_ctx->stats.session_traffic_flows) {
                            LOG_NOARG(INFO, "ALL SESSION TRAFFIC FLOWS VERIFIED\n");
                        }
                    }
                } else {
                    stream->verified = true;
                    bbl_stream_setup_established(stream);
                    g_ctx->stats.stream_traffic_flows_verified++;
                    if(g_ctx->stats.stream_traffic_flows_verified == g_ctx->stats.stream_traffic_flows) {
                        LOG_NOARG(INFO, "ALL STREAM TRAFFIC FLOWS VERIFIED\n");
                    }
                }
            }
            if(stream->verified) {
                if(g_ctx->config.traffic_stop_verified) {
                    stream->enabled = false;
                }
            }
        }
    }
    if(g_ctx->config.stream_rate_calc && stream->pps >= 1) {
        bbl_compute_avg_rate(&stream->rate_packets_rx, packets);
    }
}

void
bbl_stream_final()
{
    bbl_stream_s *stream = g_ctx->stream_head;
    while(stream) {
        bbl_stream_ctrl(stream);
        stream = stream->next;
    }
}

static bool
bbl_stream_ldp_lookup(bbl_stream_s *stream)
{
    if(!stream->ldp_entry) {
        if(stream->config->ipv4_ldp_lookup_address) {
            stream->ldp_entry = ldb_db_lookup_ipv4(
                stream->tx_network_interface->ldp_adjacency->instance, 
                stream->config->ipv4_ldp_lookup_address);
        } else if (*(uint64_t*)stream->config->ipv6_ldp_lookup_address) {
            stream->ldp_entry = ldb_db_lookup_ipv6(
                stream->tx_network_interface->ldp_adjacency->instance, 
                &stream->config->ipv6_ldp_lookup_address);
        }
    }

    if(!(stream->ldp_entry && stream->ldp_entry->active)) {
        return false;
    }
    if(stream->ldp_entry->version != stream->ldp_entry_version) {
        stream->ldp_entry_version = stream->ldp_entry->version;
        /* Free packet if LDP entry has changed. */
        if(stream->tx_buf) {
            free(stream->tx_buf);
            stream->tx_buf = NULL;
        }
    }
    return true;
}

static bool
bbl_stream_can_send(bbl_stream_s *stream)
{
    if(*(stream->endpoint) == ENDPOINT_ACTIVE) {
        if(stream->ldp_lookup) {
            return bbl_stream_ldp_lookup(stream);
        }
        if(stream->nat && stream->direction == BBL_DIRECTION_DOWN) {
            /* NAT enabled downstream streams need to wait for upstream 
             * packet to learn translated source IP and port. */
            if(stream->reverse && 
               stream->reverse->rx_source_ip && 
               stream->reverse->rx_source_port) {
                return true;
            }
        } else {
            return true;
        }
    }

    /* Free packet if not ready to send. */
    if(stream->tx_buf) {
        free(stream->tx_buf);
        stream->tx_buf = NULL;
    }
    return false;
}

static void
bbl_stream_update_tcp(bbl_stream_s *stream)
{
    uint16_t  tcp_len = stream->tx_bbl_hdr_len + TCP_HDR_LEN_MIN;
    uint8_t  *tcp_buf = (uint8_t*)(stream->tx_buf + (stream->tx_len - tcp_len));
    uint16_t *checksum = (uint16_t*)(tcp_buf+16);

    if(stream->tcp_flags) {
        *(tcp_buf+13) = stream->tcp_flags & 0x3f;
    }

    *checksum = 0;
    if(stream->ipv6_src && stream->ipv6_dst) {
        *checksum = bbl_ipv6_tcp_checksum(stream->ipv6_src, stream->ipv6_dst, tcp_buf, tcp_len);
    } else {
        *checksum = bbl_ipv4_tcp_checksum(stream->ipv4_src, stream->ipv4_dst, tcp_buf, tcp_len);
    }
}

static void
bbl_stream_update_udp(bbl_stream_s *stream)
{
    uint16_t  udp_len = stream->tx_bbl_hdr_len + UDP_HDR_LEN;
    uint8_t  *udp_buf = (uint8_t*)(stream->tx_buf + (stream->tx_len - udp_len));
    uint16_t *checksum = (uint16_t*)(udp_buf+6);

    *checksum = 0;
    if(stream->ipv6_src && stream->ipv6_dst) {
        *checksum = bbl_ipv6_udp_checksum(stream->ipv6_src, stream->ipv6_dst, udp_buf, udp_len);
    } else {
        *checksum = bbl_ipv4_udp_checksum(stream->ipv4_src, stream->ipv4_dst, udp_buf, udp_len);
    }
}

static protocol_error_t
bbl_stream_io_send(bbl_stream_s *stream)
{
    struct timespec time_elapsed;
    bbl_session_s *session;
    io_handle_s *io = stream->io;
    uint8_t *ptr;

    if(unlikely(stream->reset)) {
        stream->reset = false;
        stream->flow_seq = 1;
        if(stream->max_packets) {
            stream->max_packets = stream->tx_packets + stream->config->max_packets;
        }
        if(stream->config->setup_interval) {
            stream->setup = true;
        }
        return STREAM_WAIT;
    }

    if(!stream->enabled) {
        return STREAM_WAIT;
    }
    
    if(!bbl_stream_can_send(stream)) {
        return WRONG_PROTOCOL_STATE;
    }
    
    /** Enforce optional stream packet limit ... */
    if(stream->max_packets && stream->tx_packets >= stream->max_packets) {
        return FULL;
    }

    /** Enforce optional stream traffic start delay ... */
    if(stream->tx_packets == 0 && stream->config->start_delay) {
        if(stream->wait_start.tv_sec) {
            timespec_sub(&time_elapsed, &io->timestamp, &stream->wait_start);
            if(time_elapsed.tv_sec <= stream->config->start_delay) {
                /** Wait ... */
                return STREAM_WAIT;
            } else {
                stream->wait_start.tv_sec = 0;
            }
        } else {
            /** Start wait window ... */
            stream->wait_start.tv_sec = io->timestamp.tv_sec;
            stream->wait_start.tv_nsec = io->timestamp.tv_nsec;
            return STREAM_WAIT;
        }
    }

    /** Enforce optional stream traffic setup interval ... */
    if(stream->setup) {
        if(stream->wait_start.tv_sec) {
            timespec_sub(&time_elapsed, &io->timestamp, &stream->wait_start);
            if(time_elapsed.tv_sec <= stream->config->setup_interval) {
                /** Wait ... */
                return STREAM_WAIT;
            }
        } 
        stream->wait_start.tv_sec = io->timestamp.tv_sec;
        stream->wait_start.tv_nsec = io->timestamp.tv_nsec;
    }
    
    session = stream->session;
    if(session && session->version != stream->session_version) {
        if(stream->tx_buf) {
            free(stream->tx_buf);
            stream->tx_buf = NULL;
        }
        stream->session_version = session->version;
    }

    if(!stream->tx_buf) {
        if(!bbl_stream_build_packet(stream)) {
            LOG(ERROR, "Failed to build packet for stream %s\n", stream->config->name);
            return ENCODE_ERROR;
        }
    }

    /* Update BBL header fields */
    ptr = stream->tx_buf + stream->tx_len - 16;
    *(uint64_t*)ptr = stream->flow_seq; ptr += sizeof(uint64_t);
    *(uint32_t*)ptr = io->timestamp.tv_sec; ptr += sizeof(uint32_t);
    *(uint32_t*)ptr = io->timestamp.tv_nsec;
    if(stream->tcp) {
        bbl_stream_update_tcp(stream);
    } else if(g_ctx->config.stream_udp_checksum) {
        bbl_stream_update_udp(stream);
    }
    if(stream->flow_seq == 1) {
        stream->tx_first_epoch = io->timestamp.tv_sec;
    }
    return PROTOCOL_SUCCESS;
}

/**
 * bbl_stream_io_stop
 *
 * @param io IO handle
 */
void
bbl_stream_io_stop(io_handle_s *io)
{
    io_bucket_s *io_bucket = io->bucket_head;
    while(io_bucket) {
        io_bucket->base = 0;
        io_bucket->stream_cur = NULL;
        io_bucket = io_bucket->next;
    }
}

/**
 * bbl_stream_io_send_iter
 *
 * @param io IO handle
 * @param now nsec timestamp (CLOCK_MONOTONIC)
 * @return stream or NULL if nothing to send
 */
bbl_stream_s *
bbl_stream_io_send_iter(io_handle_s *io, uint64_t now)
{
    io_bucket_s *io_bucket = io->bucket_cur;
    bbl_stream_s *stream;
    uint64_t min = now - g_ctx->config.stream_burst_ms;
    uint64_t expired;
    while(io_bucket) {
        if(io_bucket->stream_cur) {
            stream = io_bucket->stream_cur;
        } else {
            stream = io_bucket->stream_head;
            io_bucket->stream_cur = stream;
            if(io_bucket->base) {
                io_bucket->base += io_bucket->nsec;
                if(io_bucket->base < min) {
                    io_bucket->base = min;
                }
            } else {
                io_bucket->base = now;
            }
        }
        if(io_bucket->base >= now) {
            /* next bucket */
            io_bucket = io_bucket->next;
            if(!io_bucket) io_bucket = io->bucket_head;
            if(io_bucket == io->bucket_cur) return NULL;
            continue;
        }
        expired = now - io_bucket->base;
        while(stream) {
            if(stream->expired > expired) {
                io_bucket->stream_cur = stream;
                break;
            }
            if (bbl_stream_io_send(stream) == PROTOCOL_SUCCESS) {
                io_bucket->stream_cur = stream->io_next;
                io->bucket_cur = io_bucket;
                return stream;
            }
            stream = stream->io_next;
        }
        if(!stream) {
            if(io_bucket->stream_cur == io_bucket->stream_head) {
                /* We can reset bucket base if none of the streams 
                 * in the bucket is active. */
                io_bucket->base = 0;
            }
            io_bucket->stream_cur = NULL;
        }
        /* next bucket */
        io_bucket = io_bucket->next;
        if(!io_bucket) io_bucket = io->bucket_head;
        if(io_bucket == io->bucket_cur) return NULL;
    }
    return NULL;
}

void
bbl_stream_group_job(timer_s *timer)
{
    bbl_stream_group_s *group = timer->data;
    bbl_stream_s *stream = group->head;
    while(stream) {
        bbl_stream_ctrl(stream);
        stream = stream->group_next;
    }
}

static bbl_stream_group_s *
bbl_stream_group_init(double pps)
{
    time_t interval = 1;
    bbl_stream_group_s *group = calloc(1, sizeof(bbl_stream_group_s));
    group->pps = pps;
    if(pps < 1.0) {
        interval = 1.0 / pps;
    }
    timer_add_periodic(&g_ctx->timer_root, &group->timer, "Stream CTRL", 
                       interval, 0, group, &bbl_stream_group_job);
    group->timer->reset = false;
    return group;
}

static void
bbl_stream_add_group(bbl_stream_s *stream)
{
    bbl_stream_group_s *group = g_ctx->stream_groups;
    while(group) {
        if(group->count < 256 && group->pps == stream->pps) {
            break;
        }
        group = group->next;
    }
    if(!group) {
        group = bbl_stream_group_init(stream->pps);
        group->next = g_ctx->stream_groups;
        g_ctx->stream_groups = group;
    }
    stream->group = group;
    stream->group_next = group->head;

    group->head = stream;
    group->count++;
}

static void
bbl_stream_select_io_lag(bbl_stream_s *stream)
{
    bbl_lag_s *lag = stream->tx_interface->lag;
    bbl_lag_member_s *member;
    io_handle_s *io;
    io_handle_s *io_iter;

    stream->lag = true;
    stream->lag_next = lag->stream_head;
    lag->stream_head = stream;
    lag->stream_count++;

    if(lag->config->lacp_enable) {
        /* With LACP enabled, member interface will be selected
         * if LAG state becomes operational state UP. */
        return;
    }

    /* Without LACP enabled, select member interface with lowest PPS. */
    member = CIRCLEQ_FIRST(&lag->lag_member_qhead);
    if(!member) {
        LOG(ERROR, "Failed to add stream %s to LAG %s (no member interfaces)\n", 
            stream->config->name, lag->interface->name);
    }
    io = member->interface->io.tx;
    CIRCLEQ_FOREACH(member, &lag->lag_member_qhead, lag_member_qnode) {
        io_iter = member->interface->io.tx;
        if(io_iter->stream_pps < io->stream_pps) {
            io = io_iter;
        }
    }
    io_stream_add(io, stream);
}

static void
bbl_stream_select_io(bbl_stream_s *stream)
{
    io_handle_s *io = stream->tx_interface->io.tx;
    io_handle_s *io_iter = io;

    assert(io);
    while(io_iter) {
        if(io_iter->stream_pps < io->stream_pps) {
            io = io_iter;
        }
        io_iter = io_iter->next;
    }
    if(io->thread) {
        stream->threaded = true;
    }
    io_stream_add(io, stream);
}

static void
bbl_stream_add(bbl_stream_s *stream)
{
    bbl_stream_add_group(stream);
    if(stream->tx_interface->type == LAG_INTERFACE) {
        bbl_stream_select_io_lag(stream);
    } else {
        bbl_stream_select_io(stream);
    }
    stream->max_packets = stream->config->max_packets;
    if(stream->config->setup_interval) {
        stream->setup = true;
    }
    if(g_ctx->stream_head) {
        g_ctx->stream_tail->next = stream;
    } else {
        g_ctx->stream_head = stream;
    }
    g_ctx->stream_tail = stream;
    g_ctx->streams++;
    g_ctx->total_pps += stream->pps;
}

static bool 
bbl_stream_session_add(bbl_stream_config_s *config, bbl_session_s *session)
{
    bbl_access_interface_s *access_interface = NULL;
    bbl_network_interface_s *network_interface = NULL;
    bbl_a10nsp_interface_s *a10nsp_interface = NULL;
    bbl_stream_s *stream_up = NULL;
    bbl_stream_s *stream_down = NULL;

    assert(config);
    assert(session);

    access_interface = session->access_interface;
    /* *
     * The corresponding network/a01nsp interfaces will be selected
     * in the following order:
     * - network/a01nsp interface from stream section
     * - network/a01nsp interface from access interface section
     * - first network/a01nsp interface from interface section (default)
     */
    if(config->network_interface) {
        network_interface = bbl_network_interface_get(config->network_interface);
    } else if(config->a10nsp_interface) {
        a10nsp_interface = bbl_a10nsp_interface_get(config->a10nsp_interface);
    } else if(session->access_config->network_interface) {
        network_interface = bbl_network_interface_get(session->access_config->network_interface);
    } else if(session->access_config->a10nsp_interface) {
        a10nsp_interface = bbl_a10nsp_interface_get(session->access_config->a10nsp_interface);
    } else {
        network_interface = bbl_network_interface_get(NULL);
        if(!network_interface) {
            a10nsp_interface = bbl_a10nsp_interface_get(NULL);
        }
    }

    if(config->session_traffic && !(network_interface || a10nsp_interface)) {
        /* Skip session traffic if no network/a10nsp interface is found. */
        return true;
    }

    if(config->direction & BBL_DIRECTION_UP) {
        if(config->type == BBL_SUB_TYPE_IPV4) {
            if(!((network_interface && network_interface->ip.address) ||
                 config->ipv4_destination_address || 
                 config->ipv4_network_address ||
                 a10nsp_interface)) {
                LOG(ERROR, "Failed to add stream %s (upstream) because of missing IPv4 destination address\n", config->name);
                return false;
            }
        } else {
            if(!((network_interface && *(uint64_t*)network_interface->ip6.address) ||
                 *(uint64_t*)config->ipv6_destination_address || 
                 *(uint64_t*)config->ipv6_network_address ||
                 a10nsp_interface)) {
                LOG(ERROR, "Failed to add stream %s (upstream) because of missing IPv6 destination address\n", config->name);
                return false;
            }
        }
        stream_up = calloc(1, sizeof(bbl_stream_s));
        stream_up->enabled = config->autostart;
        stream_up->endpoint = &g_endpoint;
        stream_up->flow_id = g_ctx->flow_id++;
        stream_up->flow_seq = 1;
        stream_up->config = config;
        stream_up->pps = config->pps_upstream;
        stream_up->type = BBL_TYPE_UNICAST;
        stream_up->sub_type = config->type;
        stream_up->direction = BBL_DIRECTION_UP;
        stream_up->session_traffic = config->session_traffic;
        stream_up->session = session;
        switch(stream_up->sub_type) {
            case BBL_SUB_TYPE_IPV4:
                stream_up->endpoint = &(session->endpoint.ipv4);
                stream_up->nat = stream_up->config->nat;
                break;
            case BBL_SUB_TYPE_IPV6:
                stream_up->endpoint = &(session->endpoint.ipv6);
                break;
            case BBL_SUB_TYPE_IPV6PD:
                stream_up->endpoint = &(session->endpoint.ipv6pd);
                break;
            default:
                break;
        }
        if(stream_up->config->raw_tcp) {
            stream_up->tcp = true;
        }
        stream_up->tx_access_interface = access_interface;
        stream_up->tx_interface = access_interface->interface;
        stream_up->session_next = session->streams.head;
        session->streams.head = stream_up;
        bbl_stream_add(stream_up);
        if(stream_up->session_traffic) {
            g_ctx->stats.session_traffic_flows++;
            session->session_traffic.flows++;
            LOG(DEBUG, "Session traffic stream %s (upstream) added to %s (access) with %0.2lf PPS\n", 
                config->name, access_interface->name, stream_up->pps);
        } else {
            g_ctx->stats.stream_traffic_flows++;
            LOG(DEBUG, "Traffic stream %s (upstream) added to %s (access) with %0.2lf PPS\n", 
                config->name, access_interface->name, stream_up->pps);
        }
    }
    if(config->direction & BBL_DIRECTION_DOWN) {
        stream_down = calloc(1, sizeof(bbl_stream_s));
        stream_down->enabled = config->autostart;
        stream_down->endpoint = &g_endpoint;
        stream_down->flow_id = g_ctx->flow_id++;
        stream_down->flow_seq = 1;
        stream_down->config = config;
        stream_down->pps = config->pps;
        stream_down->type = BBL_TYPE_UNICAST;
        stream_down->sub_type = config->type;
        stream_down->direction = BBL_DIRECTION_DOWN;
        stream_down->session = session;
        switch(stream_down->sub_type) {
            case BBL_SUB_TYPE_IPV4:
                stream_down->endpoint = &session->endpoint.ipv4;
                stream_down->nat = stream_down->config->nat;
                break;
            case BBL_SUB_TYPE_IPV6:
                stream_down->endpoint = &session->endpoint.ipv6;
                break;
            case BBL_SUB_TYPE_IPV6PD:
                stream_down->endpoint = &session->endpoint.ipv6pd;
                break;
            default:
                break;
        }
        if(stream_down->config->raw_tcp) {
            stream_down->tcp = true;
        }
        stream_down->session_traffic = config->session_traffic;
        stream_down->session_next = session->streams.head;
        session->streams.head = stream_down;
        if(network_interface) {
            stream_down->tx_network_interface = network_interface;
            stream_down->tx_interface = network_interface->interface;
            if(network_interface->ldp_adjacency && 
               (config->ipv4_ldp_lookup_address || 
                *(uint64_t*)stream_down->config->ipv6_ldp_lookup_address)) {
                stream_down->ldp_lookup = true;
            }
            bbl_stream_add(stream_down);
            if(stream_down->session_traffic) {
                g_ctx->stats.session_traffic_flows++;
                session->session_traffic.flows++;
                LOG(DEBUG, "Session traffic stream %s (downstream) added to %s (network) with %0.2lf PPS\n", 
                    config->name, network_interface->name, stream_down->pps);
            } else {
                g_ctx->stats.stream_traffic_flows++;
                LOG(DEBUG, "Traffic stream %s (downstream) added to %s (network) with %0.2lf PPS\n", 
                    config->name, network_interface->name, stream_down->pps);
            }
        } else if(a10nsp_interface) {
            stream_down->tx_a10nsp_interface = a10nsp_interface;
            stream_down->tx_interface = a10nsp_interface->interface;
            bbl_stream_add(stream_down);
            if(stream_down->session_traffic) {
                g_ctx->stats.session_traffic_flows++;
                session->session_traffic.flows++;
                LOG(DEBUG, "Session traffic stream %s (downstream) added to %s (a10nsp) with %0.2lf PPS\n", 
                    config->name, a10nsp_interface->name, stream_down->pps);
            } else {
                g_ctx->stats.stream_traffic_flows++;
                LOG(DEBUG, "Traffic stream %s (downstream) added to %s (a10nsp) with %0.2lf PPS\n", 
                    config->name, a10nsp_interface->name, stream_down->pps);
            }
        } else {
            LOG(ERROR, "Failed to add stream %s (downstream) because of missing interface\n", config->name);
            return false;
        }
        if(stream_up && stream_down) {
            stream_up->reverse = stream_down;
            stream_down->reverse = stream_up;
        }
    }
    return true;
}

bool
bbl_stream_session_init(bbl_session_s *session)
{
    bbl_stream_config_s *config;

    /** Add session traffic ... */
    if(g_ctx->config.stream_config_session_ipv4_up && session->endpoint.ipv4) {
        if(!bbl_stream_session_add(g_ctx->config.stream_config_session_ipv4_up, session)) {
            return false;
        }
        session->session_traffic.ipv4_up = session->streams.head;
    }
    if(g_ctx->config.stream_config_session_ipv4_down && session->endpoint.ipv4) {
        if(!bbl_stream_session_add(g_ctx->config.stream_config_session_ipv4_down, session)) {
            return false;
        }
        session->session_traffic.ipv4_down = session->streams.head;
    }
    if(g_ctx->config.stream_config_session_ipv6_up && session->endpoint.ipv6) {
        if(!bbl_stream_session_add(g_ctx->config.stream_config_session_ipv6_up, session)) {
            return false;
        }
        session->session_traffic.ipv6_up = session->streams.head;
    }
    if(g_ctx->config.stream_config_session_ipv6_down && session->endpoint.ipv6) {
        if(!bbl_stream_session_add(g_ctx->config.stream_config_session_ipv6_down, session)) {
            return false;
        }
        session->session_traffic.ipv6_down = session->streams.head;
    }
    if(g_ctx->config.stream_config_session_ipv6pd_up && session->endpoint.ipv6pd) {
        if(!bbl_stream_session_add(g_ctx->config.stream_config_session_ipv6pd_up, session)) {
            return false;
        }
        session->session_traffic.ipv6pd_up = session->streams.head;
    }
    if(g_ctx->config.stream_config_session_ipv6pd_down && session->endpoint.ipv6pd) {
        if(!bbl_stream_session_add(g_ctx->config.stream_config_session_ipv6pd_down, session)) {
            return false;
        }
        session->session_traffic.ipv6pd_down = session->streams.head;
    }

    /** Add streams of corresponding stream-group-id */
    if(session->streams.group_id) {
        config = g_ctx->config.stream_config;
        while(config) {
            if(config->stream_group_id == session->streams.group_id) {
                if(!bbl_stream_session_add(config, session)) {
                    return false;
                }
            }
            config = config->next;
        }
    }

    return true;
}

bool
bbl_stream_init() {

    bbl_stream_config_s *config;
    bbl_stream_s *stream;

    bbl_network_interface_s *network_interface;
    int i;

    uint32_t group;
    uint32_t source;

    /* Add RAW streams */
    config = g_ctx->config.stream_config;
    while(config) {
        if(config->stream_group_id == 0) {
            network_interface = bbl_network_interface_get(config->network_interface);
            if(!network_interface) {
                LOG(ERROR, "Failed to add RAW stream %s because of missing network interface\n", config->name);
                return false;
            }

            if(config->direction & BBL_DIRECTION_DOWN) {
                stream = calloc(1, sizeof(bbl_stream_s));
                stream->enabled = config->autostart;
                stream->endpoint = &g_endpoint;
                stream->flow_id = g_ctx->flow_id++;
                stream->flow_seq = 1;
                stream->config = config;
                stream->pps = config->pps;
                stream->type = BBL_TYPE_UNICAST;
                stream->sub_type = config->type;
                if(config->type == BBL_SUB_TYPE_IPV4) {
                    /* All IPv4 multicast addresses start with 1110 */
                    if((config->ipv4_destination_address & htobe32(0xf0000000)) == htobe32(0xe0000000)) {
                        stream->enabled = true;
                        stream->endpoint = &(g_ctx->multicast_endpoint);
                        stream->type = BBL_TYPE_MULTICAST;
                    }
                }
                stream->direction = BBL_DIRECTION_DOWN;
                stream->tx_network_interface = network_interface;
                stream->tx_interface = network_interface->interface;
                if(network_interface->ldp_adjacency && 
                   (config->ipv4_ldp_lookup_address || 
                    *(uint64_t*)stream->config->ipv6_ldp_lookup_address)) {
                    stream->ldp_lookup = true;
                }
                if(config->raw_tcp) {
                    stream->tcp = true;
                }
                bbl_stream_add(stream);
                if(stream->type == BBL_TYPE_MULTICAST) {
                    LOG(DEBUG, "RAW multicast traffic stream %s added to %s with %0.2lf PPS\n", 
                        config->name, network_interface->name, stream->pps);
                } else {
                    g_ctx->stats.stream_traffic_flows++;
                    LOG(DEBUG, "RAW traffic stream %s added to %s with %0.2lf PPS\n", 
                        config->name, network_interface->name, stream->pps);
                }
            }
        }
        config = config->next;
    }

    /* Add autogenerated multicast streams */
    if(g_ctx->config.send_multicast_traffic && g_ctx->config.igmp_group_count) {
        network_interface = bbl_network_interface_get(g_ctx->config.multicast_traffic_network_interface);
        if(!network_interface) {
            LOG_NOARG(ERROR, "Failed to add autogenerated multicast streams because of missing network interface\n");
            return false;
        }

        for(i = 0; i < g_ctx->config.igmp_group_count; i++) {

            group = be32toh(g_ctx->config.igmp_group) + i * be32toh(g_ctx->config.igmp_group_iter);
            if(g_ctx->config.igmp_source) {
                source = g_ctx->config.igmp_source;
            } else {
                source = network_interface->ip.address;
            }
            group = htobe32(group);

            config = calloc(1, sizeof(bbl_stream_config_s));
            config->next = g_ctx->config.stream_config_multicast;
            g_ctx->config.stream_config_multicast = config;

            config->name = (char*)g_multicast_traffic;
            config->autostart = true;
            config->type = BBL_SUB_TYPE_IPV4;
            config->direction = BBL_DIRECTION_DOWN;
            config->ttl = BBL_DEFAULT_TTL;
            config->pps = g_ctx->config.multicast_traffic_pps;
            config->dst_port = BBL_UDP_PORT;
            config->src_port = BBL_UDP_PORT;
            config->length = g_ctx->config.multicast_traffic_len;
            config->priority = g_ctx->config.multicast_traffic_tos;
            config->ipv4_destination_address = group;
            config->ipv4_network_address = source;

            stream = calloc(1, sizeof(bbl_stream_s));
            stream->enabled = true;
            stream->endpoint = &(g_ctx->multicast_endpoint);
            stream->flow_id = g_ctx->flow_id++;
            stream->flow_seq = 1;
            stream->config = config;
            stream->pps = config->pps;
            stream->type = BBL_TYPE_MULTICAST;
            stream->sub_type = config->type;
            stream->direction = BBL_DIRECTION_DOWN;
            stream->tx_network_interface = network_interface;
            stream->tx_interface = network_interface->interface;
            bbl_stream_add(stream);
            LOG(DEBUG, "Autogenerated multicast traffic stream added to %s with %0.2lf PPS\n", 
                network_interface->name, stream->pps);
        }
    }

    /* Add session traffic stream configurations */
    if(g_ctx->config.session_traffic_ipv4_pps) {
        /* Upstream */
        config = calloc(1, sizeof(bbl_stream_config_s));
        config->name = (char*)g_session_traffic_ipv4;
        config->autostart = true;
        config->stream_group_id = UINT16_MAX;
        config->type = BBL_SUB_TYPE_IPV4;
        config->direction = BBL_DIRECTION_UP;
        config->ttl = BBL_DEFAULT_TTL;
        config->session_traffic = true;
        config->pps = g_ctx->config.session_traffic_ipv4_pps;
        config->pps_upstream = config->pps;
        config->dst_port = BBL_UDP_PORT;
        config->src_port = BBL_UDP_PORT;
        config->ipv4_network_address = g_ctx->config.session_traffic_ipv4_address;
        g_ctx->config.stream_config_session_ipv4_up = config;
        /* Downstream */
        config = calloc(1, sizeof(bbl_stream_config_s));
        config->name = (char*)g_session_traffic_ipv4;
        config->autostart = true;
        config->stream_group_id = UINT16_MAX;
        config->type = BBL_SUB_TYPE_IPV4;
        config->direction = BBL_DIRECTION_DOWN;
        config->ttl = BBL_DEFAULT_TTL;
        config->session_traffic = true;
        config->pps = g_ctx->config.session_traffic_ipv4_pps;
        config->pps_upstream = config->pps;
        config->dst_port = BBL_UDP_PORT;
        config->src_port = BBL_UDP_PORT;
        config->ipv4_network_address = g_ctx->config.session_traffic_ipv4_address;
        if(g_ctx->config.session_traffic_ipv4_label) {
            config->tx_mpls1 = true;
            config->tx_mpls1_label = g_ctx->config.session_traffic_ipv4_label;
            config->tx_mpls1_ttl = 255;
        }
        g_ctx->config.stream_config_session_ipv4_down = config;
    }
    if(g_ctx->config.session_traffic_ipv6_pps) {
        /* Upstream */
        config = calloc(1, sizeof(bbl_stream_config_s));
        config->name = (char*)g_session_traffic_ipv6;
        config->autostart = true;
        config->stream_group_id = UINT16_MAX;
        config->type = BBL_SUB_TYPE_IPV6;
        config->direction = BBL_DIRECTION_UP;
        config->ttl = BBL_DEFAULT_TTL;
        config->session_traffic = true;
        config->pps = g_ctx->config.session_traffic_ipv6_pps;
        config->pps_upstream = config->pps;
        config->dst_port = BBL_UDP_PORT;
        config->src_port = BBL_UDP_PORT;
        memcpy(config->ipv6_network_address, g_ctx->config.session_traffic_ipv6_address, IPV6_ADDR_LEN);
        g_ctx->config.stream_config_session_ipv6_up = config;
        /* Downstream */
        config = calloc(1, sizeof(bbl_stream_config_s));
        config->name = (char*)g_session_traffic_ipv6;
        config->autostart = true;
        config->stream_group_id = UINT16_MAX;
        config->type = BBL_SUB_TYPE_IPV6;
        config->direction = BBL_DIRECTION_DOWN;
        config->ttl = BBL_DEFAULT_TTL;
        config->session_traffic = true;
        config->pps = g_ctx->config.session_traffic_ipv6_pps;
        config->pps_upstream = config->pps;
        config->dst_port = BBL_UDP_PORT;
        config->src_port = BBL_UDP_PORT;
        memcpy(config->ipv6_network_address, g_ctx->config.session_traffic_ipv6_address, IPV6_ADDR_LEN);
        if(g_ctx->config.session_traffic_ipv6_label) {
            config->tx_mpls1 = true;
            config->tx_mpls1_label = g_ctx->config.session_traffic_ipv6_label;
            config->tx_mpls1_ttl = 255;
        }
        g_ctx->config.stream_config_session_ipv6_down = config;
    }
    if(g_ctx->config.session_traffic_ipv6pd_pps) {
        /* Upstream */
        config = calloc(1, sizeof(bbl_stream_config_s));
        config->name = (char*)g_session_traffic_ipv6pd;
        config->autostart = true;
        config->stream_group_id = UINT16_MAX;
        config->type = BBL_SUB_TYPE_IPV6PD;
        config->direction = BBL_DIRECTION_UP;
        config->ttl = BBL_DEFAULT_TTL;
        config->session_traffic = true;
        config->pps = g_ctx->config.session_traffic_ipv6pd_pps;
        config->pps_upstream = config->pps;
        config->dst_port = BBL_UDP_PORT;
        config->src_port = BBL_UDP_PORT;
        memcpy(config->ipv6_network_address, g_ctx->config.session_traffic_ipv6_address, IPV6_ADDR_LEN);
        g_ctx->config.stream_config_session_ipv6pd_up = config;
        /* Downstream */
        config = calloc(1, sizeof(bbl_stream_config_s));
        config->name = (char*)g_session_traffic_ipv6pd;
        config->autostart = true;
        config->stream_group_id = UINT16_MAX;
        config->type = BBL_SUB_TYPE_IPV6PD;
        config->direction = BBL_DIRECTION_DOWN;
        config->ttl = BBL_DEFAULT_TTL;
        config->session_traffic = true;
        config->pps = g_ctx->config.session_traffic_ipv6pd_pps;
        config->pps_upstream = config->pps;
        config->dst_port = BBL_UDP_PORT;
        config->src_port = BBL_UDP_PORT;
        memcpy(config->ipv6_network_address, g_ctx->config.session_traffic_ipv6_address, IPV6_ADDR_LEN);
        if(g_ctx->config.session_traffic_ipv6_label) {
            config->tx_mpls1 = true;
            config->tx_mpls1_label = g_ctx->config.session_traffic_ipv6_label;
            config->tx_mpls1_ttl = 255;
        }
        g_ctx->config.stream_config_session_ipv6pd_down = config;
    }
    return true;
}

void
bbl_stream_reset(bbl_stream_s *stream)
{
    if(!stream) return;

    stream->reset_packets_tx = stream->tx_packets;
    stream->reset_packets_rx = stream->rx_packets;
    stream->reset_loss = stream->rx_loss;

    stream->rx_min_delay_us = 0;
    stream->rx_max_delay_us = 0;
    stream->rx_len = 0;
    stream->rx_fragments = 0;
    stream->rx_fragment_offset = 0;
    stream->rx_ttl = 0;
    stream->rx_priority = 0;
    stream->rx_outer_vlan_pbit = 0;
    stream->rx_inner_vlan_pbit = 0;
    stream->rx_mpls1 = false;
    stream->rx_mpls1_exp = 0;
    stream->rx_mpls1_ttl = 0;
    stream->rx_mpls1_label = 0;
    stream->rx_mpls2 = false;
    stream->rx_mpls2_exp = 0;
    stream->rx_mpls2_ttl = 0;
    stream->rx_mpls2_label = 0;
    stream->rx_source_ip = 0;
    stream->rx_source_port = 0;
    stream->rx_first_seq = 0;
    stream->rx_last_seq = 0;

    stream->rate_packets_tx.avg_max = 0;
    stream->rate_packets_rx.avg_max = 0;

    stream->reset = true;
    stream->verified = false;
}

const char *
stream_type_string(bbl_stream_s *stream) {
    switch(stream->type) {
        case BBL_TYPE_UNICAST: return "unicast";
        case BBL_TYPE_MULTICAST: return "multicast";
        default: return "invalid";
    }
}

const char *
stream_sub_type_string(bbl_stream_s *stream) {
    switch(stream->sub_type) {
        case BBL_SUB_TYPE_IPV4: return "ipv4";
        case BBL_SUB_TYPE_IPV6: return "ipv6";
        case BBL_SUB_TYPE_IPV6PD: return "ipv6pd";
        default: return "invalid";
    }
}

static void
bbl_stream_rx_nat(bbl_ethernet_header_s *eth, bbl_stream_s *stream) {
    bbl_ipv4_s *ipv4 = NULL;
    bbl_udp_s *udp = NULL;
    if(eth->type == ETH_TYPE_IPV4) {
        ipv4 = (bbl_ipv4_s*)eth->next;
        if(ipv4->protocol == PROTOCOL_IPV4_UDP || ipv4->protocol == PROTOCOL_IPV4_TCP) {
            udp = (bbl_udp_s*)ipv4->next;
            stream->rx_source_ip = ipv4->src;
            stream->rx_source_port = udp->src;
        }
    }
}

bbl_stream_s *
bbl_stream_rx(bbl_ethernet_header_s *eth, uint8_t *mac)
{
    bbl_bbl_s *bbl = eth->bbl;
    bbl_stream_s *stream;
    bbl_stream_config_s *config;
    bbl_session_s *session;
    bbl_mpls_s *mpls;

    uint64_t loss = 0;
    uint64_t flow_seq;
    uint64_t rx_last_seq;
    static bool log_loss = true;

    if(!(bbl && bbl->type == BBL_TYPE_UNICAST)) {
        return NULL;
    }

    stream = bbl_stream_index_get(bbl->flow_id);
    if(stream) {
        flow_seq = bbl->flow_seq; 
        rx_last_seq = stream->rx_last_seq;
        if(rx_last_seq) {
            /* Stream already verified */
            if(flow_seq > rx_last_seq) {
                if(flow_seq > (rx_last_seq +1)) {
                    loss = flow_seq - (rx_last_seq +1);
                    stream->rx_loss += loss;
                    if(unlikely(log_loss)) {
                        log_loss = log_id[LOSS].enable;
                        LOG(LOSS, "LOSS Unicast flow: %lu seq: %lu last: %lu loss: %lu\n",
                            bbl->flow_id, flow_seq, rx_last_seq, loss);
                    }
                }
                stream->rx_last_seq = flow_seq;
                stream->rx_last_epoch = eth->timestamp.tv_sec;
                stream->rx_packets++;
            } else {
                stream->rx_wrong_order++;
                stream->rx_packets++;
            }
        } else {
            /* Verify stream ... */
            stream->rx_len = eth->length;
            stream->rx_priority = eth->tos;
            stream->rx_ttl = eth->ttl;
            stream->rx_outer_vlan_pbit = eth->vlan_outer_priority;
            stream->rx_inner_vlan_pbit = eth->vlan_inner_priority;
            mpls = eth->mpls;
            if(mpls) {
                stream->rx_mpls1 = true;
                stream->rx_mpls1_label = mpls->label;
                stream->rx_mpls1_exp = mpls->exp;
                stream->rx_mpls1_ttl = mpls->ttl;
                mpls = mpls->next;
                if(mpls) {
                    stream->rx_mpls2 = true;
                    stream->rx_mpls2_label = mpls->label;
                    stream->rx_mpls2_exp = mpls->exp;
                    stream->rx_mpls2_ttl = mpls->ttl;
                }
            }
            config = stream->config;
            if(config->rx_mpls1_label) {
                /* Check if expected outer label is received ... */
                if(stream->rx_mpls1_label != config->rx_mpls1_label) {
                    /* Wrong outer label received! */
                    return NULL;
                }
                if(config->rx_mpls2_label) {
                    /* Check if expected inner label is received ... */
                    if(stream->rx_mpls2_label != config->rx_mpls2_label) {
                        /* Wrong inner label received! */
                        return NULL;
                    }
                }
            }
            if(bbl->sub_type != stream->sub_type || 
                bbl->direction != stream->direction) {
                return NULL;
            }
            if(mac) {
                if(memcmp(mac, eth->dst, ETH_ADDR_LEN) != 0) {
                    return NULL;
                }
            } else {
                session = stream->session;
                if(!session) {
                    return NULL;
                }
                if(session->session_state == BBL_TERMINATED || 
                   session->session_state == BBL_IDLE) {
                    return NULL;
                }
                if(memcmp(session->client_mac, eth->dst, ETH_ADDR_LEN) != 0) {
                    return NULL;
                }
                if(stream->session_traffic) {
                    if(bbl->outer_vlan_id != session->vlan_key.outer_vlan_id ||
                       bbl->inner_vlan_id != session->vlan_key.inner_vlan_id ||
                       bbl->session_id != session->session_id) {
                        stream->rx_wrong_session++;
                        return NULL;
                    }
                }
            }
            if(stream->nat && stream->direction == BBL_DIRECTION_UP) {
                bbl_stream_rx_nat(eth, stream);
            }
            stream->rx_first_seq = flow_seq;
            stream->rx_last_seq = flow_seq;
            stream->rx_first_epoch = eth->timestamp.tv_sec;
            stream->rx_last_epoch = eth->timestamp.tv_sec;
            stream->rx_packets++;
        }
        if(g_ctx->config.stream_delay_calc) {
            bbl_stream_delay(stream, &eth->timestamp, &bbl->timestamp);
        }
        return stream;
    } else {
        return NULL;
    }
}

/**
 * This function enables or disabled all
 * streams except session-traffic and multicast 
 * of the given session, optionally 
 * filtered by name!
 * 
 * @param enabled true (start) / false (stop)
 * @param session session object
 * @param name optionally filter by name
 * @param direction optionally filter by direction
 * @param state optionally filter by state
 */
static void
bbl_stream_enable_session(bool enabled, bbl_session_s *session, 
                          const char *name, uint8_t direction,
                          stream_state_t state)
{
    bbl_stream_s *stream = session->streams.head;
    while(stream) {
        if(state == STREAM_STATE_VERIFIED || state == STREAM_STATE_BIVERIFIED) {
            if((stream->verified == false) || 
               (state == STREAM_STATE_BIVERIFIED && stream->reverse && stream->reverse->verified == false)) {
                stream = stream->session_next; continue;
            }
        }
        if(stream->session_traffic == false &&
           stream->type != BBL_TYPE_MULTICAST && 
           stream->direction & direction) {
            if(name) {
                if(strcmp(name, stream->config->name) == 0) {
                    stream->enabled = enabled;
                }
            } else {
                stream->enabled = enabled;
            }
        }
        stream = stream->session_next;
    }
}

/**
 * This function enables or disabled all
 * streams except session-traffic and multicast
 * optionally filtered by session-group-id 
 * and name! The session-group-id must be set to -1
 * to not filter based on session-group-id. If set
 * to 0 will match all streams belonging to 
 * session of the default session-group-id 0. 
 * 
 * @param enabled true (start) / false (stop)
 * @param session_group_id session-group-id
 * @param name optionally filter by name
 * @param direction optionally filter by direction
 * @param verified_only optionally match verified streams only
 * @param state optionally filter by state
 */
static void
bbl_streams_enable_filter(bool enabled, int session_group_id, 
                          const char *name, const char *interface, uint8_t direction, 
                          stream_state_t state)
{
    bbl_stream_s *stream = g_ctx->stream_head;
    bbl_session_s *session;
    uint32_t i;

    if(session_group_id < 0) {
        while(stream) {
            if(state == STREAM_STATE_VERIFIED || state == STREAM_STATE_BIVERIFIED) {
                if((stream->verified == false) || 
                   (state == STREAM_STATE_BIVERIFIED && stream->reverse && stream->reverse->verified == false)) {
                    stream = stream->next; continue;
                }
            }
            if(stream->session_traffic == false &&
               stream->type != BBL_TYPE_MULTICAST && 
               stream->direction & direction) {
                if(interface) {
                    if(strcmp(interface, stream->tx_interface->name) != 0) {
                        stream = stream->next; continue;
                    }
                }
                if(name) {
                    if(strcmp(name, stream->config->name) != 0) {
                        stream = stream->next; continue;
                    }
                }
                stream->enabled = enabled;
            }
            stream = stream->next;
        }
    } else {
        for(i = 0; i < g_ctx->sessions; i++) {
            session = &g_ctx->session_list[i];
            if(session && session->session_group_id == session_group_id) {
                bbl_stream_enable_session(enabled, session, name, direction, state);
            }
        }
    }
}

static json_t *
bbl_stream_summary_json(int session_group_id, const char *name, const char *interface, uint8_t direction)
{
    bbl_stream_s *stream = g_ctx->stream_head;
    json_t *jobj, *jobj_array;
    jobj_array = json_array();

    while(stream) {

        if(session_group_id >= 0) {
            if(!(stream->session && stream->session->session_group_id == session_group_id)) goto NEXT;
        }
        if(name) {
            if(!strcmp(name, stream->config->name) == 0) goto NEXT;
        }
        if(interface) {
            if(!strcmp(interface, stream->tx_interface->name) == 0) goto NEXT;
        }
        if(!(stream->direction & direction)) goto NEXT;

        jobj = json_pack("{si ss* ss ss ss sb sb sb ss*}",
            "flow-id", stream->flow_id,
            "name", stream->config->name,
            "type", stream_type_string(stream),
            "sub-type", stream_sub_type_string(stream),
            "direction", stream->direction == BBL_DIRECTION_UP ? "upstream" : "downstream",
            "enabled", stream->enabled,
            "active", *(stream->endpoint) == ENDPOINT_ACTIVE ? true : false,
            "verified", stream->verified,
            "interface", stream->tx_interface->name);
        if(jobj) {
            if(stream->session) {
                json_object_set_new(jobj, "session-id", json_integer(stream->session->session_id));
                json_object_set_new(jobj, "session-traffic", json_boolean(stream->session_traffic));
            }
            json_array_append_new(jobj_array, jobj);
        }
NEXT:
        stream = stream->next;
    }
    return jobj_array;
}

json_t *
bbl_stream_json(bbl_stream_s *stream, bool debug)
{
    json_t *root = NULL;
    io_handle_s *io = stream->io;
    char *tx_interface = NULL;
    const char *tx_interface_state = NULL;
    char *rx_interface = NULL;
    char *src_address = NULL;
    char *dst_address = NULL;
    uint16_t src_port = 0;
    uint16_t dst_port = 0;

    if(!stream) {
        return NULL;
    }

    if(stream->tx_interface) {
        tx_interface = stream->tx_interface->name;
        tx_interface_state = interface_state_string(stream->tx_interface->state);
    }
    if(stream->rx_access_interface) {
        rx_interface = stream->rx_access_interface->name;
    } else if(stream->rx_network_interface) {
        rx_interface = stream->rx_network_interface->name;
    } else if(stream->rx_a10nsp_interface) {
        rx_interface = stream->rx_a10nsp_interface->name;
    }

    if (stream->direction == BBL_DIRECTION_DOWN && stream->reverse) {
        src_port = stream->config->dst_port;
        dst_port = stream->config->src_port;
    } else {
        src_port = stream->config->src_port;
        dst_port = stream->config->dst_port;
    }

    if(stream->ipv6_src && stream->ipv6_dst) {
        src_address = format_ipv6_address((ipv6addr_t*)stream->ipv6_src);
        dst_address = format_ipv6_address((ipv6addr_t*)stream->ipv6_dst);
    } else {
        src_address = format_ipv4_address(&stream->ipv4_src);
        if(stream->nat && stream->reverse && 
           stream->reverse->rx_source_ip && 
           stream->reverse->rx_source_port) {
            dst_address = format_ipv4_address(&stream->reverse->rx_source_ip);
            dst_port = stream->reverse->rx_source_port;
        } else { 
            dst_address = format_ipv4_address(&stream->ipv4_dst);
        }
    }

    if(stream->type == BBL_TYPE_UNICAST) {
        root = json_pack("{sI ss* ss ss ss sb sb sb ss sI ss sI ss ss* ss* ss* sI sI si si si si si si sI sI sI sI sI sI sI sI sI sI sI sI sI sI sI sf sf sf sI sI sI }",
            "flow-id", stream->flow_id,
            "name", stream->config->name,
            "type", stream_type_string(stream),
            "sub-type", stream_sub_type_string(stream),
            "direction", stream->direction == BBL_DIRECTION_UP ? "upstream" : "downstream",
            "enabled", stream->enabled,
            "active", *(stream->endpoint) == ENDPOINT_ACTIVE ? true : false,
            "verified", stream->verified,
            "source-address", src_address,
            "source-port", src_port,
            "destination-address", dst_address,
            "destination-port", dst_port,
            "protocol", stream->tcp ? "tcp" : "udp",
            "tx-interface", tx_interface,
            "tx-interface-state", tx_interface_state,
            "rx-interface", rx_interface,
            "rx-first-seq", stream->rx_first_seq,
            "rx-last-seq", stream->rx_last_seq,
            "rx-tos-tc", stream->rx_priority,
            "rx-ttl", stream->rx_ttl,
            "rx-outer-vlan-pbit", stream->rx_outer_vlan_pbit,
            "rx-inner-vlan-pbit", stream->rx_inner_vlan_pbit,
            "rx-len", stream->rx_len,
            "tx-len", stream->tx_len,
            "tx-packets", stream->tx_packets - stream->reset_packets_tx,
            "tx-bytes", (stream->tx_packets - stream->reset_packets_tx) * stream->tx_len,
            "rx-packets", stream->rx_packets - stream->reset_packets_rx,
            "rx-bytes", (stream->rx_packets - stream->reset_packets_rx) * stream->rx_len,
            "rx-loss", stream->rx_loss - stream->reset_loss,
            "rx-wrong-order", stream->rx_wrong_order,
            "rx-delay-us-min", stream->rx_min_delay_us,
            "rx-delay-us-max", stream->rx_max_delay_us,
            "rx-pps", stream->rate_packets_rx.avg,
            "tx-pps", stream->rate_packets_tx.avg,
            "rx-pps-max", stream->rate_packets_rx.avg_max,
            "tx-pps-max", stream->rate_packets_tx.avg_max,
            "tx-bps-l2", stream->rate_packets_tx.avg * stream->tx_len * 8,
            "rx-bps-l2", stream->rate_packets_rx.avg * stream->rx_len * 8,
            "rx-bps-l3", stream->rate_packets_rx.avg * stream->config->length * 8,
            "tx-mbps-l2", (double)(stream->rate_packets_tx.avg * stream->tx_len * 8) / 1000000.0,
            "rx-mbps-l2", (double)(stream->rate_packets_rx.avg * stream->rx_len * 8) / 1000000.0,
            "rx-mbps-l3", (double)(stream->rate_packets_rx.avg * stream->config->length * 8) / 1000000.0,
            "tx-first-epoch", stream->tx_first_epoch,
            "rx-first-epoch", stream->rx_first_epoch,
            "rx-last-epoch", stream->rx_last_epoch
            );

        if(stream->rx_interface_changes) { 
            json_object_set_new(root, "rx-interface-changes", json_integer(stream->rx_interface_changes));
            json_object_set_new(root, "rx-interface-changed-epoch", json_integer(stream->rx_interface_changed_epoch));
        }
        if(stream->rx_fragments) {
            json_object_set_new(root, "rx-fragments", json_integer(stream->rx_fragments));
            json_object_set_new(root, "rx-fragment-offset", json_integer(stream->rx_fragment_offset));
        }
        if(stream->config->rx_mpls1) { 
            json_object_set_new(root, "rx-mpls1-expected", json_integer(stream->config->rx_mpls1_label));
        }
        if(stream->rx_mpls1) {
            json_object_set_new(root, "rx-mpls1", json_integer(stream->rx_mpls1_label));
            json_object_set_new(root, "rx-mpls1-exp", json_integer(stream->rx_mpls1_exp));
            json_object_set_new(root, "rx-mpls1-ttl", json_integer(stream->rx_mpls1_ttl));
        }
        if(stream->config->rx_mpls2) { 
            json_object_set_new(root, "rx-mpls2-expected", json_integer(stream->config->rx_mpls2_label));
        }
        if(stream->rx_mpls2) {
            json_object_set_new(root, "rx-mpls2", json_integer(stream->rx_mpls2_label));
            json_object_set_new(root, "rx-mpls2-exp", json_integer(stream->rx_mpls2_exp));
            json_object_set_new(root, "rx-mpls2-ttl", json_integer(stream->rx_mpls2_ttl));
        }
        if(stream->rx_source_ip) {
            json_object_set_new(root, "rx-source-ip", json_string(format_ipv4_address(&stream->rx_source_ip)));
            json_object_set_new(root, "rx-source-port", json_integer(stream->rx_source_port));
        }
        if(stream->session) {
            json_object_set_new(root, "rx-wrong-session", json_integer(stream->rx_wrong_session));
            json_object_set_new(root, "session-id", json_integer(stream->session->session_id));
            json_object_set_new(root, "session-version", json_integer(stream->session_version));
            json_object_set_new(root, "session-traffic", json_boolean(stream->session_traffic));
        }
        if(stream->reverse) {
            json_object_set_new(root, "reverse-flow-id", json_integer(stream->reverse->flow_id));
        }
        if(stream->lag && io && io->interface) {
            json_object_set_new(root, "lag-member-interface", json_string(io->interface->name));
            json_object_set_new(root, "lag-member-interface-state", json_string(interface_state_string(io->interface->state)));
        }
    } else {
        root = json_pack("{sI ss* ss ss ss sb sb ss* ss* sI sI sI sI sI sf}",
            "flow-id", stream->flow_id,
            "name", stream->config->name,
            "type", stream_type_string(stream),
            "sub-type", stream_sub_type_string(stream),
            "direction", stream->direction == BBL_DIRECTION_UP ? "upstream" : "downstream",
            "enabled", stream->enabled,
            "active", *(stream->endpoint) == ENDPOINT_ACTIVE ? true : false,
            "tx-interface", tx_interface,
            "tx-interface-state", tx_interface_state,
            "tx-len", stream->tx_len,
            "tx-packets", stream->tx_packets - stream->reset_packets_tx,
            "tx-pps", stream->rate_packets_tx.avg,
            "tx-pps-max", stream->rate_packets_tx.avg_max,
            "tx-bps-l2", stream->rate_packets_tx.avg * stream->tx_len * 8,
            "tx-mbps-l2", (double)(stream->rate_packets_tx.avg * stream->tx_len * 8) / 1000000.0);
    }
    if(root && debug) {
        /* Add debug informations. */
        json_object_set_new(root, "debug-global-traffic", json_boolean(g_traffic));
        json_object_set_new(root, "debug-init-phase", json_boolean(g_init_phase));
        json_object_set_new(root, "debug-nat", json_boolean(stream->nat));
        json_object_set_new(root, "debug-reset", json_boolean(stream->reset));
        json_object_set_new(root, "debug-lag", json_boolean(stream->lag));
        json_object_set_new(root, "debug-tx-pps-config", json_real(stream->pps));
        json_object_set_new(root, "debug-tx-packets-real", json_integer(stream->tx_packets));
        json_object_set_new(root, "debug-tx-seq", json_integer(stream->flow_seq));
        json_object_set_new(root, "debug-max-packets", json_integer(stream->max_packets));
        json_object_set_new(root, "debug-tcp-flags", json_integer(stream->tcp_flags));
        json_object_set_new(root, "debug-expired", json_integer(stream->expired));
    }
    return root;
}

/* Control Socket Commands */

int
bbl_stream_ctrl_stats(int fd, uint32_t session_id __attribute__((unused)), json_t *arguments __attribute__((unused)))
{
    int result = 0;
    json_t *root = json_pack("{ss si s{si si}}",
                             "status", "ok",
                             "code", 200,
                             "stream-stats",
                             "total-flows", g_ctx->stats.stream_traffic_flows,
                             "verified-flows", g_ctx->stats.stream_traffic_flows_verified);
    if(root) {
        result = json_dumpfd(root, fd, 0);
        json_decref(root);
    }
    return result;
}

int
bbl_stream_ctrl_info(int fd, uint32_t session_id __attribute__((unused)), json_t *arguments)
{
    int result = 0;

    json_t *root;
    json_t *json_stream = NULL;

    bbl_stream_s *stream;

    int number = 0;
    int debug = 0;

    uint64_t flow_id;

    /* Unpack further arguments */
    if(json_unpack(arguments, "{s:i}", "flow-id", &number) != 0) {
        return bbl_ctrl_status(fd, "error", 400, "missing flow-id");
    }    
    json_unpack(arguments, "{s:b}", "debug", &debug);

    flow_id = number;
    stream = bbl_stream_index_get(flow_id);
    if(stream) {
        json_stream = bbl_stream_json(stream, debug);
        root = json_pack("{ss si so*}",
                         "status", "ok",
                         "code", 200,
                         "stream-info", json_stream);
        if(root) {
            result = json_dumpfd(root, fd, 0);
            json_decref(root);
        } else {
            result = bbl_ctrl_status(fd, "error", 500, "internal error");
            json_decref(json_stream);
        }
        return result;
    } else {
        return bbl_ctrl_status(fd, "warning", 404, "stream not found");
    }
}

int
bbl_stream_ctrl_summary(int fd, uint32_t session_id __attribute__((unused)), json_t *arguments)
{
    int result = 0;

    const char *name = NULL;
    const char *interface = NULL;
    const char *s = NULL;

    int session_group_id = -1;
    uint8_t direction = BBL_DIRECTION_BOTH;

    if(json_unpack(arguments, "{s:i}", "session-group-id", &session_group_id) == 0) {
        if(session_group_id < 0 || session_group_id > UINT16_MAX) {
            return bbl_ctrl_status(fd, "error", 400, "invalid session-group-id");
        }
    }
    if(json_unpack(arguments, "{s:s}", "direction", &s) == 0) {
        if(strcmp(s, "upstream") == 0) {
            direction = BBL_DIRECTION_UP;
        } else if(strcmp(s, "downstream") == 0) {
            direction = BBL_DIRECTION_DOWN;
        } else if(strcmp(s, "both") == 0) {
            direction = BBL_DIRECTION_BOTH;
        } else {
            return bbl_ctrl_status(fd, "error", 400, "invalid direction");
        }
    }
    json_unpack(arguments, "{s:s}", "name", &name);
    json_unpack(arguments, "{s:s}", "interface", &interface);

    json_t *root = json_pack("{ss si so*}",
        "status", "ok",
        "code", 200,
        "stream-summary", bbl_stream_summary_json(session_group_id, name, interface, direction));

    result = json_dumpfd(root, fd, 0);
    json_decref(root);
    return result;
}

int
bbl_stream_ctrl_session(int fd, uint32_t session_id, json_t *arguments __attribute__((unused)))
{
    int result = 0;
    json_t *root;
    json_t *json_streams = NULL;
    json_t *json_stream = NULL;

    bbl_session_s *session;
    bbl_stream_s *stream;

    if(session_id == 0) {
        /* session-id is mandatory */
        return bbl_ctrl_status(fd, "error", 400, "missing session-id");
    }

    session = bbl_session_get(session_id);
    if(session) {
        stream = session->streams.head;

        json_streams = json_array();
        while(stream) {
            json_stream = bbl_stream_json(stream, false);
            json_array_append_new(json_streams, json_stream);
            stream = stream->session_next;
        }
        root = json_pack("{ss si s{si sI sI sI sI sI sI sI sI sf sf so*}}",
                         "status", "ok",
                         "code", 200,
                         "session-streams",
                         "session-id", session->session_id,
                         "rx-packets", session->stats.packets_rx,
                         "tx-packets", session->stats.packets_tx,
                         "rx-accounting-packets", session->stats.accounting_packets_rx,
                         "tx-accounting-packets", session->stats.accounting_packets_tx,
                         "rx-pps", session->stats.rate_packets_rx.avg,
                         "tx-pps", session->stats.rate_packets_tx.avg,
                         "rx-bps-l2", session->stats.rate_bytes_rx.avg * 8,
                         "tx-bps-l2", session->stats.rate_bytes_tx.avg * 8,
                         "rx-mbps-l2", (double)(session->stats.rate_bytes_rx.avg * 8) / 1000000.0,
                         "tx-mbps-l2", (double)(session->stats.rate_bytes_tx.avg * 8) / 1000000.0,
                         "streams", json_streams);

        if(root) {
            result = json_dumpfd(root, fd, 0);
            json_decref(root);
        } else {
            result = bbl_ctrl_status(fd, "error", 500, "internal error");
            json_decref(json_streams);
        }
        return result;
    } else {
        return bbl_ctrl_status(fd, "warning", 404, "session not found");
    }
}

int
bbl_stream_ctrl_reset(int fd, uint32_t session_id __attribute__((unused)), json_t *arguments __attribute__((unused)))
{
    bbl_stream_s *stream = g_ctx->stream_head;
    
    g_ctx->stats.stream_traffic_flows_verified = 0;

    /* Iterate over all traffic streams */
    while(stream) {
        if(!stream->session_traffic) {
            bbl_stream_reset(stream);
        }
        stream = stream->next;
    }
    return bbl_ctrl_status(fd, "ok", 200, NULL);    
}

int
bbl_stream_ctrl_pending(int fd, uint32_t session_id __attribute__((unused)), json_t *arguments __attribute__((unused)))
{
    int result = 0;
    bbl_stream_s *stream = g_ctx->stream_head;

    json_t *root, *json_streams;
    json_streams = json_array();

    /* Iterate over all traffic streams */
    /* Iterate over all traffic streams */
    while(stream) {
        if(!stream->verified) {
            json_array_append_new(json_streams, json_integer(stream->flow_id));
        }
        stream = stream->next;
    }

    root = json_pack("{ss si so}",
                     "status", "ok",
                     "code", 200,
                     "streams-pending", json_streams);
    if(root) {
        result = json_dumpfd(root, fd, 0);
        json_decref(root);
    } else {
        result = bbl_ctrl_status(fd, "error", 500, "internal error");
        json_decref(json_streams);
    }
    return result;
}

static int
bbl_stream_ctrl_enabled(int fd, uint32_t session_id, json_t *arguments, 
                        bool enabled, stream_state_t state)
{
    bbl_stream_s *stream = g_ctx->stream_head;
    bbl_session_s *session;
    const char *name = NULL;
    const char *interface = NULL;
    const char *s = NULL;

    int session_group_id = -1;
    int number = 0;
    int arg = 0;
    uint64_t flow_id = 0;
    uint8_t direction = BBL_DIRECTION_BOTH;

    if(json_unpack(arguments, "{s:i}", "flow-id", &number) == 0) {
        flow_id = number;
        stream = bbl_stream_index_get(flow_id);
        if(stream) {
            stream->enabled = enabled;
        } else {
            return bbl_ctrl_status(fd, "warning", 404, "stream not found");
        }
        return bbl_ctrl_status(fd, "ok", 200, NULL);
    }
    
    if(json_unpack(arguments, "{s:i}", "session-group-id", &session_group_id) == 0) {
        if(session_group_id < 0 || session_group_id > UINT16_MAX) {
            return bbl_ctrl_status(fd, "error", 400, "invalid session-group-id");
        }
    }
    if(json_unpack(arguments, "{s:s}", "direction", &s) == 0) {
        if(strcmp(s, "upstream") == 0) {
            direction = BBL_DIRECTION_UP;
        } else if(strcmp(s, "downstream") == 0) {
            direction = BBL_DIRECTION_DOWN;
        } else if(strcmp(s, "both") == 0) {
            direction = BBL_DIRECTION_BOTH;
        } else {
            return bbl_ctrl_status(fd, "error", 400, "invalid direction");
        }
    }
    json_unpack(arguments, "{s:s}", "name", &name);
    json_unpack(arguments, "{s:s}", "interface", &interface);

    arg = 0;
    json_unpack(arguments, "{s:b}", "verified-only", &arg);
    if(arg) state = STREAM_STATE_VERIFIED;

    arg = 0;
    json_unpack(arguments, "{s:b}", "bidirectional-verified-only", &arg);
    if(arg) state = STREAM_STATE_BIVERIFIED;

    if(session_id) {
        session = bbl_session_get(session_id);
        if(session) {
            bbl_stream_enable_session(enabled, session, name, direction, state);
        } else {
            return bbl_ctrl_status(fd, "warning", 404, "session not found");
        }
    } else {
        bbl_streams_enable_filter(enabled, session_group_id, name, interface, direction, state);
    }
    return bbl_ctrl_status(fd, "ok", 200, NULL);
}

int
bbl_stream_ctrl_start(int fd, uint32_t session_id, json_t *arguments)
{
    return bbl_stream_ctrl_enabled(fd, session_id, arguments, true, STREAM_STATE_ANY);
}

int
bbl_stream_ctrl_stop(int fd, uint32_t session_id, json_t *arguments)
{
    return bbl_stream_ctrl_enabled(fd, session_id, arguments, false, STREAM_STATE_ANY);
}

int
bbl_stream_ctrl_stop_verified(int fd, uint32_t session_id, json_t *arguments)
{
    return bbl_stream_ctrl_enabled(fd, session_id, arguments, false, STREAM_STATE_VERIFIED);
}

int
bbl_stream_ctrl_update(int fd, uint32_t session_id __attribute__((unused)), json_t *arguments)
{
    bbl_stream_s *stream;
    const char *s = NULL;

    int number = 0;
    uint64_t flow_id;

    /* Unpack further arguments */
    if(json_unpack(arguments, "{s:i}", "flow-id", &number) != 0) {
        return bbl_ctrl_status(fd, "error", 400, "missing flow-id");
    }

    uint8_t tcp_flags = 0;

    if(json_unpack(arguments, "{s:s}", "tcp-flags", &s) == 0) {
        if(strcmp(s, "ack") == 0) {
            tcp_flags = 0x10;
        } else if(strcmp(s, "fin") == 0) {
            tcp_flags = 0x01;
        } else if(strcmp(s, "fin-ack") == 0) {
            tcp_flags = 0x11;
        } else if(strcmp(s, "syn") == 0) {
            tcp_flags = 0x02;
        } else if(strcmp(s, "syn-ack") == 0) {
            tcp_flags = 0x12;
        } else if(strcmp(s, "rst") == 0) {
            tcp_flags = 0x04;
        } else {
            return bbl_ctrl_status(fd, "error", 400, "invalid tcp-flags (ack|fin|fin-ack|syn|syn-ack|rst)");
        }
    }

    flow_id = number;
    stream = bbl_stream_index_get(flow_id);
    if(stream) {
        if(tcp_flags) {
            stream->tcp_flags = tcp_flags;
        }
    } else {
        return bbl_ctrl_status(fd, "warning", 404, "stream not found");
    }
    return bbl_ctrl_status(fd, "ok", 200, NULL);
}