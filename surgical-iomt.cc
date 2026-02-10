/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * Surgical IoMT Network Simulation with NetAnim Visualization
 * Devices: Robotic controller, Endoscope, Vital monitor → Edge server (Wi-Fi 6)
 */

#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/wifi-module.h"
#include "ns3/mobility-module.h"
#include "ns3/internet-module.h"
#include "ns3/applications-module.h"
#include "ns3/netanim-module.h"  // <-- Required for NetAnim

using namespace ns3;

NS_LOG_COMPONENT_DEFINE ("SurgicalIoMTNetAnim");

int 
main (int argc, char *argv[])
{
  double simulationTime = 15.0; // Extended for better animation viewing

  CommandLine cmd (__FILE__);
  cmd.AddValue ("simulationTime", "Simulation time (seconds)", simulationTime);
  cmd.Parse (argc, argv);

  // ========== 1. Create Nodes ==========
  NodeContainer surgicalDevices;
  surgicalDevices.Create (3);   // 0: Robot, 1: Endoscope, 2: Vital monitor
  NodeContainer edgeServer;
  edgeServer.Create (1);        // 3: Edge server (AP)

  // ========== 2. Wi-Fi Setup (802.11ax) ==========
  WifiHelper wifi;
  wifi.SetStandard (WIFI_STANDARD_802_11ax);
  wifi.SetRemoteStationManager ("ns3::ConstantRateWifiManager",
                                "DataMode", StringValue ("HeMcs7"));

  WifiMacHelper mac;
  Ssid ssid = Ssid ("Smart-OR-Network");
  
  // AP (Edge server)
  mac.SetType ("ns3::ApWifiMac", "Ssid", SsidValue (ssid));
  NetDeviceContainer apDevice = wifi.Install (wifi, mac, edgeServer.Get (0));

  // Stations (Surgical devices)
  mac.SetType ("ns3::StaWifiMac", "Ssid", SsidValue (ssid),
               "ActiveProbing", BooleanValue (false));
  NetDeviceContainer staDevices = wifi.Install (wifi, mac, surgicalDevices);

  // ========== 3. Mobility (Fixed OR layout) ==========
  MobilityHelper mobility;
  Ptr<ListPositionAllocator> positionAlloc = CreateObject<ListPositionAllocator> ();
  positionAlloc->Add (Vector (0.0, 0.0, 0.0));   // Robot controller (left)
  positionAlloc->Add (Vector (5.0, 0.0, 0.0));   // Endoscope (right)
  positionAlloc->Add (Vector (2.5, 4.0, 0.0));   // Vital monitor (top)
  positionAlloc->Add (Vector (2.5, 2.0, 0.0));   // Edge server (center - surgical table)
  mobility.SetPositionAllocator (positionAlloc);
  mobility.SetMobilityModel ("ns3::ConstantPositionMobilityModel");
  mobility.Install (surgicalDevices);
  mobility.Install (edgeServer);

  // ========== 4. Enable NetAnim Tracing ==========
  AnimationInterface anim ("surgical-iomt.xml");
  anim.SetMaxPktsPerTraceFile (100000);  // Prevent trace file overflow
  
  // Customize node appearance in NetAnim
  anim.UpdateNodeDescription (0, "Robot Controller");
  anim.UpdateNodeDescription (1, "Endoscope");
  anim.UpdateNodeDescription (2, "Vital Monitor");
  anim.UpdateNodeDescription (3, "Edge Server (AP)");
  
  anim.UpdateNodeColor (0, 255, 0, 0);     // Red = Critical control
  anim.UpdateNodeColor (1, 0, 0, 255);     // Blue = Video
  anim.UpdateNodeColor (2, 0, 255, 0);     // Green = Telemetry
  anim.UpdateNodeColor (3, 128, 128, 128); // Gray = Server
  
  anim.UpdateNodeSize (0, 15, 15);
  anim.UpdateNodeSize (1, 15, 15);
  anim.UpdateNodeSize (2, 15, 15);
  anim.UpdateNodeSize (3, 25, 25);         // Larger server node

  // ========== 5. Internet Stack ==========
  InternetStackHelper stack;
  stack.Install (surgicalDevices);
  stack.Install (edgeServer);

  Ipv4AddressHelper address;
  address.SetBase ("192.168.1.0", "255.255.255.0");
  Ipv4InterfaceContainer deviceInterfaces = address.Assign (staDevices);
  Ipv4InterfaceContainer edgeInterface = address.Assign (apDevice);

  // ========== 6. Applications (same as before) ==========
  uint16_t robotPort = 8000;
  uint16_t videoPort = 8001;
  uint16_t vitalPort = 8002;

  // Servers on edge
  UdpEchoServerHelper robotServer (robotPort);
  UdpEchoServerHelper videoServer (videoPort);
  UdpEchoServerHelper vitalServer (vitalPort);
  ApplicationContainer serverApps;
  serverApps.Add (robotServer.Install (edgeServer.Get (0)));
  serverApps.Add (videoServer.Install (edgeServer.Get (0)));
  serverApps.Add (vitalServer.Install (edgeServer.Get (0)));
  serverApps.Start (Seconds (1.0));
  serverApps.Stop (Seconds (simulationTime));

  // Robot controller (10ms interval)
  UdpEchoClientHelper robotClient (edgeInterface.GetAddress (0), robotPort);
  robotClient.SetAttribute ("MaxPackets", UintegerValue (1000));
  robotClient.SetAttribute ("Interval", TimeValue (MilliSeconds (10)));
  robotClient.SetAttribute ("PacketSize", UintegerValue (64));
  ApplicationContainer robotApps = robotClient.Install (surgicalDevices.Get (0));
  robotApps.Start (Seconds (2.0));
  robotApps.Stop (Seconds (simulationTime));

  // Endoscope video (high bandwidth)
  UdpEchoClientHelper videoClient (edgeInterface.GetAddress (0), videoPort);
  videoClient.SetAttribute ("MaxPackets", UintegerValue (5000));
  videoClient.SetAttribute ("Interval", TimeValue (MicroSeconds (66667)));
  videoClient.SetAttribute ("PacketSize", UintegerValue (1400));
  ApplicationContainer videoApps = videoClient.Install (surgicalDevices.Get (1));
  videoApps.Start (Seconds (3.0));
  videoApps.Stop (Seconds (simulationTime));

  // Vital signs (1 Hz)
  UdpEchoClientHelper vitalClient (edgeInterface.GetAddress (0), vitalPort);
  vitalClient.SetAttribute ("MaxPackets", UintegerValue (100));
  vitalClient.SetAttribute ("Interval", TimeValue (Seconds (1.0)));
  vitalClient.SetAttribute ("PacketSize", UintegerValue (100));
  ApplicationContainer vitalApps = vitalClient.Install (surgicalDevices.Get (2));
  vitalApps.Start (Seconds (4.0));
  vitalApps.Stop (Seconds (simulationTime));

  // ========== 7. Run Simulation ==========
  Simulator::Stop (Seconds (simulationTime));
  Simulator::Run ();
  Simulator::Destroy ();

  std::cout << "\n✅ Simulation complete. Trace file: surgical-iomt.xml" << std::endl;
  std::cout << "👉 Open this file in NetAnim to view animation." << std::endl;
  return 0;
}
