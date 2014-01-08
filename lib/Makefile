TARGET	= libresch
INCDIR	= ../include/
CFLAGS	= -W -Wall -g -O2 -s -pipe

.PHONY: all
all: 
	gcc $(CFLAGS) -I$(INCDIR) -o $(TARGET).o -c $(TARGET).c
	ar -rcs $(TARGET).a $(TARGET).o

.PHONY: clean
clean:
	rm -f $(TARGET).o $(TARGET).a *~

.PHONY: install
install:
	@./script/install

.PHONY: uninstall
uninstall:
	@./script/uninstall
