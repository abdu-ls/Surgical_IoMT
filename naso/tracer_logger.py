import time
import csv
import re
import subprocess

# --- CONFIGURATION ---
DURATION = 3600       # 1 Hour
INTERVAL = 5          # Target interval between samples
OUTPUT_FILE = "network_trace.csv"
IPERF_FREQ = 5        # Run iperf only every N iterations to reduce load

# --- TOPOLOGY REFERENCES (MUST MATCH YOUR MAIN SCRIPT) ---
# Ensure sta3, sta4, ap1 are defined in your global scope before running this
# sta3 = Client (Surgical Device)
# sta4 = Edge Server (Processing Node)
# ap1  = Access Point / Switch
# --- TOPOLOGY REFERENCES ---
try:
    CLIENT_STA = sta3           # Surgical robot (wireless)
    SERVER_STA = fog1           # Edge server (wired) <-- CHANGED
    SWITCH_AP = ap1         
    SERVER_IP = "10.0.0.100"    # Fog1's IP <-- CHANGED
except NameError:
    print("ERROR: sta3, fog1, or ap1 not defined.")
    exit(1)

# --- HELPER FUNCTIONS ---

def get_ping_stats():
    try:
        # -c 3 (3 packets), -i 0.2 (fast interval) -> ~1 second total
        out = CLIENT_STA.cmd("ping -c 3 -i 0.2 {}".format(SERVER_IP))
        latency = jitter = loss = 0.0
        
        if "packet loss" in out:
            loss_part = out.split("packet loss")[1].split("%")[0]
            loss = float(loss_part.split()[-1])
        
        if "rtt" in out:
            # Format: rtt min/avg/max/mdev = 1.2/3.4/5.6/0.7 ms
            rtt_part = out.split("rtt")[1].split("=")[1].split("/")
            latency = float(rtt_part[1])
            jitter = float(rtt_part[3].split()[0])
            
        return latency, jitter, loss
    except Exception as e:
        return 0.0, 0.0, 100.0 # Return high loss on error

def get_bandwidth():
    try:
        # Use iperf3 if available, else iperf. -t 1 (1 second test)
        out = CLIENT_STA.cmd("iperf3 -c {} -t 1 -P 1 2>/dev/null".format(SERVER_IP))
        # Handle both "Mbits/sec" and "Mbit/sec"
        m = re.search(r"(\d+\.\d+)\s*Mbits?/sec", out)
        return float(m.group(1)) if m else 0.0
    except Exception as e:
        return 0.0

def get_switch_stats():
    try:
        # Call ONCE and parse both queue drops and bytes
        out = SWITCH_AP.cmd("ovs-ofctl dump-ports {}".format(SWITCH_AP))
        
        tx_bytes = rx_bytes = drops = 0
        
        # Parse TX/RX
        tx_match = re.search(r"tx_bytes=(\d+)", out)
        rx_match = re.search(r"rx_bytes=(\d+)", out)
        drop_match = re.search(r"drop=(\d+)", out) # Might vary by OVS version
        
        tx_bytes = int(tx_match.group(1)) if tx_match else 0
        rx_bytes = int(rx_match.group(1)) if rx_match else 0
        drops = int(drop_match.group(1)) if drop_match else 0
        
        return tx_bytes, rx_bytes, drops
    except Exception as e:
        return 0, 0, 0

def get_wireless_rssi():
    try:
        # Get signal strength for wireless station
        # iwdev <iface> station dump | grep signal
        out = CLIENT_STA.cmd("iwdev {} station dump | grep signal".format(CLIENT_STA.name))
        # Output example: "signal: -65 dBm"
        m = re.search(r"signal:\s*(-\d+)", out)
        return int(m.group(1)) if m else -100
    except:
        return -100

