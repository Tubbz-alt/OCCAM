
all: run

cfg: cfg.c
	clang cfg.c -o cfg

run: cfg
	./cfg -C config.cfg

clean:
	rm -f cfg *~

