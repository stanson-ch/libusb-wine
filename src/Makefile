all:

PROJ = libusb0

CFLAGS = -g -O2 \
  -fPIC \
  -Wall \
  -pipe \
  -fno-strict-aliasing \
  -Wdeclaration-after-statement \
  -Wempty-body \
  -Wignored-qualifiers \
  -Wstrict-prototypes \
  -Wtype-limits \
  -Wunused-but-set-parameter \
  -Wvla \
  -Wwrite-strings \
  -Wpointer-arith \
  -Wlogical-op \
  -gdwarf-2 \
  -gstrict-dwarf \
  -fno-omit-frame-pointer

LDFLAGS =

OBJS = linux.o error.o usb.o descriptors.o usb-wine.o

WINE_INCLUDE = /usr/include/wine/windows

CC = gcc
WINEGCC = winegcc
WINEBUILD = winebuild

dlldir = /usr/lib/wine
fakedlldir = $(dlldir)/fakedlls

$(OBJS):%.o: %.c
	$(CC) -c -o $@ $^ -I. -I$(WINE_INCLUDE) -D__WINESRC__ -D_REENTRANT $(CFLAGS)

$(PROJ).dll.so $(PROJ).dll.fake: $(PROJ).spec $(OBJS)
	$(WINEGCC) -o $@ -B$(WINEBUILD) -fasynchronous-unwind-tables -shared $(PROJ).spec \
  $(OBJS) $(LDFLAGS)

$(PROJ).def: $(PROJ).spec
	$(WINEBUILD) -w --def -o $@ --export $(PROJ).spec

$(PROJ).cross.a: $(PROJ).spec
	$(WINEBUILD) -b i386-mingw32 -w --implib -o $@ --export $(PROJ).spec

all: $(PROJ).dll.so $(PROJ).dll.fake

install install-lib:: $(PROJ).dll.so $(PROJ).dll.fake
	cp $(PROJ).dll.so $(DESTDIR)$(dlldir)/$(PROJ).dll.so
	cp $(PROJ).dll.fake $(DESTDIR)$(fakedlldir)/$(PROJ).dll

install install-dev:: $(PROJ).def
	cp $(PROJ).def $(DESTDIR)$(dlldir)/$(PROJ).def

uninstall::
	rm -f $(DESTDIR)$(dlldir)/$(PROJ).dll.so $(DESTDIR)$(fakedlldir)/$(PROJ).dll \
  $(DESTDIR)$(dlldir)/$(PROJ).def

clean::
	rm -f *.def *.a *.o *.so *.fake

.PHONY: all install install-lib install-dev uninstall clean
