OUTPUT_FORMAT(binary)

SECTIONS
{
	.text : {
		*(.exports)
	}

	/DISCARD/ : {
		*
	}
}
