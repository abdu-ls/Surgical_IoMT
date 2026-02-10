/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * Minimal Surgical IoMT Simulation - NS-3.43 Compatible
 * No abstract class errors, no missing Default() methods
 */

#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/wifi-module.h"
#include "ns3/mobility-module.h"
#include "ns3/internet-module.h"
#include "ns3/applications-module.h"

using namespace ns3;

int main (int argc, char *argv[])
{
  double simulationTime = 10.0;
  CommandLine cmd;
  cmd.AddValue ("simulationTime", "Simulation time (seconds)", simulationTime);
  cmd.Parse (argc, argv);

  // ========== 1. Create Nodes ==========
  NodeContainer devices;
  devices.Create (4);  // 0: Robot, 1: Endoscope, 2: Vital, 3: Server (AP)

  // ========== 2. Wi-Fi Setup (802.11ax) ==========
  // Channel setup (required)
  YansWifiChannelHelper channelHelper = YansWifiChannelHelper::Default ();
  YansWifiPhyHelper phyHelper;
  phyHelper.SetChannel (channelHelper.Create ());

  // MAC setup
  WifiMacHelper macHelper;
  Ssid ssid = Ssid ("Smart-OR");

  // Wi-Fi standard
  WifiHelper wifiHelper;
  wifiHelper.SetStandard (WIFI_STANDARD_80211ax);  // ✅ Correct enum
  wifiHelper.SetRemoteStationManager ("ns3::ConstantRateWifiManager");

  // AP (Server = node 3)
  macHelper.SetType ("ns3::ApWifiMac", "Ssid", SsidValue (ssid));
  NetDeviceContainer apDevice = wifiHelper.Install (phyHelper, macHelper, devices.Get (3));

  // Stations (nodes 0, 1, 2)
  macHelper.SetType ("ns3::StaWifiMac", "Ssid", SsidValue (ssid),
                     "ActiveProbing", BooleanValue (false));
  NetDeviceContainer staDevices = wifiHelper.Install (phyHelper, macHelper, NodeContainer (devices.Get (0), devices.Get (1), devices.Get (2)));

  // ========== 3. Mobility (Fixed OR layout) ==========
  MobilityHelper mobility;
  Ptr<ListPositionAllocator> posAlloc = CreateObject<ListPositionAllocator> ();
  posAlloc->Add (Vector (0.0, 0.0, 0.0));   // Robot
  posAlloc->Add (Vector (5.0, 0.0, 0.0));   // Endoscope
  posAlloc->Add (Vector (2.5, 4.0, 0.0));   // Vital monitor
  posAlloc->Add (Vector (2.5, 2.0, 0.0));   // Server (center)
  mobility.SetPositionAllocator (posAlloc);
  mobility.SetMobilityModel ("ns3::ConstantPositionMobilityModel");
  mobility.Install (devices);

  // ========== 4. Internet Stack ==========
  InternetStackHelper stack;
  stack.Install (devices);

  Ipv4AddressHelper address;
  address.SetBase ("192.168.1.0", "255.255.255.0");
  Ipv4InterfaceContainer staInterfaces = address.Assign (staDevices);
  Ipv4InterfaceContainer apInterface = address.Assign (apDevice);

  // ========== 5. Applications ==========
  uint16_t port = 8080;
  
  // Server (echo)
  UdpEchoServerHelper server (port);
  ApplicationContainer serverApp = server.Install (devices.Get (3));
  serverApp.Start (Seconds (1.0));
  serverApp.Stop (Seconds (simulationTime));

  // Robot client (critical traffic)
  UdpEchoClientHelper robotClient (apInterface.GetAddress (0), port);
  robotClient.SetAttribute ("MaxPackets", UintegerValue (100));
  robotClient.SetAttribute ("Interval", TimeValue (MilliSeconds (10)));
  robotClient.SetAttribute ("PacketSize", UintegerValue (64));
  ApplicationContainer robotApp = robotClient.Install (devices.Get (0));
  robotApp.Start (Seconds (2.0));
  robotApp.Stop (Seconds (simulationTime));

  // ========== 6. Run Simulation ==========
  Simulator::Stop (Seconds (simulationTime));
  Simulator::Run ();
  Simulator::Destroy ();

  std::cout << "\n✅ Simulation completed successfully!" << std::endl;
  std::cout << "Devices: Robot (0), Endoscope (1), Vital (2), Server/AP (3)" << std::endl;
  std::cout << "Network: Wi-Fi 6 (802.11ax) with fixed OR topology" << std::endl;
  return 0;
}
