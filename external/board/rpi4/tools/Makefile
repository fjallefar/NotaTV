CFLAGS ?= -O2
BINS = tvmon tryboot

all: $(BINS)

%: %.c
	$(CC) $(CFLAGS) -Wall -Wextra $(LDFLAGS) -o $@ $<

clean:
	rm -f $(BINS)
