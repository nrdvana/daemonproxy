all: daemonproxy

OBJS := fd.o service.o signal.o controller.o Contained_RBTree.o daemonproxy.o
CFLAGS := -MMD -MP -D UNIT_TESTING -O0 -g3 -Wall

-include $(OBJS:.o=.d)

daemonproxy: $(OBJS)
	gcc -o $@ -O1 -g3 $^ -lrt

controller.o: controller_data.autogen.c

controller_data.autogen.c: controller.c
	perl generate_controller_data.pl < $< > $@

clean:
	rm controller_data.autogen.c
	rm *.o
	rm *.d
