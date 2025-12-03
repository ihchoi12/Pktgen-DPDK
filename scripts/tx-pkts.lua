-- RX/TX Rate Measurement Script (based on measure-tx-rate.lua)

-- Since we run from Pktgen-DPDK directory, just add current directory to path
package.path = package.path .. ";./?.lua;?.lua;test/?.lua;app/?.lua;"

require "Pktgen"

local port = 0
local sleeptime = tonumber(os.getenv("PKTGEN_DURATION")) or 5
local packet_size = tonumber(os.getenv("PKTGEN_PACKET_SIZE")) or 64

pktgen.stop(port)
pktgen.clear(port)
pktgen.clr()
pktgen.delay(100)

-- Configuration (same as measure-tx-rate.lua)
pktgen.set(port, "size", packet_size)
pktgen.set(port, "rate", 100)  -- 100% rate to utilize multiple cores
pktgen.set(port, "count", 0)   -- Continuous transmission (0 = infinite)

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

-- Set packet size for range mode
pktgen.range.pkt_size(port, "start", packet_size)
pktgen.range.pkt_size(port, "inc", 0)
pktgen.range.pkt_size(port, "min", packet_size)
pktgen.range.pkt_size(port, "max", packet_size)

-- Enable range mode (same as measure-tx-rate.lua)
pktgen.set_range(port, "on")

pktgen.delay(100)

-- Start transmission
print("Starting packet transmission for " .. sleeptime .. " seconds")
pktgen.start(port)

pktgen.delay(sleeptime * 1000) -- sleep time in milliseconds

-- Stop transmission BEFORE reading statistics
print("Stopping packet transmission...")
pktgen.stop(port)

-- Wait a bit for any remaining packets to be transmitted by hardware
pktgen.delay(100)

-- Print packet statistics summary after stopping
print("\nPrinting packet statistics summary...")
pktgen.print_stats()

os.exit(0)
