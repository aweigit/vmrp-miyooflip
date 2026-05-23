CC := gcc
CFLAGS := -flto -fPIC -O3 -Wall -DNETWORK_SUPPORT -DVMRP -I./header
LDFLAGS := -lpthread -lm -flto -fPIC
OBJS := network.o fileLib.o vmrp.o utils.o rbtree.o bridge.o memory.o cJSON.o

LDFLAGS += -lSDL2 -lSDL2_mixer -L./lib -lunicorn

ifeq ($(DEBUG),1)
	CFLAGS += -DDEBUG
	OBJS += debug.o trace.o
	LDFLAGS += -lcapstone
endif

main: $(OBJS) main.o
	$(CC) $(CFLAGS) -o ./bin/$@ $^ $(LDFLAGS)

%.o:%.c
	$(CC) $(CFLAGS) -c $^

.PHONY: clean
clean:
	-rm -f *.o
