# NotaTV: shared code for the phone page's CGI scripts (www/cgi-bin/set, logs). Sourced, not run; kept outside the
# web root so httpd never serves or runs it.
CONF=/data/notatv/notatv.conf
# in /run/notatv (root only), not /tmp: no other user can put a file or symlink there first
FAIL=/run/notatv/phone-fail   # one line (time) per wrong code
LOCK=/run/notatv/phone.lock

reply() { printf 'Status: %s\r\nContent-Type: text/plain; charset=utf-8\r\n\r\n%s\n' "$1" "$2"; exit 0; }
cfg() { sed -n "s/^$1=//p" "$CONF" 2>/dev/null | head -1; }

# %XX and + decoded as plain bytes (no backslash escapes are interpreted, %00 is dropped)
urldecode() {
	printf '%s' "$1" | LC_ALL=C awk 'BEGIN { for (i = 1; i < 256; i++) h[sprintf("%02x", i)] = sprintf("%c", i) }
	{
		if (NR > 1) printf "\n"
		gsub(/\+/, " ")
		n = split($0, p, "%")
		printf "%s", p[1]
		for (i = 2; i <= n; i++) {
			c = tolower(substr(p[i], 1, 2))
			if (c in h) printf "%s%s", h[c], substr(p[i], 3)
			else if (c == "00") printf "%s", substr(p[i], 3)
			else printf "%%%s", p[i]
		}
	}'
}

# the phone page code: $1 = the code that was sent. At most 10 wrong codes in 10 minutes, counted under a lock so
# requests sent at the same time cannot all slip in under the limit. A wrong code is answered after 2 s.
pin_check() {
	pin=$(cfg phone_pin)
	[ -n "$pin" ] || reply "403 Forbidden" "Choose a code on the TV first: Settings → Network → Phone page"
	now=$(date +%s)
	exec 8> $LOCK
	flock 8
	if [ -f "$FAIL" ]; then
		awk -v n="$now" '$1 > n - 600' "$FAIL" > "$FAIL.tmp" 2>/dev/null; mv "$FAIL.tmp" "$FAIL"
		[ "$(wc -l < "$FAIL")" -ge 10 ] && reply "429 Too Many Requests" "Too many wrong codes - try again in 10 minutes"
	fi
	if [ "$1" != "$pin" ]; then
		echo "$now" >> "$FAIL"
		WRONG=$(wc -l < "$FAIL")
		flock -u 8
		sleep 2
		return 1
	fi
	flock -u 8
	return 0
}
