#!/usr/bin/env lua

os.execute("make")

-- Convert the generated binary into something that C++ can actually use.
local binary_file = io.open("DAC-Driver.sax", "rb")
local header_file = io.open("DAC-Driver.sax.h", "w")

for byte in function() return binary_file:read(1) end do
	header_file:write(string.unpack("B", byte) .. ", ")
end

binary_file:close()
header_file:close()
