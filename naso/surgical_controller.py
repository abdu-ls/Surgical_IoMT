#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
Surgical IoMT SDN Controller (Ryu)
===================================
A corrected L2 learning switch for Surgical IoMT networks.

Features:
- OpenFlow 1.3 support
- Safe packet parsing (no crashes on malformed packets)
- Flow timeouts to prevent table exhaustion
- Proper buffer handling for PacketOut messages
- Logging for debugging surgical traffic flows

Usage:
    ryu-manager surgical_sdn_controller.py

Requirements:
- Ryu >= 4.0
- Mininet-WiFi with OVSKernelAP
- Controller must run on 127.0.0.1:6653 (default)

Author: Your Name
Date: 2026
"""

from ryu.base import app_manager
from ryu.controller import ofp_event
from ryu.controller.handler import CONFIG_DISPATCHER, MAIN_DISPATCHER
from ryu.controller.handler import set_ev_cls
from ryu.ofproto import ofproto_v1_3
from ryu.lib.packet import packet
from ryu.lib.packet import ethernet
from ryu.lib.packet import ether_types


class SurgicalSimpleSwitch13(app_manager.RyuApp):
    """
    L2 Learning Switch for Surgical IoMT Networks.
    
    This controller learns MAC addresses and installs flows to forward
    surgical device traffic efficiently. Critical for low-latency OR networks.
    """
    
    OFP_VERSIONS = [ofproto_v1_3.OFP_VERSION]

    def __init__(self, *args, **kwargs):
        super(SurgicalSimpleSwitch13, self).__init__(*args, **kwargs)
        self.mac_to_port = {}  # {dpid: {mac: port}}

    @set_ev_cls(ofp_event.EventOFPSwitchFeatures, CONFIG_DISPATCHER)
    def switch_features_handler(self, ev):
        """
        Handle switch connection: install table-miss flow entry.
        Unknown packets are sent to the controller for learning.
        """
        datapath = ev.msg.datapath
        ofproto = datapath.ofproto
        parser = datapath.ofproto_parser

        # Table-miss flow: send unknown packets to controller
        match = parser.OFPMatch()
        actions = [parser.OFPActionOutput(
            ofproto.OFPP_CONTROLLER,
            ofproto.OFPCML_NO_BUFFER  # ✅ Correct: max_len for controller output
        )]
        self.add_flow(
            datapath, 
            priority=0, 
            match=match, 
            actions=actions,
            idle_timeout=0,
            hard_timeout=0
        )

        self.logger.info("Switch connected: %016x", datapath.id)

    def add_flow(self, datapath, priority, match, actions, buffer_id=None,
                 idle_timeout=0, hard_timeout=0):
        """
        Install a flow entry in the switch flow table.
        
        Args:
            datapath: OpenFlow datapath object
            priority: Flow priority (higher = more specific)
            match: OFPMatch object defining packet criteria
            actions: List of OFPAction objects
            buffer_id: ID of buffered packet (or None)
            idle_timeout: Remove flow after N seconds of inactivity
            hard_timeout: Remove flow after N seconds regardless of activity
        """
        ofproto = datapath.ofproto
        parser = datapath.ofproto_parser

        instructions = [parser.OFPInstructionActions(
            ofproto.OFPIT_APPLY_ACTIONS, actions
        )]

        if buffer_id:
            mod = parser.OFPFlowMod(
                datapath=datapath,
                buffer_id=buffer_id,
                priority=priority,
                match=match,
                instructions=instructions,
                idle_timeout=idle_timeout,
                hard_timeout=hard_timeout
            )
        else:
            mod = parser.OFPFlowMod(
                datapath=datapath,
                priority=priority,
                match=match,
                instructions=instructions,
                idle_timeout=idle_timeout,
                hard_timeout=hard_timeout
            )
        
        datapath.send_msg(mod)

    @set_ev_cls(ofp_event.EventOFPPacketIn, MAIN_DISPATCHER)
    def _packet_in_handler(self, ev):
        """
        Handle packets sent to controller (unknown destinations).
        
        Learns source MAC, decides forwarding action, installs flow,
        and sends packet out appropriate port.
        """
        msg = ev.msg
        datapath = msg.datapath
        ofproto = datapath.ofproto
        parser = datapath.ofproto_parser
        in_port = msg.match['in_port']

        # Parse packet
        pkt = packet.Packet(msg.data)
        
        # ✅ SAFE: Use get_protocol() to avoid IndexError
        eth = pkt.get_protocol(ethernet.ethernet)
        if not eth:
            # Not an Ethernet packet (e.g., ARP, malformed) - ignore
            return
            
        # Ignore LLDP packets (used for topology discovery)
        if eth.ethertype == ether_types.ETH_TYPE_LLDP:
            return

        dst_mac = eth.dst
        src_mac = eth.src
        dpid = datapath.id
        
        # Initialize MAC table for this switch
        self.mac_to_port.setdefault(dpid, {})
        
        self.logger.info("PacketIn: dpid=%016x src=%s dst=%s in_port=%d", 
                        dpid, src_mac, dst_mac, in_port)

        # === LEARN SOURCE MAC ===
        self.mac_to_port[dpid][src_mac] = in_port
        self.logger.debug("Learned: %s -> port %d on switch %016x", 
                         src_mac, in_port, dpid)

        # === DECIDE FORWARDING ACTION ===
        if dst_mac in self.mac_to_port[dpid]:
            # Known destination: forward to specific port
            out_port = self.mac_to_port[dpid][dst_mac]
            self.logger.debug("Forwarding %s -> port %d", dst_mac, out_port)
        else:
            # Unknown destination: flood to all ports (except input)
            out_port = ofproto.OFPP_FLOOD
            self.logger.debug("Flooding unknown destination %s", dst_mac)

        actions = [parser.OFPActionOutput(out_port)]

        # === INSTALL FLOW TO AVOID FUTURE PACKET_IN ===
        if out_port != ofproto.OFPP_FLOOD:
            # Install specific flow for this src-dst pair
            match = parser.OFPMatch(
                in_port=in_port,
                eth_dst=dst_mac,
                eth_src=src_mac
            )
            # ✅ Add timeouts to prevent flow table exhaustion
            self.add_flow(
                datapath,
                priority=10,  # Higher than table-miss (0)
                match=match,
                actions=actions,
                buffer_id=msg.buffer_id,
                idle_timeout=30,    # ✅ Remove after 30s inactivity
                hard_timeout=300    # ✅ Remove after 5min max
            )
            # If buffer_id is valid, flow installation handles packet forwarding
            # No need for separate PacketOut in this case
            if msg.buffer_id != ofproto.OFP_NO_BUFFER:
                return

        # === SEND PACKET_OUT FOR FLOODED OR UNBUFFERED PACKETS ===
        # ✅ CORRECT: Use OFP_NO_BUFFER to check if packet is buffered
        data = None
        if msg.buffer_id == ofproto.OFP_NO_BUFFER:
            # Packet not buffered at switch: include full packet data
            data = msg.data
        # else: packet is buffered; switch will use buffer_id

        out = parser.OFPPacketOut(
            datapath=datapath,
            buffer_id=msg.buffer_id,
            in_port=in_port,
            actions=actions,
            data=data
        )
        datapath.send_msg(out)
        self.logger.debug("PacketOut sent: buffer_id=%d data_len=%d", 
                         msg.buffer_id, len(data) if data else 0)
