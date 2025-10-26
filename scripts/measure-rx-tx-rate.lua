-- RX/TX Rate Measurement Script (based on measure-tx-rate.lua)

-- Since we run from Pktgen-DPDK directory, just add current directory to path
package.path = package.path .. ";./?.lua;?.lua;test/?.lua;app/?.lua;"

require "Pktgen"

local port = 0
local sleeptime = tonumber(os.getenv("PKTGEN_DURATION")) or 10

pktgen.stop(port)
pktgen.clear(port)
pktgen.clr()
pktgen.delay(100)

-- Configuration (same as measure-tx-rate.lua)
pktgen.set(port, "size", 64)
pktgen.set(port, "rate", 100)  -- Maximum rate
pktgen.set(port, "count", 0)   -- Continuous transmission

-- Set MAC addresses (same as measure-tx-rate.lua)
pktgen.set_mac(port, "src", "08:c0:eb:b6:cd:5d")
pktgen.set_mac(port, "dst", "08:c0:eb:b6:e8:05")

-- Set IP addresses (same as measure-tx-rate.lua)
pktgen.set_ipaddr(port, "src", "10.0.1.7")
pktgen.set_ipaddr(port, "dst", "10.0.1.8/24")

-- Set up Range configuration for TCP (same as measure-tx-rate.lua)
pktgen.range.ip_proto("all", "tcp")

-- Set MAC addresses in range (same as measure-tx-rate.lua)
pktgen.range.src_mac(port, "start", "08:c0:eb:b6:cd:5d")
pktgen.range.dst_mac(port, "start", "08:c0:eb:b6:e8:05")

-- Set source IP (fixed, same as measure-tx-rate.lua)
pktgen.range.src_ip(port, "start", "10.0.1.7")
pktgen.range.src_ip(port, "inc", "0.0.0.0")
pktgen.range.src_ip(port, "min", "10.0.1.7")
pktgen.range.src_ip(port, "max", "10.0.1.7")

-- Set destination IP (fixed, same as measure-tx-rate.lua)
pktgen.range.dst_ip(port, "start", "10.0.1.8")
pktgen.range.dst_ip(port, "inc", "0.0.0.0")
pktgen.range.dst_ip(port, "min", "10.0.1.8")
pktgen.range.dst_ip(port, "max", "10.0.1.8")

-- Set source TCP port (20000-20255, increment by 1, same as measure-tx-rate.lua)
pktgen.range.src_port(port, "start", 20000)
pktgen.range.src_port(port, "inc", 1)
pktgen.range.src_port(port, "min", 10000)
pktgen.range.src_port(port, "max", 60000)

-- Set destination TCP port (fixed at 20000, same as measure-tx-rate.lua)
pktgen.range.dst_port(port, "start", 20000)
pktgen.range.dst_port(port, "inc", 0)
pktgen.range.dst_port(port, "min", 20000)
pktgen.range.dst_port(port, "max", 20000)

-- Set TTL (same as measure-tx-rate.lua)
pktgen.range.ttl(port, "start", 64)
pktgen.range.ttl(port, "inc", 0)
pktgen.range.ttl(port, "min", 64)
pktgen.range.ttl(port, "max", 64)

-- Enable range mode (same as measure-tx-rate.lua)
pktgen.set_range(port, "on")

pktgen.delay(100)

print("=== Starting RX/TX Rate Measurement ===")
print("Configuration:")
print("  Port: " .. port)
print("  Packet Size: 64 bytes")
print("  Protocol: TCP")
print("  Test Duration: " .. sleeptime .. " seconds")
print("  Rate: 100% (maximum)")
print("  Range Mode: Enabled (Port 10000-60000)")
print("==========================================")

-- Start transmission
print("Starting packet transmission for " .. sleeptime .. " seconds...")
pktgen.start(port)

-- Record initial statistics
pktgen.delay(1000)  -- Wait 1 second for transmission to stabilize
local initial_stats = pktgen.portStats("all", "port")[port]
local initial_tx_pkts = initial_stats.opackets
local initial_rx_pkts = initial_stats.ipackets

-- Wait for test duration
pktgen.delay(sleeptime * 1000)

-- Stop transmission and get final statistics
pktgen.stop(port)
local final_stats = pktgen.portStats("all", "port")[port]
local final_tx_pkts = final_stats.opackets
local final_rx_pkts = final_stats.ipackets

-- Calculate average rates in Mpps
local total_tx_packets = final_tx_pkts - initial_tx_pkts
local total_rx_packets = final_rx_pkts - initial_rx_pkts
local avg_tx_rate_mpps = total_tx_packets / (sleeptime * 1000000)
local avg_rx_rate_mpps = total_rx_packets / (sleeptime * 1000000)

-- Calculate packet loss percentage
local packet_loss_pct = 0
if total_tx_packets > 0 then
    packet_loss_pct = (1 - total_rx_packets/total_tx_packets) * 100
end

-- Display results
print("\n=== RX/TX Rate Measurement Results ===")
print("Test Duration: " .. sleeptime .. " seconds")
print("Packet Size: 64 bytes")
print("Protocol: TCP with ranges")
print("Total Packets Transmitted: " .. total_tx_packets)
print("Total Packets Received: " .. total_rx_packets)
print("Average TX Rate: " .. string.format("%.3f", avg_tx_rate_mpps) .. " Mpps")
print("Average RX Rate: " .. string.format("%.3f", avg_rx_rate_mpps) .. " Mpps")
print("Packet Loss: " .. string.format("%.2f", packet_loss_pct) .. "%")
print("=====================================")

-- Print results in parseable format for automation
print("RESULT_TX_RATE_MPPS:" .. string.format("%.3f", avg_tx_rate_mpps))
print("RESULT_RX_RATE_MPPS:" .. string.format("%.3f", avg_rx_rate_mpps))
print("RESULT_TX_PACKETS:" .. total_tx_packets)
print("RESULT_RX_PACKETS:" .. total_rx_packets)

-- Auto quit to prevent interactive mode - use os.exit instead of pktgen.quit()
print("Auto-quitting pktgen...")
pktgen.delay(1000)  -- Wait 1 second before quitting
os.exit(0)
