
_PAL=1
_NTSC=0
_VESA=0
_MOUSE=0
_IOPRESET=1
EE_OBJS =\
	chase.o \
	cl_demo.o \
	cl_input.o \
	cl_main.o \
	cl_parse.o \
	cl_tent.o \
	cmd.o \
	common.o \
	console.o \
	crc.o \
	cvar.o \
	draw.o \
	d_edge.o \
	d_fill.o \
	d_init.o \
	d_modech.o \
	d_part.o \
	d_polyse.o \
	d_scan.o \
	d_sky.o \
	d_sprite.o \
	d_surf.o \
	d_vars.o \
	d_zpoint.o \
	host.o \
	host_cmd.o \
	keys.o \
	menu.o \
	mathlib.o \
	model.o \
	nonintel.o \
	pr_cmds.o \
	pr_edict.o \
	pr_exec.o \
	r_aclip.o \
	r_alias.o \
	r_bsp.o \
	r_light.o \
	r_draw.o \
	r_efrag.o \
	r_edge.o \
	r_misc.o \
	r_main.o \
	r_sky.o \
	r_sprite.o \
	r_surf.o \
	r_part.o \
	r_vars.o \
	screen.o \
	sbar.o \
	sv_main.o \
	sv_phys.o \
	sv_move.o \
	sv_user.o \
	zone.o	\
	view.o	\
	wad.o \
	world.o \
	cd_ps2.o \
	net_vcr.o \
	net_main.o \
	net_loop.o \
	net_none.o \
	sys_ps2.o \
	snd_dma.o \
	snd_mix.o \
	snd_mem.o \
	snd_ps2.o \
	in_ps2.o \
	vid_ps2.o \
	ps2.o \
	pad.o
EE_BIN = bin/quake.elf

EE_LIBS =   -lgskit -ldmakit -lps2_drivers -laudsrv -lvorbisfile -lvorbis -logg -lmouse -lkbd -lm -lc -lkernel -lpad
EE_INCS :=  -I$(PS2SDK)/sbv/include -I$(PS2SDK)/ports/include -I$(PS2SDK)/ports/include/SDL -I$(GSKIT)/include -I$(PS2SDK)/ee/include -I$(PS2DEV)/ee/ee/include
EE_CFLAGS = -std=gnu11 -fcommon -fno-aggressive-loop-optimizations -DNEWLIB_PORT_AWARE -g -Dstricmp=strcasecmp -funroll-loops -fomit-frame-pointer -fexpensive-optimizations
EE_LDFLAGS := -L$(PS2SDK)/sbv/lib -L$(PS2SDK)/ports/lib -L$(GSKIT)/lib

ifeq ($(_PAL), 1)
	 EE_CFLAGS += -D_PAL
endif
ifeq ($(_NTSC), 1)
	 EE_CFLAGS += -D_NTSC
endif
ifeq ($(_VESA), 1)
	 EE_CFLAGS += -D_VESA
endif
ifeq ($(_MOUSE), 1)
	 EE_CFLAGS += -D_MOUSE
endif
ifeq ($(_IOPRESET), 1)
	 EE_CFLAGS += -D_IOPRESET
endif

all: $(EE_BIN)

# Optional: compress the ELF with ps2-packer for faster loading.
# Needs a working host ps2-packer binary (the bundled one is i386-only and
# will not run on modern/Alpine hosts); run separately, not part of `all`.
pack: $(EE_BIN)
	 ps2_packer/ps2_packer bin/quake.elf bin/packed_quake.elf
clean:
	rm -f $(EE_BIN) $(EE_OBJS)

run:
	ps2client -h 10.0.0.10 execee host:$(EE_BIN)

include $(PS2SDK)/samples/Makefile.pref
include Makefile.eeglobal