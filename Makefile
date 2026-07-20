.PHONY: all
all:
	gcc -Wno-int-conversion -o test main.c buddy.c
