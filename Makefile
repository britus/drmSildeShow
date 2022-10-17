
FLAGS=`pkg-config cairo --cflags --libs libdrm`
FLAGS+=-Wall -O0 -g
FLAGS+=-D_FILE_OFFSET_BITS=64

all:
	gcc -o atomicmode dis_atomic_app.c $(FLAGS)

install: all
	@cp -v atomicmode /usr/local/bin/bootsplash
	@cp -v bootsplash.service /lib/systemd/system/
	@systemctl daemon-reload