-- RX/TX Rate Measurement Script (based on measure-tx-rate.lua)

-- Since we run from Pktgen-DPDK directory, just add current directory to path
package.path = package.path .. ";./?.lua;?.lua;test/?.lua;app/?.lua;"

require "Pktgen"

local port = 0
local sleeptime = tonumber(os.getenv("PKTGEN_DURATION")) or 5
local packet_size = tonumber(os.getenv("PKTGEN_PACKET_SIZE")) or 64

-- Get MAC/IP from environment variables
local src_mac = os.getenv("PKTGEN_SRC_MAC")
local dst_mac = os.getenv("PKTGEN_DST_MAC")
local src_ip = os.getenv("PKTGEN_SRC_IP") or "192.168.0.1"
local dst_ip = os.getenv("PKTGEN_DST_IP") or "198.18.0.1"

-- Validate required environment variables
if not src_mac or src_mac == "" then
    print("ERROR: PKTGEN_SRC_MAC environment variable not set")
    os.exit(1)
end
if not dst_mac or dst_mac == "" then
    print("ERROR: PKTGEN_DST_MAC environment variable not set")
    os.exit(1)
end

print("=== Packet Configuration ===")
print("SRC MAC: " .. src_mac)
print("DST MAC: " .. dst_mac)
print("SRC IP: " .. src_ip)
print("DST IP: " .. dst_ip)
print("============================")

pktgen.stop(port)
pktgen.clear(port)
pktgen.clr()
pktgen.delay(100)

-- Configuration
pktgen.set(port, "size", packet_size)
pktgen.set(port, "rate", 100)  -- 100% rate to utilize multiple cores
pktgen.set(port, "count", 0)   -- Continuous transmission (0 = infinite)

-- Set MAC addresses from environment variables
pktgen.set_mac(port, "src", src_mac)
pktgen.set_mac(port, "dst", dst_mac)

-- Set IP addresses from environment variables
pktgen.set_ipaddr(port, "src", src_ip)
pktgen.set_ipaddr(port, "dst", dst_ip .. "/24")

-- Set up Range configuration for TCP
pktgen.range.ip_proto("all", "tcp")

-- Set MAC addresses in range
pktgen.range.src_mac(port, "start", src_mac)
pktgen.range.dst_mac(port, "start", dst_mac)

-- Set source IP (fixed)
pktgen.range.src_ip(port, "start", src_ip)
pktgen.range.src_ip(port, "inc", "0.0.0.0")
pktgen.range.src_ip(port, "min", src_ip)
pktgen.range.src_ip(port, "max", src_ip)

-- Set destination IP (fixed, must match L3FWD LPM route 198.18.0.0/24)
pktgen.range.dst_ip(port, "start", dst_ip)
pktgen.range.dst_ip(port, "inc", "0.0.0.0")
pktgen.range.dst_ip(port, "min", dst_ip)
pktgen.range.dst_ip(port, "max", dst_ip)

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

-- Start transmission
print("Starting packet transmission for " .. sleeptime .. " seconds...")
pktgen.start(port)

-- Convert sleeptime from seconds to milliseconds for pktgen.delay()
local delay_ms = sleeptime * 1000
pktgen.delay(delay_ms)

-- Stop transmission BEFORE reading statistics
print("Stopping packet transmission...")
pktgen.stop(port)

-- Wait a bit for any remaining packets to be transmitted by hardware
pktgen.delay(100)

-- Print packet statistics summary after stopping
print("\nPrinting packet statistics summary...")
pktgen.print_stats()

os.exit(0)
