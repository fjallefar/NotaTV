# NotaTV shared shell functions
CONF=/data/notatv/notatv.conf
RUN=/run/notatv
H=/usr/libexec/notatv

cfg_get() { # key [default]
	v=$(grep -m1 "^$1=" "$CONF" 2>/dev/null | cut -d= -f2-)
	[ -n "$v" ] && echo "$v" || echo "$2"
}

cfg_set() { # key value  (only used by scripts when tvshell is not running)
	mkdir -p /data/notatv
	touch "$CONF"
	grep -v "^$1=" "$CONF" > "$CONF.tmp" 2>/dev/null
	# no line breaks/control characters: one setting per line
	printf '%s=%s\n' "$1" "$(printf '%s' "$2" | tr '\000-\037\177' ' ')" >> "$CONF.tmp"
	mv "$CONF.tmp" "$CONF"
}

log() { echo "[$(date +%T)] $*" >> /tmp/notatv.log; }
progress() { echo "$*" > /tmp/notatv-progress; log "$*"; }
# message to the user interface: English text with %s and arguments after \037 (tvshell translates, see tr_msg)
msg() { f=$1; shift; printf '%s' "$f"; for a in "$@"; do printf '\037%s' "$a"; done; printf '\n'; }
device_name() { cfg_get device_name NotaTV; }

# the data partition (label NOTADATA) on the same disk as the root filesystem - never one on another disk, e.g. a
# USB stick someone labelled NOTADATA (its settings and app packages would be used, as root)
data_dev() {
	mm=$(mountpoint -d / 2>/dev/null) || return 1
	p=$(readlink -f "/sys/dev/block/$mm") || return 1
	[ -e "$p/partition" ] || return 1
	for q in "${p%/*}"/*/partition; do
		[ -e "$q" ] || continue
		d=/dev/$(basename "${q%/partition}")
		blkid "$d" 2>/dev/null | grep -q 'LABEL="NOTADATA"' && { echo "$d"; return 0; }
	done
	return 1
}

# the boot partition (label NOTABOOT): partition 1 on the same disk as the root filesystem (the updater has its own copy)
boot_dev() {
	mm=$(mountpoint -d / 2>/dev/null) || return 1
	p=$(readlink -f "/sys/dev/block/$mm") || return 1
	[ -e "$p/partition" ] || return 1
	for q in "${p%/*}"/*/partition; do
		[ "$(cat "$q" 2>/dev/null)" = 1 ] || continue
		d=/dev/$(basename "${q%/partition}")
		blkid "$d" 2>/dev/null | grep -q 'LABEL="NOTABOOT"' && { echo "$d"; return 0; }
	done
	return 1
}
