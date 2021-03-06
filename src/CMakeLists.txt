
if(WIN32 AND NOT CYGWIN)
add_custom_command(
	OUTPUT ${PearPC_BINARY_DIR}/src/build_number.h
	COMMAND "${PearPC_SOURCE_DIR}/scripts/gen_buildnum_h_helper.cmd" "${PearPC_SOURCE_DIR}"
)
else(WIN32 AND NOT CYGWIN)
add_custom_command(
	OUTPUT ${PearPC_BINARY_DIR}/src/build_number.h
	COMMAND "${PearPC_SOURCE_DIR}/scripts/gen_buildnum_h.sh" "${PearPC_BINARY_DIR}/src/build_number.h"
)
endif(WIN32 AND NOT CYGWIN)

IF(WIN32)
	FIND_PACKAGE(Cygwin REQUIRED)
	SET(CMAKE_FIND_ROOT_PATH ${CMAKE_FIND_ROOT_PATH} ${CYGWIN_INSTALL_PATH})
ENDIF(WIN32)

IF(NOT MSVC)
	set_property(SOURCE cpu/cpu_jitc_x86/jitc_mmu.S PROPERTY LANGUAGE C)
	set_property(SOURCE cpu/cpu_jitc_x86/jitc_tools.S PROPERTY LANGUAGE C)
	set_property(SOURCE system/arch/x86/vaccel.S PROPERTY LANGUAGE C)
	add_library(vaccel system/arch/x86/vaccel.S)
ENDIF(NOT MSVC)
IF(MSVC)
	set_property(SOURCE cpu/cpu_jitc_x86/jitc_mmu.obj PROPERTY LANGUAGE C)
	set_property(SOURCE cpu/cpu_jitc_x86/jitc_tools.obj PROPERTY LANGUAGE C)
	set_property(SOURCE system/arch/x86/vaccel.obj PROPERTY LANGUAGE C)
	add_library(vaccel system/arch/x86/vaccel.obj)
ENDIF(MSVC)

SET(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${CMAKE_CURRENT_SOURCE_DIR})

FIND_PACKAGE(BISON REQUIRED)
FIND_PACKAGE(FLEX REQUIRED)

FLEX_TARGET(DebuggerLexer debug/lex.l debug/lex.c)
BISON_TARGET(DebuggerParser debug/debugparse.y debug/debugparse.c)

add_custom_target(PearPCBuildNumber DEPENDS ${PearPC_BINARY_DIR}/src/build_number.h)

# JIT CPU
add_library(cpu-jitc cpu/cpu_jitc_x86/jitc.cc cpu/cpu_jitc_x86/jitc_debug.cc cpu/cpu_jitc_x86/jitc_mmu.S cpu/cpu_jitc_x86/jitc_mmu.obj cpu/cpu_jitc_x86/jitc_tools.S cpu/cpu_jitc_x86/jitc_tools.obj cpu/cpu_jitc_x86/ppc_alu.cc cpu/cpu_jitc_x86/ppc_cpu.cc cpu/cpu_jitc_x86/ppc_dec.cc cpu/cpu_jitc_x86/ppc_esc.cc cpu/cpu_jitc_x86/ppc_exc.cc cpu/cpu_jitc_x86/ppc_fpu.cc cpu/cpu_jitc_x86/ppc_mmu.cc cpu/cpu_jitc_x86/ppc_opc.cc cpu/cpu_jitc_x86/ppc_vec.cc cpu/cpu_jitc_x86/x86asm.cc)

# interpreted CPU
add_library(cpu-generic cpu/cpu_generic/ppc_alu.cc cpu/cpu_generic/ppc_cpu.cc cpu/cpu_generic/ppc_dec.cc cpu/cpu_generic/ppc_exc.cc cpu/cpu_generic/ppc_fpu.cc cpu/cpu_generic/ppc_mmu.cc cpu/cpu_generic/ppc_opc.cc cpu/cpu_generic/ppc_vec.cc)

