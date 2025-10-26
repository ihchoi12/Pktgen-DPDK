-- Simple test script to check if lua execution works
package.path = package.path .. ";./?.lua;?.lua;test/?.lua;app/?.lua;"

require "Pktgen"

local port = 0

print("Simple test script loaded successfully")
print("Stopping and clearing port " .. port)

pktgen.stop(port)
pktgen.clear(port)
pktgen.clr()
pktgen.delay(100)

print("Test completed successfully")
