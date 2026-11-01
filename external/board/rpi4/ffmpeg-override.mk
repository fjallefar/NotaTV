# NotaTV Pi 4: Raspberry Pi's ffmpeg fork (v4l2-request for HEVC hardware decoding, H.264 via v4l2m2m)
# source: github.com/jc-kynesim/rpi-ffmpeg branch dev/6.1.3/rpi_import_2 (commit in board/rpi4/rpi-ffmpeg.commit, fetched by make rpi4)
FFMPEG_OVERRIDE_SRCDIR = $(BR2_EXTERNAL_NOTATV_PATH)/../src/rpi-ffmpeg
# fixes on top of the fork are in board/rpi4/rpi-ffmpeg-patches/ (already applied in src/rpi-ffmpeg)