add_library(ppc-common configparser.cc debug/asm.cc debug/debugger.cc debug/debugparse.c debug/lex.c debug/parsehelper.c debug/ppcdis.cc debug/ppcopc.cc debug/stdfuncs.cc debug/x86dis.cc debug/x86opc.cc io/3c90x/3c90x.cc io/cuda/cuda.cc io/graphic/gcard.cc io/ide/ata.cc io/ide/cd.cc io/ide/ide.cc io/ide/idedevice.cc io/ide/sparsedisk.cc io/io.cc io/macio/macio.cc io/nvram/nvram.cc io/pci/pci.cc io/pci/pcihwtd.cc io/pic/pic.cc io/prom/fcode.cc io/prom/forth.cc io/prom/forthtable.cc io/prom/fs/fs.cc io/prom/fs/hfs/block.c io/prom/fs/hfs/btree.c io/prom/fs/hfs/data.c io/prom/fs/hfs/file.c io/prom/fs/hfs/hfs.c io/prom/fs/hfs/low.c io/prom/fs/hfs/medium.c io/prom/fs/hfs/node.c io/prom/fs/hfs/os.cc io/prom/fs/hfs/record.c io/prom/fs/hfs/version.c io/prom/fs/hfs/volume.c io/prom/fs/hfs.cc io/prom/fs/hfsplus/blockiter.c io/prom/fs/hfsplus/btree.c io/prom/fs/hfsplus/hfstime.c io/prom/fs/hfsplus/libhfsp.c io/prom/fs/hfsplus/os.cc io/prom/fs/hfsplus/partitions.c io/prom/fs/hfsplus/record.c io/prom/fs/hfsplus/unicode.c io/prom/fs/hfsplus/volume.c io/prom/fs/hfsplus.cc io/prom/fs/part.cc io/prom/prom.cc io/prom/promboot.cc io/prom/promdt.cc io/prom/prommem.cc io/prom/promosi.cc io/rtl8139/rtl8139.cc io/serial/serial.cc io/usb/usb.cc ppc_button_changecd.c ppc_font.c ppc_img.c system/arch/generic/sysvaccel.cc system/arch/x86/sysvaccel.cc system/device.cc system/display.cc system/file.cc system/font.cc system/gif.cc system/keyboard.cc system/mouse.cc system/osapi/posix/syscdrom.cc system/osapi/posix/sysclipboard.cc system/osapi/posix/sysethtun.cc system/osapi/posix/sysfile.cc system/osapi/posix/sysinit.cc system/osapi/posix/systhread.cc system/osapi/posix/systimer.cc system/osapi/win32/syscdrom.cc system/osapi/win32/sysclipboard.cc system/osapi/win32/sysethtun.cc system/osapi/win32/sysfile.cc system/osapi/win32/sysinit.cc system/osapi/win32/systhread.cc system/osapi/win32/systimer.cc system/sys.cc system/sysethpcap.cc system/sysexcept.cc system/ui/win32/gui.cc system/ui/win32/sysdisplay.cc system/ui/win32/syskeyboard.cc system/ui/win32/sysmouse.cc system/ui/win32/syswin.cc system/ui/x11/gui.cc system/ui/x11/sysdisplay.cc system/ui/x11/syskeyboard.cc system/ui/x11/sysmouse.cc system/ui/x11/sysx11.cc system/vt100.cc tools/atom.cc tools/crc32.cc tools/data.cc tools/debug.cc tools/endianess.cc tools/except.cc tools/snprintf.cc tools/str.cc tools/stream.cc tools/strtools.cc tools/thread.cc ${BF_SOURCES})

link_directories( ${LINK_DIRECTORIES} /usr/X11R6/lib )

add_executable(ppc-jitc main.cc)
add_executable(ppc-generic main.cc)

target_link_libraries (ppc-jitc cpu-jitc CrissCross ppc-common vaccel)
target_link_libraries (ppc-generic cpu-generic CrissCross ppc-common vaccel)

add_dependencies(ppc-common PearPCBuildNumber)
add_dependencies(cpu-jitc PearPCBuildNumber)
add_dependencies(cpu-generic PearPCBuildNumber)

IF(NOT WIN32)
	target_link_libraries(ppc-jitc pthread X11)
	target_link_libraries(ppc-generic pthread X11)
	IF (NOT APPLE)
		target_link_libraries(ppc-jitc rt dl)
		target_link_libraries(ppc-generic rt dl)
	ENDIF(NOT APPLE)
ENDIF(NOT WIN32)

IF(MSVC)
	target_link_libraries(ppc-jitc winmm)
	target_link_libraries(ppc-generic winmm)
ENDIF(MSVC)
