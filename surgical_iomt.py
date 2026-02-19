#!/usr/bin/python

from mininet.node import Controller, RemoteController, OVSKernelSwitch
from mininet.log import setLogLevel, info
from mn_wifi.net import Mininet_wifi
from mn_wifi.node import Station, OVSKernelAP
from mn_wifi.cli import CLI
from mininet.link import TCLink
from mn_wifi.link import wmediumd
from mn_wifi.wmediumdConnector import interference

def topology():
    
    net = Mininet_wifi(controller=RemoteController,
                       accessPoint=OVSKernelAP,
                       link=wmediumd,
                       wmediumd_mode=interference)

    info("*** Creating nodes\n")

    # SDN Controller (Ryu/ONOS must be running on port 6653)
    c0 = net.addController('c0', controller=RemoteController,
                           ip='127.0.0.1', port=6653)

    # Access Point (Operating room WiFi)
    ap1 = net.addAccessPoint('ap1', ssid='surgical-wifi',
                             mode='g', channel='6',
                             position='50,50,0')

    # IoMT Devices (Wireless Stations) - Using /24 subnet
    patient = net.addStation('sta1', ip='10.0.0.1/24',
                             position='20,60,0')
    
    wearable = net.addStation('sta2', ip='10.0.0.2/24',
                              position='25,65,0')

    surgical_robot = net.addStation('sta3', ip='10.0.0.3/24',
                                    position='40,50,0')

    hd_camera = net.addStation('sta4', ip='10.0.0.4/24',
                               position='45,55,0')

    # Fog / Edge server (Wired, connected to AP)
    fog = net.addHost('fog1', ip='10.0.0.100/24')

    # Cloud server (Wired, connected to Fog)
    cloud = net.addHost('cloud1', ip='10.0.0.200/24')

    info("*** Configuring WiFi nodes\n")
    net.configureWifiNodes()

    info("*** Creating links\n")
    # AP to Fog: Low-latency edge link (1Gbps, 1ms)
    net.addLink(ap1, fog, cls=TCLink, bw=1000, delay='1ms', loss=0)
    
    # Fog to Cloud: Higher-latency backhaul (100Mbps, 30ms)
    net.addLink(fog, cloud, cls=TCLink, bw=100, delay='30ms', loss=0.1)

    info("*** Starting network\n")
    net.build()
    c0.start()
    ap1.start([c0])

    # --- OPTIONAL: Set default routes for wireless stations ---
    # Ensures traffic to fog/cloud goes through AP
    for sta in [patient, wearable, surgical_robot, hd_camera]:
        sta.cmd('ip route add 10.0.0.100 via 10.0.0.1')  # Route to Fog
        sta.cmd('ip route add 10.0.0.200 via 10.0.0.1')  # Route to Cloud

    info("*** Running CLI\n")
    CLI(net)

    info("*** Stopping network\n")
    net.stop()

if __name__ == '__main__':
    setLogLevel('info')
    topology()
