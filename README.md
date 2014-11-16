Collection of configs, scripts and programs were involved in writing
["Restricting program memory"][restrict-memory].

* big_alloc.c - simple program that allocates ~100 MiB of memory
* lxc-my.conf - config for LXC
* linker - Linker related stuff
* Makefile - you guess.
* memrestrict.c - intercept malloc/free calls to acount heap usage
* vg-wrap.sh - LD_PRELOAD wrapper for valgrind


[restrict-memory]: http://avd.reduct.ru/programming/restrict-memory.html
