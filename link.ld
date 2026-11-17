ENTRY( _ZN4SMPS12UpdateDriver)
OUTPUT_FORMAT(binary)

SECTIONS
{
	.text : {
		*(.text.start)
		*(.text)
		*(.text.*)
		*(.rodata)
		*(.rodata.*)
	}

	# Expect driver data to be after the binary blob (but do not actually provide it).
	.driver_data (NOLOAD) : {
		*(.driver_data)
	}

	# All other sections are discarded by passing '--orphan-handling=discard' to LD.
}
