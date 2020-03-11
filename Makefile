all: mylib.so server

mylib.o: mylib.c
	gcc -Wall -fPIC -DPIC -c mylib.c -I ../include
 
server.o: server.c
	gcc -Wall -fPIC -DPIC -c server.c -I ../include


mylib.so: mylib.o
	ld -shared -o mylib.so mylib.o -ldl -L ../lib
 
server: server.o
	gcc -o server server.o -ldl -L ../lib -ldirtree


clean:
	rm -f *.o *.so

