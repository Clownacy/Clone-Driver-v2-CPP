#!/usr/bin/env lua

local common = require "build_tools.lua.common"

local message, abort = common.build_rom("DAC-Driver", "DAC-Driver", "", "-p=FF -z=0,saxman-optimised,Size_of_DAC_driver_guess,after", true, "https://github.com/Clownacy/Sonic-2-Clone-Driver-v2")

if message then
	exit_code = false
end

if abort then
	os.exit(exit_code, true)
end

-- Convert the generated binary into something that C++ can actually use.
local binary_file = io.open("DAC-Driver.bin", "rb")
local header_file = io.open("DAC-Driver.bin.h", "w")

for byte in function() return binary_file:read(1) end do
	header_file:write(string.unpack("B", byte) .. ", ")
end

binary_file:close()
header_file:close()

os.exit(exit_code, false)
