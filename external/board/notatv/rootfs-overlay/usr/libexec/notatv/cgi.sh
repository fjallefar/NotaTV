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

# S6 (audit 9/11): the page sends everything encrypted when it has the key from the TV's QR code (#k=, never sent
# over the network): the body is "_box=<base64 of nonce + secretbox>" around the real form. phonebox opens it with
# /data/notatv/phone-key; a box that opens is also the login (only the TV's screen showed the key).
# auth_request: sets data to the real form and AUTH to "key" or "code" (the phone page code, as before).
# A box must carry _ts (the phone's clock, within 10 minutes of the box's) and is taken only once (a box seen on
# the network cannot be sent again).
KEYF=/data/notatv/phone-key
SEEN=/run/notatv/phone-seen
auth_request() {
	box=$(getval _box)
	if [ -n "$box" ]; then
		plain=$(urldecode "$box" | /usr/libexec/notatv/phonebox open $KEYF 2>/dev/null) ||
			reply "401 Unauthorized" "The QR code is old - scan the new one on the TV (Settings → Network → Connect a phone)"
		h=$(printf '%s' "$box" | sha256sum | cut -c1-32)
		exec 7> $LOCK.seen
		flock 7
		grep -qx "$h" $SEEN 2>/dev/null && reply "409 Conflict" "Sent twice - try again"
		{ tail -n 499 $SEEN 2>/dev/null; echo "$h"; } > $SEEN.tmp && mv $SEEN.tmp $SEEN
		flock -u 7
		data=$plain
		ts=$(getval _ts)
		case "$ts" in ''|*[!0-9]*) reply "400 Bad Request" "Missing time" ;; esac
		d=$(( $(date +%s) - ts ))
		[ $d -lt 0 ] && d=$((-d))
		[ $d -le 600 ] || reply "400 Bad Request" "The phone's clock and the box's differ - check the time on both"
		AUTH=key
		return 0
	fi
	AUTH=code
	return 1 # the caller checks the code (pin_check)
}
# fields that are logins or carry them (an IPTV address often has the user name and password in it): only accepted
# encrypted. Without the QR code the page can still send keys, text and the like with the code.
SECRET_FIELDS='_ytcookies xtream_server xtream_user xtream_pass iptv_url iptv_epg media_share'
