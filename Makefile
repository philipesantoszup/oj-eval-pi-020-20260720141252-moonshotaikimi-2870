.PHONY: all clean

all: code

code: main.c buddy.c
	gcc -Wno-int-conversion -o code main.c buddy.c

clean:
	rm -f code
