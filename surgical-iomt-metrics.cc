/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * Surgical IoMT Simulation with Latency + Task Completion Metrics + CSV Export
 * NS-3.43 Compatible | Struct definition order fixed
 */

#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/wifi-module.h"
#include "ns3/mobility-module.h"
#include "ns3/internet-module.h"
#include "ns3/applications-module.h"
#include "ns3/flow-monitor-module.h"
#include "ns3/ipv4-flow-classifier.h"
#include "ns3/netanim-module.h"
#include <iomanip>
#include <map>
#include <fstream>  // For CSV export

using namespace ns3;

NS_LOG_COMPONENT_DEFINE ("SurgicalIoMTMetrics");

// ===== CRITICAL FIX: Define struct BEFORE using it in functions =====
struct DeviceMetrics {
  std::string name;
  uint32_t txPackets;
  uint32_t rxPackets;
  double lossRate;
  double avgLatencyMs;
  double avgJitterMs;
  double taskCompletionTime;
  bool taskCompleted;
};

// Function to export metrics to CSV (now safe - struct is fully defined)
void ExportMetricsToCSV (const std::vector<DeviceMetrics>& results,
                         const std::map<std::string, uint32_t>& taskTargets)
{
  std::ofstream csvFile ("surgical_metrics.csv");
  if (!csvFile.is_open ())
    {
      NS_LOG_ERROR ("Failed to open surgical_metrics.csv for writing");
      return;
    }

  // CSV header
  csvFile << "Device,TxPackets,RxPackets,LossPercent,AvgLatencyMs,AvgJitterMs,"
          << "TaskTargetPackets,TaskCompleted,TaskCompletionTimeSec,SuccessRatePercent\n";

  // CSV rows
  for (const auto& r : results)
    {
      std::string completed = r.taskCompleted ? "Yes" : "No";
      double successRate = (taskTargets.at(r.name) > 0) ? 
        (double)r.rxPackets / taskTargets.at(r.name) * 100.0 : 0.0;

      csvFile << r.name << ","
              << r.txPackets << ","
              << r.rxPackets << ","
              << r.lossRate << ","
              << r.avgLatencyMs << ","
              << r.avgJitterMs << ","
              << taskTargets.at(r.name) << ","
              << completed << ","
              << r.taskCompletionTime << ","
              << successRate << "\n";
    }

  csvFile.close ();
  std::cout << "\n📊 CSV exported: surgical_metrics.csv\n";
}

