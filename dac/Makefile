	include /opt/clownmdsdk/generic.mk

all: DAC-Driver.sax

%.p: %.asm
	asl -c -L -q $< -o $@

%.bin: %.p
	p2bin -q $< $@

%.sax: %.bin
	clownlzss -s $< $@
