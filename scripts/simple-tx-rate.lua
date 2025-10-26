-- Simplified TX Rate Measurement Script
package.path = package.path .. ";./?.lua;?.lua;test/?.lua;app/?.lua;"

require "Pktgen"

local port = 0
local sleeptime = 3  -- Reduced to 3 seconds for testing

print("Starting simplified TX rate measurement...")

pktgen.stop(port)
pktgen.clear(port)
pktgen.clr()
pktgen.delay(100)

-- Basic configuration
pktgen.set(port, "size", 64)
pktgen.set(port, "rate", 100)  -- Maximum rate
pktgen.set(port, "count", 0)   -- Continuous transmission

-- Simple MAC and IP addresses (no range)
pktgen.set_mac(port, "src", "08:c0:eb:b6:cd:5d")
pktgen.set_mac(port, "dst", "08:c0:eb:b6:e8:05")
pktgen.set_ipaddr(port, "src", "10.0.1.7")
pktgen.set_ipaddr(port, "dst", "10.0.1.8/24")

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
print("\n=== Simplified TX Rate Results ===")
print("Test Duration: " .. sleeptime .. " seconds")
print("Total Packets Transmitted: " .. total_packets)
print("Average TX Rate: " .. string.format("%.3f", avg_tx_rate_mpps) .. " Mpps")
print("====================================\n")
