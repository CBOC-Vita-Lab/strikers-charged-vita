# clang as a compiler for the VitaSDK ABI (shared by Makefile.vita and
# Makefile.vitaprobe; link with arm-vita-eabi-g++).
#
# clang compiles a translated shard in ~14 s where VitaSDK's GCC 15 needs
# ~100 s. The flags below are what make its output link-compatible with code
# built by VitaSDK's GCC (newlib, libstdc++, libgcc, vitaGL):
#   -fsigned-char      VitaSDK's GCC has signed plain char (ARM default: unsigned)
#   -fshort-enums      VitaSDK's GCC uses AAPCS variable-size enums
#   -femulated-tls     VitaSDK has no native TLS in its C++ runtime: GCC emits
#                      __emutls_get_address() calls and libstdc++/libgcc expect them
#   -munaligned-access clang defaults to strict alignment on a bare-metal
#                      triple; the Vita (like GCC's default) allows unaligned
#                      LDR/STR, which the guest memory accessors rely on
#   __WINT_TYPE__ / __INT_FAST*_TYPE__: the typedefs newlib builds from these
#                      differ between the two compilers and enter C++ name
#                      mangling, so they are set to GCC's choices
#   __USES_INITFINI__  what VitaSDK's GCC predefines (newlib's crt0 runs
#                      .init_array through __libc_init_array)
VITASDK ?= /opt/vitasdk
VBIN := $(VITASDK)/bin
VSYS := $(VITASDK)/arm-vita-eabi
GXXV := 15.2.0

TARGET := --target=armv7a-none-eabihf -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -mthumb \
          -fsigned-char -fshort-enums -femulated-tls -fno-pic -munaligned-access \
          -D__vita__ -D__USES_INITFINI__ -U_GNU_SOURCE \
          -Wno-builtin-macro-redefined \
          -U__WINT_TYPE__ '-D__WINT_TYPE__=unsigned int' \
          -U__INT_FAST8_TYPE__ -D__INT_FAST8_TYPE__=int -U__INT_FAST16_TYPE__ -D__INT_FAST16_TYPE__=int \
          -U__UINT_FAST8_TYPE__ '-D__UINT_FAST8_TYPE__=unsigned int' \
          -U__UINT_FAST16_TYPE__ '-D__UINT_FAST16_TYPE__=unsigned int' \
          -nostdlibinc
CSYS   := -isystem $(VSYS)/include
CXXSYS := -isystem $(VSYS)/include/c++/$(GXXV) -isystem $(VSYS)/include/c++/$(GXXV)/arm-vita-eabi \
          -isystem $(VSYS)/include/c++/$(GXXV)/backward $(CSYS)