int main (int argc, char *argv[])
{
  double simulationTime = 15.0;
  bool enableNetAnim = true;

  CommandLine cmd;
  cmd.AddValue ("simulationTime", "Simulation time (seconds)", simulationTime);
  cmd.AddValue ("enableNetAnim", "Enable NetAnim trace output", enableNetAnim);
  cmd.Parse (argc, argv);

  // ========== 1. Create Nodes ==========
  NodeContainer devices;
  devices.Create (4);  // 0: Robot, 1: Endoscope, 2: Vital, 3: Server (AP)

  // ========== 2. Wi-Fi Setup (802.11ax) ==========
  YansWifiChannelHelper channelHelper = YansWifiChannelHelper::Default ();
  YansWifiPhyHelper phyHelper;
  phyHelper.SetChannel (channelHelper.Create ());

  WifiMacHelper macHelper;
  Ssid ssid = Ssid ("Smart-OR");

  WifiHelper wifiHelper;
  wifiHelper.SetStandard (WIFI_STANDARD_80211ax);
  wifiHelper.SetRemoteStationManager ("ns3::ConstantRateWifiManager");

  macHelper.SetType ("ns3::ApWifiMac", "Ssid", SsidValue (ssid));
  NetDeviceContainer apDevice = wifiHelper.Install (phyHelper, macHelper, devices.Get (3));

  macHelper.SetType ("ns3::StaWifiMac", "Ssid", SsidValue (ssid),
                     "ActiveProbing", BooleanValue (false));
  NetDeviceContainer staDevices = wifiHelper.Install (phyHelper, macHelper, 
                                                      NodeContainer (devices.Get (0), 
                                                                     devices.Get (1), 
                                                                     devices.Get (2)));

  // ========== 3. Mobility (Fixed OR layout) ==========
  MobilityHelper mobility;
  Ptr<ListPositionAllocator> posAlloc = CreateObject<ListPositionAllocator> ();
  posAlloc->Add (Vector (0.0, 0.0, 0.0));   // Robot
  posAlloc->Add (Vector (5.0, 0.0, 0.0));   // Endoscope
  posAlloc->Add (Vector (2.5, 4.0, 0.0));   // Vital
  posAlloc->Add (Vector (2.5, 2.0, 0.0));   // Server
  mobility.SetPositionAllocator (posAlloc);
  mobility.SetMobilityModel ("ns3::ConstantPositionMobilityModel");
  mobility.Install (devices);

  // ========== 4. NetAnim (Optional) ==========
  if (enableNetAnim)
    {
      AnimationInterface anim ("surgical-iomt-metrics.xml");
      anim.UpdateNodeDescription (0, "Robot Ctrl");
      anim.UpdateNodeDescription (1, "Endoscope");
      anim.UpdateNodeDescription (2, "Vital Mon");
      anim.UpdateNodeDescription (3, "Edge Server");
      anim.UpdateNodeColor (0, 255, 0, 0);
      anim.UpdateNodeColor (1, 0, 0, 255);
      anim.UpdateNodeColor (2, 0, 255, 0);
      anim.UpdateNodeColor (3, 128, 128, 128);
    }

  // ========== 5. Internet Stack ==========
  InternetStackHelper stack;
  stack.Install (devices);

  Ipv4AddressHelper address;
  address.SetBase ("192.168.1.0", "255.255.255.0");
  Ipv4InterfaceContainer staInterfaces = address.Assign (staDevices);
  Ipv4InterfaceContainer apInterface = address.Assign (apDevice);

  // ========== 6. FlowMonitor ==========
  FlowMonitorHelper flowmon;
  flowmon.SetMonitorAttribute ("MaxPerFlowPackets", UintegerValue (1000));
  Ptr<FlowMonitor> monitor = flowmon.InstallAll ();

  // ========== 7. Applications ==========
  const uint16_t robotPort = 8000;
  const uint16_t videoPort = 8001;
  const uint16_t vitalPort = 8002;

  // Server
  UdpEchoServerHelper robotServer (robotPort);
  UdpEchoServerHelper videoServer (videoPort);
  UdpEchoServerHelper vitalServer (vitalPort);

  ApplicationContainer serverApps;
  serverApps.Add (robotServer.Install (devices.Get (3)));
  serverApps.Add (videoServer.Install (devices.Get (3)));
  serverApps.Add (vitalServer.Install (devices.Get (3)));
  serverApps.Start (Seconds (1.0));
  serverApps.Stop (Seconds (simulationTime));

  // Robotic Controller
  UdpEchoClientHelper robotClient (apInterface.GetAddress (0), robotPort);
  robotClient.SetAttribute ("MaxPackets", UintegerValue (100));
  robotClient.SetAttribute ("Interval", TimeValue (MilliSeconds (10)));
  robotClient.SetAttribute ("PacketSize", UintegerValue (64));
  ApplicationContainer robotApps = robotClient.Install (devices.Get (0));
  robotApps.Start (Seconds (2.0));
  robotApps.Stop (Seconds (simulationTime));

  // Endoscope
  UdpEchoClientHelper videoClient (apInterface.GetAddress (0), videoPort);
  videoClient.SetAttribute ("MaxPackets", UintegerValue (500));
  videoClient.SetAttribute ("Interval", TimeValue (MicroSeconds (66667)));
  videoClient.SetAttribute ("PacketSize", UintegerValue (1400));
  ApplicationContainer videoApps = videoClient.Install (devices.Get (1));
  videoApps.Start (Seconds (2.5));
  videoApps.Stop (Seconds (simulationTime));

  // Vital Monitor
  UdpEchoClientHelper vitalClient (apInterface.GetAddress (0), vitalPort);
  vitalClient.SetAttribute ("MaxPackets", UintegerValue (15));
  vitalClient.SetAttribute ("Interval", TimeValue (Seconds (1.0)));
  vitalClient.SetAttribute ("PacketSize", UintegerValue (100));
  ApplicationContainer vitalApps = vitalClient.Install (devices.Get (2));
  vitalApps.Start (Seconds (3.0));
  vitalApps.Stop (Seconds (simulationTime));

  // ========== 8. Run Simulation ==========
  Simulator::Stop (Seconds (simulationTime));
  Simulator::Run ();

  // ========== 9. Extract Metrics ==========
  monitor->CheckForLostPackets ();
  Ptr<Ipv4FlowClassifier> classifier = DynamicCast<Ipv4FlowClassifier> (flowmon.GetClassifier ());
  std::map<FlowId, FlowMonitor::FlowStats> stats = monitor->GetFlowStats ();

  // Map device IPs to names and task targets
  std::map<Ipv4Address, std::string> ipToDevice = {
    {Ipv4Address ("192.168.1.1"), "Robot Ctrl"},
    {Ipv4Address ("192.168.1.2"), "Endoscope "},
    {Ipv4Address ("192.168.1.3"), "Vital Mon"}
  };
  std::map<std::string, uint32_t> taskTargets = {
    {"Robot Ctrl", 100},
    {"Endoscope ", 500},
    {"Vital Mon", 15}
  };

  std::vector<DeviceMetrics> results;

  for (auto& flow : stats)
    {
      Ipv4FlowClassifier::FiveTuple t = classifier->FindFlow (flow.first);
      auto it = ipToDevice.find (t.sourceAddress);
      if (it == ipToDevice.end ()) continue;

      std::string deviceName = it->second;
      uint32_t targetPackets = taskTargets[deviceName];

      double lossRate = (flow.second.txPackets > 0) ? 
        (1.0 - (double)flow.second.rxPackets / flow.second.txPackets) * 100.0 : 100.0;

      double avgLatencyMs = (flow.second.rxPackets > 0) ? 
        flow.second.delaySum.GetMilliSeconds() / flow.second.rxPackets : 0.0;

      double avgJitterMs = (flow.second.rxPackets > 0) ? 
        flow.second.jitterSum.GetMilliSeconds() / flow.second.rxPackets : 0.0;

      double taskTimeSec = 0.0;
      if (flow.second.rxPackets > 0 && flow.second.txPackets > 0)
        {
          taskTimeSec = flow.second.timeLastRxPacket.GetSeconds() - flow.second.timeFirstTxPacket.GetSeconds();
        }

      bool completed = (flow.second.rxPackets >= targetPackets);

      results.push_back ({
        deviceName,
        flow.second.txPackets,
        flow.second.rxPackets,
        lossRate,
        avgLatencyMs,
        avgJitterMs,
        taskTimeSec,
        completed
      });
    }

  // ========== 10. Export to CSV ==========
  ExportMetricsToCSV (results, taskTargets);

  // ========== 11. Output Results to Terminal ==========
  std::cout << "\n";
  std::cout << "╔════════════════════════════════════════════════════════════════════════════════╗\n";
  std::cout << "║        SURGICAL IOMT NETWORK METRICS - LATENCY & TASK COMPLETION            ║\n";
  std::cout << "╚════════════════════════════════════════════════════════════════════════════════╝\n";
  std::cout << "\n";

  // Latency/Jitter/Loss Table
  std::cout << "┌──────────────┬──────────┬──────────┬──────────┬──────────┬──────────┐\n";
  std::cout << "│ Device       │ Tx Pkts  │ Rx Pkts  │ Loss (%) │ Latency  │ Jitter   │\n";
  std::cout << "│              │          │          │          │ (ms)     │ (ms)     │\n";
  std::cout << "├──────────────┼──────────┼──────────┼──────────┼──────────┼──────────┤\n";
  for (auto& r : results)
    {
      std::cout << "│ " << std::left << std::setw(12) << r.name
                << " │ " << std::right << std::setw(8) << r.txPackets
                << " │ " << std::setw(8) << r.rxPackets
                << " │ " << std::setw(8) << std::fixed << std::setprecision(2) << r.lossRate
                << " │ " << std::setw(8) << std::fixed << std::setprecision(2) << r.avgLatencyMs
                << " │ " << std::setw(8) << std::fixed << std::setprecision(2) << r.avgJitterMs
                << " │\n";
    }
  std::cout << "└──────────────┴──────────┴──────────┴──────────┴──────────┴──────────┘\n";

  // Task Completion Table
  std::cout << "\n";
  std::cout << "┌──────────────┬──────────────┬──────────────┬──────────────┬──────────────┐\n";
  std::cout << "│ Device       │ Task Target  │ Completed?   │ Completion   │ Success      │\n";
  std::cout << "│              │ (packets)    │              │ Time (s)     │ Rate (%)     │\n";
  std::cout << "├──────────────┼──────────────┼──────────────┼──────────────┼──────────────┤\n";
  for (auto& r : results)
    {
      std::string status = r.taskCompleted ? "✅ Yes" : (r.rxPackets > 0 ? "⚠️ Partial" : "❌ No");
      double successRate = (taskTargets.at(r.name) > 0) ? 
        (double)r.rxPackets / taskTargets.at(r.name) * 100.0 : 0.0;

      std::cout << "│ " << std::left << std::setw(12) << r.name
                << " │ " << std::right << std::setw(12) << taskTargets.at(r.name)
                << " │ " << std::setw(12) << status
                << " │ " << std::setw(12) << std::fixed << std::setprecision(3) << r.taskCompletionTime
                << " │ " << std::setw(12) << std::fixed << std::setprecision(1) << successRate
                << " │\n";
    }
  std::cout << "└──────────────┴──────────────┴──────────────┴──────────────┴──────────────┘\n";

  // Surgical Safety Assessment
  std::cout << "\n";
  std::cout << "┌──────────────────────────────────────────────────────────────────────────────┐\n";
  std::cout << "│ SURGICAL SAFETY ASSESSMENT                                                   │\n";
  std::cout << "├──────────────────────────────────────────────────────────────────────────────┤\n";

  bool allSafe = true;
  for (auto& r : results)
    {
      if (r.name == "Robot Ctrl")
        {
          bool latencySafe = (r.avgLatencyMs < 50.0);
          bool timeSafe = (r.taskCompletionTime < 5.0 && r.taskCompleted);
          
          if (latencySafe && timeSafe)
            std::cout << "│ ✅ ROBOTIC CONTROL: Latency=" << r.avgLatencyMs << "ms (<50ms), Task=" 
                      << r.taskCompletionTime << "s (<5s) → SAFE FOR SURGERY      │\n";
          else
            {
              allSafe = false;
              std::cout << "│ ⚠️  ROBOTIC CONTROL: SAFETY THRESHOLDS EXCEEDED                           │\n";
              if (!latencySafe) std::cout << "│    → Latency " << r.avgLatencyMs << "ms > 50ms surgical limit                │\n";
              if (!timeSafe) std::cout << "│    → Task time " << r.taskCompletionTime << "s > 5s or incomplete           │\n";
            }
        }
    }
  if (allSafe)
    std::cout << "│                                                                              │\n";
  std::cout << "└──────────────────────────────────────────────────────────────────────────────┘\n";

  std::cout << "\n📁 Files generated:\n";
  std::cout << "   • surgical_metrics.csv        (for analysis in Excel/Python)\n";
  if (enableNetAnim)
    std::cout << "   • surgical-iomt-metrics.xml   (open with NetAnim)\n";
  std::cout << "\n💡 Quick analysis tip:\n";
  std::cout << "   python3 -c \"import pandas as pd; df=pd.read_csv('surgical_metrics.csv'); print(df)\"\n";

  Simulator::Destroy ();
  return 0;
}
