# Makefile
TARGET	= user_test
CC	= gcc
CFLAGS +=-I/usr/local/gdev/include -L/usr/local/gdev/lib64/ -lucuda -lgdev -L../../include
LIBS = -lucuda -lgdev /usr/lib/resch/libresch.a

NV_ARCH = sm_20 
NVCC = nvcc -arch $(NV_ARCH) -cubin
NV_SRC = loop

all:
	$(NVCC) -o $(NV_SRC)_gpu.cubin $(NV_SRC)_gpu.cu 
	gcc -o $(TARGET) $(CFLAGS) rt_task_gpu.c $(NV_SRC).c  $(LIBS)
clean:
	rm -f $(TARGET)  ./*~ core* $(NV_SRC)_gpu.cubin
