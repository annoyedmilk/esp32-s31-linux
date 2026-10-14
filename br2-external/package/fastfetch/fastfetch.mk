################################################################################
#
# fastfetch
#
################################################################################

FASTFETCH_VERSION = 2.70.0
FASTFETCH_SITE = $(call github,fastfetch-cli,fastfetch,$(FASTFETCH_VERSION))
FASTFETCH_LICENSE = MIT
FASTFETCH_LICENSE_FILES = LICENSE
# No desktop, GPU, audio or image libraries on this board.  SET_TWEAK would
# add a git suffix to the version.
FASTFETCH_CONF_OPTS = \
	-DBUILD_FLASHFETCH=OFF \
	-DBUILD_TESTS=OFF \
	-DSET_TWEAK=OFF \
	-DINSTALL_LICENSE=OFF \
	-DIS_MUSL=$(if $(BR2_TOOLCHAIN_USES_MUSL),ON,OFF) \
	-DENABLE_LTO=OFF \
	-DENABLE_VULKAN=OFF \
	-DENABLE_WAYLAND=OFF \
	-DENABLE_XCB_RANDR=OFF \
	-DENABLE_XRANDR=OFF \
	-DENABLE_DRM=OFF \
	-DENABLE_VADRM=OFF \
	-DENABLE_VAX11=OFF \
	-DENABLE_VDPAU=OFF \
	-DENABLE_GIO=OFF \
	-DENABLE_DCONF=OFF \
	-DENABLE_EET=OFF \
	-DENABLE_DBUS=OFF \
	-DENABLE_SQLITE3=OFF \
	-DENABLE_RPM=OFF \
	-DENABLE_EGL=OFF \
	-DENABLE_GLX=OFF \
	-DENABLE_OPENCL=OFF \
	-DENABLE_FREETYPE=OFF \
	-DENABLE_ELF=OFF \
	-DENABLE_IMAGE_LOGO=OFF \
	-DENABLE_ZLIB=OFF \
	-DENABLE_LUA=OFF \
	-DENABLE_LIBZFS=OFF

$(eval $(cmake-package))
