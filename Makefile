default: big_alloc

big_alloc: big_alloc.c
	gcc $^ -o $@

container: big_alloc
	sudo lxc-execute -n restricted -f ./lxc-my.conf /bin/sh

linker:
	gcc big_alloc.c -o big_alloc -Wl,-verbose > default.lst

big_alloc_linker: big_alloc.c
	gcc -g big_alloc.c -o big_alloc_linker -Wl,-T hack.lst

clean:
	rm -f *.o big_alloc big_alloc_linker
