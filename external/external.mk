include $(sort $(wildcard $(BR2_EXTERNAL_NOTATV_PATH)/package/*/*.mk))

# SDL's "offscreen" video driver (EGL without a display): used on the Raspberry Pi, where tvshell shows
# the UI itself with scaling in the display hardware (src/shell/kms_present.c). Build setup only - SDL's code
# is unchanged.
SDL3_CONF_OPTS += -DSDL_OFFSCREEN=ON

# The kernel stores "user@host" from the build machine (uname -a, /proc/version). Use a neutral name instead,
# so a release image does not reveal who or what built it.
LINUX_MAKE_ENV += KBUILD_BUILD_USER=notatv KBUILD_BUILD_HOST=notatv

# host-zstd only needs zstd itself. Without this it finds the build machine's lz4 headers, and lz4 1.11
# removed the old type names zstd 1.5.7 still uses (contentChecksum_t), so the build stops.
HOST_ZSTD_OPTS += HAVE_LZ4=0
