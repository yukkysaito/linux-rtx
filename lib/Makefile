TARGET	= libresch
INCDIR	= ../include/
CFLAGS	= -W -Wall -g -O2 -s -pipe


.PHONY: all
all: $(TARGET).o
	ar -rcs $(TARGET).a $(TARGET).o
	#gcc -fPIC -I$(INCDIR) -c libresch.c -o libresch.o
	gcc -shared -Wl,-soname,$(TARGET).so.1 -o ./$(TARGET).so.1.0.0 $(TARGET).o

%.o:%.c
	gcc -fPIC -c $^ -o $@ $(CFLAGS) -I$(INCDIR)


.PHONY: clean
clean:
	rm -f $(TARGET).o $(TARGET).a $(TARGET).so* *~

.PHONY: install
install:
	@./script/install

.PHONY: uninstall
uninstall:
	@./script/uninstall
