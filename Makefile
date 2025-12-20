CC = cc
CFLAGS = -w -O2

TARGET = light

all: $(TARGET)

$(TARGET): light.HolyCode.c
	$(CC) $(CFLAGS) -o $(TARGET) light.HolyCode.c

install: $(TARGET)
	sudo cp $(TARGET) /usr/bin/
	sudo chmod +x /usr/bin/$(TARGET)

clean:
	rm -f $(TARGET)
