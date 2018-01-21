CC := gcc
CFLAGS := -O2 -Wall

TARGET := fwtool
OBJS := fwtool.o tsv.o crc32.o

all: $(TARGET)

clean:
	rm -vf $(TARGET) $(OBJS)

depend:
	$(CC) -MM $(OBJS:.o=.c) > Makefile.dep

$(TARGET): $(OBJS)
	$(CC) -o $@ $(OBJS)

-include Makefile.dep
