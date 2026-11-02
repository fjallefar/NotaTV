# Twitch (dev.twitch.tv): the Client ID is read from the private file secrets/twitch-client-id (not in git)
CFLAGS_twitch := -DTW_CLIENT_ID=\"$(shell cat $(SECRETS)/twitch-client-id 2>/dev/null)\"
