# This Makefile should be run only by build.sh

CCFLAGS = -Xclang -disable-O0-optnone -c

all: main.o

main.o: main.c 
	${CC} ${CCFLAGS} main.c -o main.o

clean:
	rm -f *~ .*.bc *.bc *.ll *.o .*.o *.manifest main main_slash
	rm -rf slash
