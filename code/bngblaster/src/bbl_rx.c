/*
 * BNG Blaster (BBL) - RX
 *
 * Hannes Gredler, July 2020
 * Christian Giese, October 2020
 *
 * Copyright (C) 2020-2025, RtBrick, Inc.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "bbl.h"
#include "bbl_stream.h"

static bool
bbl_rx_stream_network(bbl_network_interface_s *interface, 
                      bbl_ethernet_header_s *eth) 
{
    bbl_stream_s *stream;
    if(!eth->bbl) return false;
    
    if(interface->skip_stream_mac_validation) {
        /* Use special marker to skip MAC validation for upstream packets 
         * in routed scenarios (e.g., PPPoE) where destination MAC is 
         * gateway MAC, not interface MAC */
        stream = bbl_stream_rx(eth, (uint8_t*)BBL_SKIP_MAC_VALIDATION);
    } else {
        /* Normal MAC validation against interface MAC */
        stream = bbl_stream_rx(eth, interface->mac);
    }
    if(stream) {
        if(stream->rx_network_interface != interface) {
            if(stream->rx_network_interface) {
                /* RX interface has changed! */
                stream->rx_interface_changes++;
                stream->rx_interface_changed_epoch = eth->timestamp.tv_sec;
            }
            stream->rx_network_interface = interface;
        }
        return true;
    }
    return false;
}

static bool
bbl_rx_stream_access(bbl_access_interface_s *interface, 
                     bbl_ethernet_header_s *eth) 
{
    bbl_stream_s *stream;
    if(!eth->bbl) return false;
    stream = bbl_stream_rx(eth, NULL);
    if(stream) {
        if(stream->rx_access_interface == NULL) {
            stream->rx_access_interface = interface;
        }
        return true;
    }
    return false;
}

static bool
bbl_rx_stream_a10nsp(bbl_a10nsp_interface_s *interface, 
                     bbl_ethernet_header_s *eth) 
{
    bbl_stream_s *stream;
    if(!eth->bbl) return false;
    stream = bbl_stream_rx(eth, interface->mac);
    if(stream) {
        if(stream->rx_a10nsp_interface == NULL) {
            stream->rx_a10nsp_interface = interface;
        }
        return true;
    }
    return false;
}

bool
bbl_rx_thread(bbl_interface_s *interface, 
              bbl_ethernet_header_s *eth)
{
    bbl_network_interface_s *network_interface;
    if(interface->state == INTERFACE_DISABLED) {
        return true;
    }
    network_interface = interface->network_vlan[eth->vlan_outer];
    if(network_interface) {
        return bbl_rx_stream_network(network_interface, eth);
    } else if(interface->access) {
        return bbl_rx_stream_access(interface->access, eth);
    } else if(interface->a10nsp) {
        return bbl_rx_stream_a10nsp(interface->a10nsp, eth);
    }
    return false;
}

void
bbl_rx_handler(bbl_interface_s *interface,
               bbl_ethernet_header_s *eth)
{
    bbl_network_interface_s *network_interface;

    if(interface->state == INTERFACE_DISABLED) {
        return;
    }

    /* Check for link/port protocols like LACP or LLDP */
    switch(eth->type) {
        case ETH_TYPE_LACP:
            bbl_lag_rx_lacp(interface, eth);
            return;
        default:
            break;
    }

    if(interface->type == LAG_MEMBER_INTERFACE) {
        bbl_rx_handler(interface->lag->interface, eth);
        return;
    }

    network_interface = interface->network_vlan[eth->vlan_outer];
    if(network_interface) {
        if(!bbl_rx_stream_network(network_interface, eth)) {
            bbl_network_rx_handler(network_interface, eth);
        }
    } else if(interface->access) {
        if(!bbl_rx_stream_access(interface->access, eth)) {
            bbl_access_rx_handler(interface->access, eth);
        }
    } else if(interface->a10nsp) {
        if(!bbl_rx_stream_a10nsp(interface->a10nsp, eth)) {
            bbl_a10nsp_rx_handler(interface->a10nsp, eth);
        }
    }
}