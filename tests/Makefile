CC=gcc
CFLAGS=-Wall -Wno-trigraphs -DBUILD_LEGACY_SUPPORT

all: tests
default: tests

tests: test_interpreter test_patchlist test_gxp test_menu

test_interpreter: test_interpreter.c interpreter.obj
	$(CC) $(CFLAGS) test_interpreter.c $(interpreter_obj) -o test_interpreter -lm

test_patchlist: test_patchlist.c interpreter.obj patch_gxp_core.o
	$(CC) $(CFLAGS) test_patchlist.c $(interpreter_obj) patch_gxp_core.o -o test_patchlist -lm

# test_gxp compiles the core itself, so TEST_SANITIZE=1 really instruments it
test_gxp: test_gxp.c ../src/patch_gxp_core.c ../src/patch_gxp_core.h interpreter.obj
	$(CC) $(CFLAGS) $(SANITIZE) test_gxp.c ../src/patch_gxp_core.c $(interpreter_obj) -o test_gxp -lm

# The in-game menu, config saving and the OSD against stand-ins for the Vita (menu_stubs.c)
menu_src := ../src/main.c ../src/menu.c ../src/config.c ../src/io.c ../src/osd.c ../src/patch_hook.c
test_menu: test_menu.c menu_stubs.c menu_stubs.h menu_inc/vitasdk.h menu_inc/taihen.h $(menu_src) $(wildcard ../src/*.h)
	$(CC) $(CFLAGS) -std=gnu99 $(SANITIZE) -Imenu_inc test_menu.c menu_stubs.c \
		../src/menu.c ../src/config.c ../src/io.c ../src/osd.c ../src/patch_hook.c -o test_menu

patch_gxp_core.o: ../src/patch_gxp_core.c ../src/patch_gxp_core.h $(interpreter_incl)
	$(CC) $(CFLAGS) -c ../src/patch_gxp_core.c -o patch_gxp_core.o

# Unit tests plus the shader-line fixtures (TEST_SANITIZE=1 builds test_gxp with ASan/UBSan)
ifeq ($(TEST_SANITIZE),1)
SANITIZE=-fsanitize=address,undefined -g
endif

check: test_interpreter test_patchlist test_gxp test_menu
	./test_interpreter
	./test_gxp
	./test_menu menu_fs
	./test_patchlist fixtures/gxp_lbp.txt --ib 720x408 --expect-stock
	./test_patchlist fixtures/gxp_lbp.txt --ib 960x544 | diff - fixtures/gxp_lbp_960x544.expected
	./test_patchlist fixtures/gxp_lbp.txt --ib 1280x720 | diff - fixtures/gxp_lbp_960x544.expected
	IB_STEP=16 ./test_patchlist_configs.sh fixtures/gxp_lbp.txt

interpreter_incl :=\
	../src/interpreter/interpreter.h\
	../src/interpreter/parser.h\
	../src/interpreter/legacy.h\
	../src/interpreter/op.h

interpreter_src :=\
	../src/interpreter/interpreter.c\
	../src/interpreter/parser.c\
	../src/interpreter/legacy.c\
	../src/interpreter/op_datatype.c\
	../src/interpreter/op_encode.c\
	../src/interpreter/op_math.c\
	../src/interpreter/op_vitagrafix.c

interpreter_obj :=\
	interpreter.o\
	parser.o\
	legacy.o\
	op_datatype.o\
	op_encode.o\
	op_math.o\
	op_vitagrafix.o

interpreter: $(interpreter_src) $(interpreter_incl) main.c
	$(CC) $(CFLAGS) $(interpreter_src) main.c -o interpreter -lm

interpreter.obj: $(interpreter_src) $(interpreter_incl)
	$(CC) $(CFLAGS) -c $(interpreter_src) -lm

clean:
	rm -f test_interpreter test_patchlist test_gxp test_menu interpreter *.o
	rm -rf menu_fs
