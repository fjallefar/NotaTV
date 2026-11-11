# TV 2 Play: the Auth0 client id is read from the private file secrets/tv2-client-id (not in git)
CFLAGS_tv2 := -DTV2_CLIENT_ID=\"$(shell cat $(SECRETS)/tv2-client-id 2>/dev/null)\"

# wvserve: the local Widevine helper (our code, C++; the CDM headers in wvserve/cdm are Chromium's, BSD).
# It goes in the package next to app.so. Google's Widevine module itself is NOT in the package: the app
# downloads it on the box from Raspberry Pi's archive the first time (see cdm_fetch in tv2.c).
APP_EXTRA += $(OUT)/tv2/wvserve
$(OUT)/tv2/wvserve: tv2/wvserve/wvserve.cc tv2/wvserve/cdm_host.h $(wildcard tv2/wvserve/cdm/*.h)
	mkdir -p $(@D)
	$(CXX) -std=c++17 -O2 -Wall -Wextra -Itv2/wvserve $(shell $(PKG_CONFIG) --cflags libcurl) -o $@ $< $(LDFLAGS) -lcurl -ldl -lpthread
