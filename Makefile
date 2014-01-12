init-frame: fd.o service.o signal.o controller.o Contained_RBTree.o init-frame.o
	gcc -o $@ -O0 -g3 $^

controller.o: controller_data.autogen.c

controller_data.autogen.c: controller.c
	perl generate_controller_data.pl < $< > $@
