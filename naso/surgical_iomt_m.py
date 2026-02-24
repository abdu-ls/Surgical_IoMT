#!/usr/bin/python
# -*- coding: utf-8 -*-
"""
Surgical IoMT Network Topology + Network Profiler (CORRECTED)
==============================================================
Fixed version with robust parsing and iwconfig support.

Changes from previous version:
- Uses iwconfig instead of iwdev for RSSI (compatible with Mininet-WiFi)
- Robust ping parsing with regex fallbacks
- Fixed CPU parsing regex for variable whitespace
- Added delay after route setup for propagation
- Better error handling throughout

Usage:
    1. Start Ryu: ryu-manager surgical_sdn_controller.py
    2. Run: sudo python3 surgical_iot.py
    3. Output: network_trace.csv for DQN training
"""

from mininet.node import Controller, RemoteController, OVSKernelSwitch
from mininet.log import setLogLevel, info
from mininet.link import TCLink
from mn_wifi.net import Mininet_wifi
from mn_wifi.node import Station, OVSKernelAP
from mn_wifi.cli import CLI
from mn_wifi.link import wmediumd
from mn_wifi.wmediumdConnector import interference

import time
import csv
import re

# =============================================================================
# === PROFILER CONFIGURATION ===
# =============================================================================
DURATION = 3600       # 1 Hour profiling
INTERVAL = 5          # Target interval between samples (seconds)
OUTPUT_FILE = "network_trace.csv"
IPERF_FREQ = 5        # Run iperf only every N iterations to reduce load