def get_server_resources():
    """
    Gets CPU and RAM usage of the Edge Server (sta4).
    Uses standard Linux commands available in Mininet-WiFi Ubuntu images.
    """
    cpu_load = 0.0
    ram_load = 0.0
    
    try:
        # --- CPU ---
        # top -bn1 gives snapshot. grep Cpu(s). awk gets idle % (usually 4th column)
        # Example: "Cpu(s):  2.3%us,  1.0%sy,  0.0%ni, 96.7%id, ..."
        cpu_out = SERVER_STA.cmd("top -bn1 | grep 'Cpu(s)'")
        idle_match = re.search(r"(\d+\.\d+)\%id", cpu_out)
        if idle_match:
            cpu_load = 100.0 - float(idle_match.group(1))
        else:
            # Fallback if 'id' not found (some top versions differ)
            cpu_load = 50.0 
            
        # --- RAM ---
        # free -m | awk 'NR==2{printf "%.2f", $3*100/$2}'
        ram_out = SERVER_STA.cmd("free -m | awk 'NR==2{printf \"%.2f\", $3*100/$2}'")
        if ram_out:
            ram_load = float(ram_out.strip())
            
    except Exception as e:
        # If commands fail, return neutral values so script doesn't crash
        cpu_load = 50.0
        ram_load = 50.0
        
    return cpu_load, ram_load

def calculate_state_label(lat, loss, drops, bw, rssi, cpu, ram):
    """
    Determines congestion/state label based on Network + Compute resources.
    Critical for DQN reward shaping.
    """
    # HIGH RISK: Bad network OR saturated server
    if (lat > 50 or loss > 5 or drops > 1000 or bw < 2 or rssi < -75 or 
        cpu > 90 or ram > 90):
        return "HIGH"
    
    # MEDIUM RISK: Moderate congestion or load
    elif (lat > 20 or loss > 1 or rssi < -65 or 
          cpu > 70 or ram > 70):
        return "MEDIUM"
    
    # LOW RISK: Good conditions
    else:
        return "LOW"

# --- MAIN LOGGER LOOP ---

print(f"Starting Network Profiler for {DURATION}s...")
print(f"Client: {CLIENT_STA.name} | Server: {SERVER_STA.name} | AP: {SWITCH_AP.name}")
start_time = time.time()
iteration = 0

# Initialize previous byte counters for delta calculation
prev_tx, prev_rx = 0, 0

with open(OUTPUT_FILE, "w", newline="") as f:
    writer = csv.writer(f)
    # Updated Header with CPU/RAM
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
            
            # 2. Server Compute Metrics (NEW)
            cpu, ram = get_server_resources()
            
            # Calculate Delta (Throughput) instead of absolute bytes
            delta_tx = tx - prev_tx
            delta_rx = rx - prev_tx
            prev_tx, prev_rx = tx, rx
            
            # 3. Bandwidth Probe (Only every N iterations to avoid saturation)
            bw = 0.0
            if iteration % IPERF_FREQ == 0:
                bw = get_bandwidth()
            else:
                bw = -1.0 # Mark as "not measured"
            
            # 4. Label (Includes CPU/RAM now)
            label = calculate_state_label(lat, loss, drops, bw, rssi, cpu, ram)
            
            # 5. Log
            now = int(time.time() - start_time)
            writer.writerow([
                now, lat, jit, loss, bw, drops, delta_tx, delta_rx, 
                rssi, cpu, ram, label
            ])
            f.flush()
            
            # 6. Console Output
            print(f"[{now}s] Lat={lat:.2f}ms Loss={loss:.1f}% RSSI={rssi}dBm CPU={cpu:.1f}% RAM={ram:.1f}% | {label}")
            
            # 7. Sleep Compensation
            elapsed = time.time() - loop_start
            sleep_time = max(0, INTERVAL - elapsed)
            time.sleep(sleep_time)
            
    except KeyboardInterrupt:
        print("\nProfiler stopped by user.")
    except Exception as e:
        print(f"\nCritical Error: {e}")
    finally:
        print(f"Data saved to {OUTPUT_FILE}")
