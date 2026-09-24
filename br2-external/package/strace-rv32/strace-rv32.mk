################################################################################
#
# strace-rv32
#
################################################################################

# The same release as package/strace, with a port to RISC-V 32-bit.
STRACE_RV32_VERSION = 7.1
STRACE_RV32_SOURCE = strace-$(STRACE_RV32_VERSION).tar.xz
STRACE_RV32_SITE = https://github.com/strace/strace/releases/download/v$(STRACE_RV32_VERSION)
STRACE_RV32_LICENSE = LGPL-2.1+
STRACE_RV32_LICENSE_FILES = COPYING LGPL-2.1-or-later
# The patch changes configure.ac and src/Makefile.am.
STRACE_RV32_AUTORECONF = YES
STRACE_RV32_CONF_OPTS = --enable-mpers=no --without-libunwind --without-libiberty

define STRACE_RV32_REMOVE_STRACE_GRAPH
	rm -f $(TARGET_DIR)/usr/bin/strace-graph
endef
STRACE_RV32_POST_INSTALL_TARGET_HOOKS += STRACE_RV32_REMOVE_STRACE_GRAPH

$(eval $(autotools-package))
