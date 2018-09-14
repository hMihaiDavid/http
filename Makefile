all:
	cc httpd.c -o httpd -std=c99 -ggdb -DDEBUG
