-- TX Rate Measurement Script

-- Since we run from Pktgen-DPDK directory, just add current directory to path
package.path = package.path .. ";./?.lua;?.lua;test/?.lua;app/?.lua;"

require "Pktgen"

local port = 0
local sleeptime = tonumber(os.getenv("PKTGEN_DURATION")) or 10

pktgen.stop(port)
pktgen.clear(port)
pktgen.clr()
pktgen.delay(100)

-- Configuration
pktgen.set(port, "size", 64)
pktgen.set(port, "rate", 100)  -- Maximum rate
pktgen.set(port, "count", 0)   -- Continuous transmission

-- Set MAC addresses
pktgen.set_mac(port, "src", "08:c0:eb:b6:cd:5d")
pktgen.set_mac(port, "dst", "08:c0:eb:b6:e8:05")

-- Set IP addresses
pktgen.set_ipaddr(port, "src", "10.0.1.7")
pktgen.set_ipaddr(port, "dst", "10.0.1.8/24")

-- Set up Range configuration for TCP
pktgen.range.ip_proto("all", "tcp")

-- Set MAC addresses in range
pktgen.range.src_mac(port, "start", "08:c0:eb:b6:cd:5d")
pktgen.range.dst_mac(port, "start", "08:c0:eb:b6:e8:05")

-- Set source IP (fixed)
pktgen.range.src_ip(port, "start", "10.0.1.7")
pktgen.range.src_ip(port, "inc", "0.0.0.0")
pktgen.range.src_ip(port, "min", "10.0.1.7")
pktgen.range.src_ip(port, "max", "10.0.1.7")

-- Set destination IP (fixed)
pktgen.range.dst_ip(port, "start", "10.0.1.8")
pktgen.range.dst_ip(port, "inc", "0.0.0.0")
pktgen.range.dst_ip(port, "min", "10.0.1.8")
pktgen.range.dst_ip(port, "max", "10.0.1.8")

-- Set source TCP port (20000-20255, increment by 1)
pktgen.range.src_port(port, "start", 20000)
pktgen.range.src_port(port, "inc", 1)
pktgen.range.src_port(port, "min", 20000)
pktgen.range.src_port(port, "max", 20255)

-- Set destination TCP port (fixed at 20000)
pktgen.range.dst_port(port, "start", 20000)
pktgen.range.dst_port(port, "inc", 0)
pktgen.range.dst_port(port, "min", 20000)
pktgen.range.dst_port(port, "max", 20000)

-- Set TTL
pktgen.range.ttl(port, "start", 64)
pktgen.range.ttl(port, "inc", 0)
pktgen.range.ttl(port, "min", 64)
pktgen.range.ttl(port, "max", 64)

-- Enable range mode
pktgen.set_range(port, "on")

pktgen.delay(100)

-- Start transmission
print("Starting packet transmission for " .. sleeptime .. " seconds...")
pktgen.start(port)

-- Record initial statistics
pktgen.delay(1000)  -- Wait 1 second for transmission to stabilize
local initial_stats = pktgen.portStats("all", "port")[port]
local initial_tx_pkts = initial_stats.opackets

-- Wait for test duration
pktgen.delay(sleeptime * 1000)

-- Stop transmission and get final statistics
pktgen.stop(port)
local final_stats = pktgen.portStats("all", "port")[port]
local final_tx_pkts = final_stats.opackets

-- Calculate average TX rate in Mpps
local total_packets = final_tx_pkts - initial_tx_pkts
local avg_tx_rate_mpps = total_packets / (sleeptime * 1000000)

-- Display results
print("\n=== TX Rate Measurement Results ===")
print("Test Duration: " .. sleeptime .. " seconds")
print("Packet Size: 64 bytes")
print("Protocol: TCP")
print("Total Packets Transmitted: " .. total_packets)
print("Average TX Rate: " .. string.format("%.3f", avg_tx_rate_mpps) .. " Mpps")
print("=====================================\n")