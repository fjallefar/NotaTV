# DRTV: the client id of DR's login (login.dr.dk) is read from the private file secrets/drtv-client-id (not in git).
# Without it the app works as before without an account; only "Sign in" is hidden.
CFLAGS_drtv := -DDR_CLIENT_ID=\"$(shell cat $(SECRETS)/drtv-client-id 2>/dev/null)\"
