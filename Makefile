CC = gcc
TARGET = oshfs
VCD_FILE = testbench.vcd

run: build
	mkdir mountpoint
	./$(TARGET) mountpoint

build:
	$(CC) -D_FILE_OFFSET_BITS=64 -o $(TARGET) $(TARGET).c -lfuse

clean:
	rm -rf ./$(TARGET)
	rmdir mountpoint