all:

PROJ = libusb0

CFLAGS = -g -O2 -m32 \
  -fPIC \
  -Wall \
  -pipe \
  -fno-strict-aliasing \
  -Wempty-body \
  -Wignored-qualifiers \
  -Wstrict-prototypes \
  -Wtype-limits \
  -Wunused-but-set-parameter \
  -Wvla \
  -Wwrite-strings \
  -Wpointer-arith \
  -Wlogical-op \
  -Wno-format-truncation \
  -gdwarf-2 \
  -gstrict-dwarf \
  -fno-omit-frame-pointer \
  -Werror=missing-prototypes \
  -Werror=implicit-function-declaration

LDFLAGS = -m32 -lpthread

OBJS = linux.o error.o usb.o descriptors.o usb-wine.o

WINE_INCLUDE = /usr/include/wine/windows

CC = gcc
WINEGCC = winegcc
WINEBUILD = winebuild

dlldir = /usr/lib/wine

$(OBJS):%.o: %.c
	$(CC) -c -o $@ $^ -I. -I$(WINE_INCLUDE) -D__WINESRC__ -D_REENTRANT $(CFLAGS)

$(PROJ).dll.so $(PROJ).dll.fake: $(PROJ).spec $(OBJS)
	$(WINEGCC) -o $@ -B$(WINEBUILD) -fasynchronous-unwind-tables -shared $(PROJ).spec \
  $(OBJS) $(LDFLAGS)

$(PROJ).a: $(PROJ).spec
	$(WINEBUILD) -m32 -w --implib -o $@ --export $(PROJ).spec

$(PROJ).def: $(PROJ).spec
	$(WINEBUILD) -w --def -o $@ --export $(PROJ).spec

$(PROJ).cross.a: $(PROJ).spec
	$(WINEBUILD) -b i386-mingw32 -w --implib -o $@ --export $(PROJ).spec

all: $(PROJ).dll.so $(PROJ).dll.fake

# Wine 6.8 and later keep builtin DLLs in a folder per architecture: the
# library in i386-unix, its placeholder in i386-windows. Wine copies
# the placeholder into a prefix's system32 when it creates or updates the
# prefix, and programs find the library only through that placeholder.
install install-lib:: $(PROJ).dll.so $(PROJ).dll.fake
	mkdir -p $(DESTDIR)$(dlldir)/i386-unix $(DESTDIR)$(dlldir)/i386-windows
	cp $(PROJ).dll.so $(DESTDIR)$(dlldir)/i386-unix/$(PROJ).dll.so
	cp $(PROJ).dll.fake $(DESTDIR)$(dlldir)/i386-windows/$(PROJ).dll

# the import library winegcc links winelib programs such as lsusb-wine with
install install-dev:: $(PROJ).a
	mkdir -p $(DESTDIR)$(dlldir)/i386-unix
	cp $(PROJ).a $(DESTDIR)$(dlldir)/i386-unix/$(PROJ).a

uninstall::
	rm -f $(DESTDIR)$(dlldir)/i386-unix/$(PROJ).dll.so \
  $(DESTDIR)$(dlldir)/i386-windows/$(PROJ).dll \
  $(DESTDIR)$(dlldir)/i386-unix/$(PROJ).a

clean::
	rm -f *.def *.a *.o *.so *.fake *.exe

.PHONY: all install install-lib install-dev uninstall clean