# =============================================================================
# === TOPOLOGY DEFINITION ===
# =============================================================================
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
    
    # AP to Fog: Low-latency edge link (1Gbps, 1ms, 0% loss)
    net.addLink(ap1, fog, cls=TCLink, bw=1000, delay='1ms', loss=0,
                htbs={'r2q': 100})
    
    # Fog to Cloud: Higher-latency backhaul (100Mbps, 30ms, 0.1% loss)
    net.addLink(fog, cloud, cls=TCLink, bw=100, delay='30ms', loss=0.1,
                htbs={'r2q': 100})

    info("*** Starting network\n")
    net.build()
    c0.start()
    ap1.start([c0])

    # --- Set default routes for wireless stations ---
    info("*** Setting routes for wireless stations\n")
    for sta in [patient, wearable, surgical_robot, hd_camera]:
        sta.cmd('ip route add 10.0.0.100 via 10.0.0.1')
        sta.cmd('ip route add 10.0.0.200 via 10.0.0.1')
    
    # ✅ FIX: Wait for routes to propagate
    info("*** Waiting 2 seconds for routes to propagate\n")
    time.sleep(2)

    # =============================================================================
    # === CONNECTIVITY TEST (Before Profiling) ===
    # =============================================================================
    info("*** Testing connectivity before profiling\n")
    # Try multiple pings to account for initial ARP resolution
    test_ping = surgical_robot.cmd('ping -c 5 -W 2 10.0.0.100')
    if '100% packet loss' in test_ping or 'unreachable' in test_ping.lower():
        info("*** WARNING: Connectivity test failed! Check routing/SDN.\n")
        info("*** Ping output: {}\n".format(test_ping[:300]))
    else:
        info("*** Connectivity OK. Starting profiler.\n")

    # =============================================================================
    # === INLINE NETWORK PROFILER (CORRECTED) ===
    # =============================================================================
    info("*** Starting Network Profiler for {}s\n".format(DURATION))
    
    # --- Node references ---
    CLIENT_STA = surgical_robot   # sta3: Surgical robot (wireless client)
    SERVER_STA = fog              # fog1: Edge server (wired)
    SWITCH_AP = ap1               # ap1: Access Point / SDN switch
    SERVER_IP = "10.0.0.100"      # Fog server IP
    
    # --- Helper Functions (CORRECTED) ---
    
    def get_ping_stats():
        """Parse ping output robustly with regex fallbacks."""
        try:
            out = CLIENT_STA.cmd("ping -c 3 -i 0.2 -W 1 {}".format(SERVER_IP))
            latency = jitter = loss = 0.0
            
            # Parse packet loss
            loss_match = re.search(r'(\d+(?:\.\d+)?)\s*%\s*packet loss', out)
            if loss_match:
                loss = float(loss_match.group(1))
            
            # Parse RTT: look for "min/avg/max/mdev = X/Y/Z/W ms"
            rtt_match = re.search(r'rtt\s+min/avg/max/(?:mdev|stddev)\s*=\s*([\d.]+)/([\d.]+)/([\d.]+)/([\d.]+)\s*ms', out)
            if rtt_match:
                latency = float(rtt_match.group(2))  # avg
                jitter = float(rtt_match.group(4))   # mdev/stddev
            else:
                # Fallback: try simple "time=X.XX ms" pattern (single ping)
                time_match = re.search(r'time[=<]\s*([\d.]+)\s*ms', out)
                if time_match:
                    latency = float(time_match.group(1))
                    jitter = 0.0  # Unknown for single ping
            
            return latency, jitter, loss
        except Exception as e:
            info("  [DEBUG] Ping error: {}\n".format(e))
            return 0.0, 0.0, 100.0

    def get_bandwidth():
        try:
            out = CLIENT_STA.cmd("iperf3 -c {} -t 1 -P 1 2>&1".format(SERVER_IP))
            # Handle both iperf2 and iperf3 output formats
            m = re.search(r'(\d+\.\d+)\s*[KMGT]?bits?/sec', out)
            return float(m.group(1)) if m else 0.0
        except Exception as e:
            info("  [DEBUG] Bandwidth error: {}\n".format(e))
            return 0.0

    def get_switch_stats():
        try:
            out = SWITCH_AP.cmd("ovs-ofctl dump-ports {}".format(SWITCH_AP.name))
            tx_bytes = rx_bytes = drops = 0
            tx_match = re.search(r'tx_bytes[=:]\s*(\d+)', out)
            rx_match = re.search(r'rx_bytes[=:]\s*(\d+)', out)
            drop_match = re.search(r'drop[=:]\s*(\d+)', out)
            tx_bytes = int(tx_match.group(1)) if tx_match else 0
            rx_bytes = int(rx_match.group(1)) if rx_match else 0
            drops = int(drop_match.group(1)) if drop_match else 0
            return tx_bytes, rx_bytes, drops
        except Exception as e:
            info("  [DEBUG] Switch stats error: {}\n".format(e))
            return 0, 0, 0

    def get_wireless_rssi():
        """✅ FIX: Use iwconfig instead of iwdev (compatible with Mininet-WiFi)."""
        try:
            # iwconfig output: "Signal level=-46 dBm" or "Signal level:-46 dBm"
            out = CLIENT_STA.cmd("iwconfig {}-wlan0 2>/dev/null | grep -i 'signal level'".format(CLIENT_STA.name))
            # Match patterns: "Signal level=-46 dBm" or "level:-46dBm" or "level=-46 dBm"
            m = re.search(r'[Ss]ignal\s*[Ll]evel[=:\s]+(-?\d+)\s*dBm', out)
            rssi = int(m.group(1)) if m else -100
            if rssi == -100:
                info("  [DEBUG] RSSI parsing failed. iwconfig output: '{}'\n".format(out.strip()[:100]))
            return rssi
        except Exception as e:
            info("  [DEBUG] RSSI error: {}\n".format(e))
            return -100

    def get_server_resources():
        cpu_load = 0.0
        ram_load = 0.0
        try:
            # --- CPU: Handle variable whitespace in top output ---
            cpu_out = SERVER_STA.cmd("top -bn1 | grep 'Cpu(s)'")
            # Match "96.7 id" with flexible whitespace: \s* allows any spacing
            idle_match = re.search(r'(\d+\.\d+)\s+id', cpu_out)
            if idle_match:
                cpu_load = 100.0 - float(idle_match.group(1))
            else:
                info("  [DEBUG] CPU parsing failed. Output: '{}'\n".format(cpu_out.strip()[:150]))
                cpu_load = 50.0 
                
            # --- RAM ---
            ram_out = SERVER_STA.cmd("free -m | awk 'NR==2{printf \"%.2f\", $3*100/$2}'")
            if ram_out and ram_out.strip():
                ram_load = float(ram_out.strip())
            else:
                info("  [DEBUG] RAM parsing failed. Output: '{}'\n".format(ram_out.strip()[:100]))
                ram_load = 50.0
        except Exception as e:
            info("  [DEBUG] Server resources error: {}\n".format(e))
            cpu_load = 50.0
            ram_load = 50.0
        return cpu_load, ram_load

    def calculate_state_label(lat, loss, drops, bw, rssi, cpu, ram):
        if (lat > 50 or loss > 5 or drops > 1000 or (bw > 0 and bw < 2) or rssi < -75 or 
            cpu > 90 or ram > 90):
            return "HIGH"
        elif (lat > 20 or loss > 1 or rssi < -65 or cpu > 70 or ram > 70):
            return "MEDIUM"
        else:
            return "LOW"

    # --- Pre-run debug test ---
    info("\n=== DEBUG: Testing commands once before profiling ===\n")
    info("Ping test: {}\n".format(CLIENT_STA.cmd('ping -c 2 -W 1 ' + SERVER_IP)[:200]))
    
    # ✅ Use iwconfig for RSSI test
    info("RSSI test: {}\n".format(CLIENT_STA.cmd('iwconfig {}-wlan0 | grep -i signal'.format(CLIENT_STA.name))[:100]))
    
    info("CPU test: {}\n".format(SERVER_STA.cmd('top -bn1 | grep Cpu')[:150]))
    info("Switch test: {}\n".format(SWITCH_AP.cmd('ovs-ofctl dump-ports ' + SWITCH_AP.name)[:200]))
    info("=== END DEBUG ===\n")

    # --- Main Logger Loop ---
    start_time = time.time()
    iteration = 0
    prev_tx, prev_rx = 0, 0

    with open(OUTPUT_FILE, "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow([
            "timestamp", "latency_ms", "jitter_ms", "loss_pct", 
            "bandwidth_mbps", "queue_drops", "delta_tx_bytes", "delta_rx_bytes", 
            "rssi_dbm", "server_cpu_pct", "server_ram_pct", "congestion_state"
        ])
        
        try:
            while (time.time() - start_time) < DURATION:
                loop_start = time.time()
                iteration += 1
                
                # 1. Network Metrics
                lat, jit, loss = get_ping_stats()
                tx, rx, drops = get_switch_stats()
                rssi = get_wireless_rssi()
                
                # 2. Server Compute Metrics
                cpu, ram = get_server_resources()
                
                # 3. Calculate Delta (Throughput)
                delta_tx = tx - prev_tx
                delta_rx = rx - prev_rx  # ✅ Fixed: was rx - prev_tx (typo)
                prev_tx, prev_rx = tx, rx
                
                # 4. Bandwidth Probe (only every N iterations)
                bw = 0.0
                if iteration % IPERF_FREQ == 0:
                    bw = get_bandwidth()
                else:
                    bw = -1.0  # Mark as "not measured"
                
                # 5. Congestion Label
                label = calculate_state_label(lat, loss, drops, bw, rssi, cpu, ram)
                
                # 6. Log to CSV
                now = int(time.time() - start_time)
                writer.writerow([
                    now, lat, jit, loss, bw, drops, delta_tx, delta_rx, 
                    rssi, cpu, ram, label
                ])
                f.flush()
                
                # 7. Console Output
                info("[{}s] Lat={:.2f}ms Loss={:.1f}% RSSI={}dBm CPU={:.1f}% RAM={:.1f}% | {}\n".format(
                    now, lat, loss, rssi, cpu, ram, label))
                
                # 8. Sleep Compensation
                elapsed = time.time() - loop_start
                sleep_time = max(0, INTERVAL - elapsed)
                time.sleep(sleep_time)
                
        except KeyboardInterrupt:
            info("\n*** Profiler stopped by user.\n")
        except Exception as e:
            import traceback
            info("\n*** Critical Profiler Error: {}\n".format(e))
            info("*** Traceback: {}\n".format(traceback.format_exc()))
        finally:
            info("*** Data saved to {}\n".format(OUTPUT_FILE))

    # =============================================================================
    # === START CLI (Optional: for manual testing after profiling) ===
    # =============================================================================
    info("*** Starting CLI (type 'exit' to stop network)\n")
    CLI(net)

    info("*** Stopping network\n")
    net.stop()

# =============================================================================
# === MAIN ENTRY POINT ===
# =============================================================================
if __name__ == '__main__':
    setLogLevel('info')
    topology()
