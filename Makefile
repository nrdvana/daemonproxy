all: init-frame

OBJS := fd.o service.o signal.o controller.o Contained_RBTree.o init-frame.o
CFLAGS := -MMD -MP -D UNIT_TESTING -O0 -g3 -Wall

-include $(OBJS:.o=.d)

init-frame: $(OBJS)
	gcc -o $@ -O1 -g3 $^

controller_data.autogen.c: controller.c
	perl generate_controller_data.pl < $< > $@

clean:
	rm controller_data.autogen.c
	rm *.o
	rm *.d
