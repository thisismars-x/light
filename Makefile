CC = cc
CFLAGS = -Wall  -O2

TARGET = light

all: $(TARGET)

$(TARGET): light.holyC.c
	$(CC) $(CFLAGS) -o $(TARGET) light.holyC.c

install: $(TARGET)
	sudo cp $(TARGET) /usr/bin/
	sudo chmod +x /usr/bin/$(TARGET)

clean:
	rm -f $(TARGET)
