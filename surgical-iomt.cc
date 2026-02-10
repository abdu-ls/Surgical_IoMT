/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * Surgical IoMT Network Simulation
 * Models 3 surgical devices sharing Wi-Fi 6 in an OR:
 *   1. Robotic surgical controller (ultra-low latency UDP)
 *   2. Endoscope video stream (high-bandwidth UDP)
 *   3. Patient vital signs monitor (periodic UDP)
 *
 * Outputs: latency, jitter, packet loss per traffic class
 */

#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/wifi-module.h"
#include "ns3/mobility-module.h"
#include "ns3/internet-module.h"
#include "ns3/applications-module.h"
#include "ns3/flow-monitor-module.h"
#include "ns3/ipv4-flow-classifier.h"

using namespace ns3;

NS_LOG_COMPONENT_DEFINE ("SurgicalIoMT");

int 
main (int argc, char *argv[])
{
  bool verbose = true;
  double simulationTime = 10.0; // seconds

  CommandLine cmd (__FILE__);
  cmd.AddValue ("verbose", "Enable verbose output", verbose);
  cmd.AddValue ("simulationTime", "Simulation time (seconds)", simulationTime);
  cmd.Parse (argc, argv);

  if (verbose)
    {
      LogComponentEnable ("UdpEchoClientApplication", LOG_LEVEL_INFO);
      LogComponentEnable ("UdpEchoServerApplication", LOG_LEVEL_INFO);
    }

  // ========== 1. Create Nodes ==========
  NodeContainer surgicalDevices;
  surgicalDevices.Create (3); // Robot controller, Endoscope, Vital monitor
  NodeContainer edgeServer;
  edgeServer.Create (1);      // Local OR edge server
  NodeContainer cloudServer;
  cloudServer.Create (1);     // Remote cloud (optional)

  // ========== 2. Wi-Fi Setup (802.11ax for modern OR) ==========
  WifiHelper wifi;
  wifi.SetStandard (WIFI_STANDARD_802_11ax);
  wifi.SetRemoteStationManager ("ns3::ConstantRateWifiManager",
                                "DataMode", StringValue ("HeMcs7"));

  WifiMacHelper mac;
  Ssid ssid = Ssid ("Smart-OR-Network");
  
  // Access Point (Edge Server acts as AP)
  mac.SetType ("ns3::ApWifiMac", "Ssid", SsidValue (ssid));
  NetDeviceContainer apDevice;
  apDevice = wifi.Install (wifi, mac, edgeServer.Get (0));

  // Surgical devices as stations
  mac.SetType ("ns3::StaWifiMac", "Ssid", SsidValue (ssid),
               "ActiveProbing", BooleanValue (false));
  NetDeviceContainer staDevices;
  staDevices = wifi.Install (wifi, mac, surgicalDevices);

  // ========== 3. Mobility (fixed positions in OR) ==========
  MobilityHelper mobility;
  Ptr<ListPositionAllocator> positionAlloc = CreateObject<ListPositionAllocator> ();
  // OR layout: devices around surgical table
  positionAlloc->Add (Vector (0.0, 0.0, 0.0));   // Robot controller
  positionAlloc->Add (Vector (2.0, 0.0, 0.0));   // Endoscope
  positionAlloc->Add (Vector (1.0, 2.0, 0.0));   // Vital monitor
  positionAlloc->Add (Vector (1.0, 1.0, 0.0));   // Edge server (center)
  positionAlloc->Add (Vector (20.0, 20.0, 0.0)); // Cloud (remote)
  mobility.SetPositionAllocator (positionAlloc);
  mobility.SetMobilityModel ("ns3::ConstantPositionMobilityModel");
  mobility.Install (surgicalDevices);
  mobility.Install (edgeServer);
  mobility.Install (cloudServer);

  // ========== 4. Internet Stack ==========
  InternetStackHelper stack;
  stack.Install (surgicalDevices);
  stack.Install (edgeServer);
  stack.Install (cloudServer);

  // Assign IP addresses
  Ipv4AddressHelper address;
  address.SetBase ("192.168.1.0", "255.255.255.0");
  Ipv4InterfaceContainer deviceInterfaces = address.Assign (staDevices);
  Ipv4InterfaceContainer edgeInterface = address.Assign (apDevice);
  address.SetBase ("10.1.1.0", "255.255.255.0");
  Ipv4InterfaceContainer cloudInterface = address.Assign (
      NetDeviceContainer ()); // Placeholder - cloud connected via point-to-point in real deployments

  // ========== 5. Applications ==========
  uint16_t robotPort = 8000;    // Robotic controller (critical)
  uint16_t videoPort = 8001;    // Endoscope video
  uint16_t vitalPort = 8002;    // Vital signs

  // --- Edge Server: Echo servers for all ports ---
  UdpEchoServerHelper robotServer (robotPort);
  UdpEchoServerHelper videoServer (videoPort);
  UdpEchoServerHelper vitalServer (vitalPort);

  ApplicationContainer serverApps;
  serverApps.Add (robotServer.Install (edgeServer.Get (0)));
  serverApps.Add (videoServer.Install (edgeServer.Get (0)));
  serverApps.Add (vitalServer.Install (edgeServer.Get (0)));
  serverApps.Start (Seconds (0.5));
  serverApps.Stop (Seconds (simulationTime));

  // --- Surgical Robot Controller: Ultra-low latency UDP (10ms interval) ---
  UdpEchoClientHelper robotClient (edgeInterface.GetAddress (0), robotPort);
  robotClient.SetAttribute ("MaxPackets", UintegerValue (1000));
  robotClient.SetAttribute ("Interval", TimeValue (MilliSeconds (10))); // 100 Hz control loop
  robotClient.SetAttribute ("PacketSize", UintegerValue (64));          // Small control packets
  ApplicationContainer robotApps = robotClient.Install (surgicalDevices.Get (0));
  robotApps.Start (Seconds (1.0));
  robotApps.Stop (Seconds (simulationTime));

  // --- Endoscope Video: High-bandwidth UDP stream (4K @ 30fps ≈ 15 Mbps) ---
  UdpEchoClientHelper videoClient (edgeInterface.GetAddress (0), videoPort);
  videoClient.SetAttribute ("MaxPackets", UintegerValue (10000));
  videoClient.SetAttribute ("Interval", TimeValue (MicroSeconds (66667))); // ~15 packets/ms for 15 Mbps
  videoClient.SetAttribute ("PacketSize", UintegerValue (1400));           // MTU-sized packets
  ApplicationContainer videoApps = videoClient.Install (surgicalDevices.Get (1));
  videoApps.Start (Seconds (1.5));
  videoApps.Stop (Seconds (simulationTime));

  // --- Vital Signs Monitor: Periodic telemetry (1 packet/sec) ---
  UdpEchoClientHelper vitalClient (edgeInterface.GetAddress (0), vitalPort);
  vitalClient.SetAttribute ("MaxPackets", UintegerValue (100));
  vitalClient.SetAttribute ("Interval", TimeValue (Seconds (1.0)));
  vitalClient.SetAttribute ("PacketSize", UintegerValue (100));
  ApplicationContainer vitalApps = vitalClient.Install (surgicalDevices.Get (2));
  vitalApps.Start (Seconds (2.0));
  vitalApps.Stop (Seconds (simulationTime));

  // ========== 6. Flow Monitor (for per-flow metrics) ==========
  FlowMonitorHelper flowmon;
  Ptr<FlowMonitor> monitor = flowmon.InstallAll ();

  // ========== 7. Run Simulation ==========
  Simulator::Stop (Seconds (simulationTime));
  Simulator::Run ();

  // ========== 8. Output Results ==========
  monitor->CheckForLostPackets ();
  Ptr<Ipv4FlowClassifier> classifier = DynamicCast<Ipv4FlowClassifier> (flowmon.GetClassifier ());
  std::map<FlowId, FlowMonitor::FlowStats> stats = monitor->GetFlowStats ();

  std::cout << "\n===== SURGICAL IOMT NETWORK RESULTS =====" << std::endl;
  std::cout << "Simulation Time: " << simulationTime << " seconds" << std::endl;
  std::cout << "Devices: Robotic Controller | Endoscope Video | Vital Monitor" << std::endl;
  std::cout << "Network: Wi-Fi 6 (802.11ax) OR Infrastructure" << std::endl;
  std::cout << "\n--- Per-Flow Performance Metrics ---" << std::endl;

  for (auto& flow : stats)
    {
      Ipv4FlowClassifier::FiveTuple t = classifier->FindFlow (flow.first);
      std::cout << "\nFlow ID: " << flow.first << std::endl;
      std::cout << "  Source: " << t.sourceAddress << " -> Dest: " << t.destinationAddress << std::endl;
      std::cout << "  Port: " << t.destinationPort << std::endl;
      
      // Identify traffic type by port
      std::string trafficType;
      if (t.destinationPort == robotPort) trafficType = "ROBOTIC CONTROL (Critical)";
      else if (t.destinationPort == videoPort) trafficType = "ENDOSCOPE VIDEO (High-BW)";
      else if (t.destinationPort == vitalPort) trafficType = "VITAL SIGNS (Telemetry)";
      else trafficType = "UNKNOWN";
      
      std::cout << "  Type: " << trafficType << std::endl;
      std::cout << "  Tx Packets: " << flow.second.txPackets << std::endl;
      std::cout << "  Rx Packets: " << flow.second.rxPackets << std::endl;
      
      double lossRate = 0.0;
      if (flow.second.txPackets > 0)
        lossRate = (1.0 - (double)flow.second.rxPackets / flow.second.txPackets) * 100.0;
      std::cout << "  Packet Loss: " << lossRate << "%" << std::endl;
      
      if (flow.second.rxPackets > 0)
        {
          double delayMs = flow.second.delaySum.GetMilliSeconds () / flow.second.rxPackets;
          double jitterMs = flow.second.jitterSum.GetMilliSeconds () / flow.second.rxPackets;
          std::cout << "  Avg Latency: " << delayMs << " ms" << std::endl;
          std::cout << "  Avg Jitter: " << jitterMs << " ms" << std::endl;
          
          // Surgical safety thresholds (per literature)
          if (trafficType.find("ROBOTIC") != std::string::npos)
            {
              if (delayMs > 50.0 || lossRate > 1.0)
                std::cout << "  ⚠️  WARNING: Exceeds surgical safety thresholds (latency <50ms, loss <1%)" << std::endl;
              else
                std::cout << "  ✅ Within surgical safety thresholds" << std::endl;
            }
        }
    }

  std::cout << "\n===== END OF SIMULATION =====" << std::endl;
  Simulator::Destroy ();
  return 0;
}
