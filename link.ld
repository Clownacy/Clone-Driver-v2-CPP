ENTRY( _ZN4SMPS12UpdateDriver)
OUTPUT_FORMAT(binary)

SECTIONS
{
	.text : {
		*(.text.start)
		*(.text)
		*(.text.*)
	}
}
