# Peezy: flutter-pi, its libraries, Plezy's Flutter bundle and keyboard maps go in the package next to app.so
# (src/apps/peezy/build/, docs/peezy.md). They need the Flutter toolchain in tools/ and an rpi4 Buildroot build,
# so they are made on request: make -C src/apps peezy-extras (PEEZY_ALL=1 also builds everything first).
# apps-catalog.py leaves an app out when a "helpers" file is missing, so an app.so alone never reaches the App Store.
peezy-extras: $(OUT)/peezy/app.so
	peezy/build/package.sh $(OUT)/peezy $(if $(PEEZY_ALL),all)
.PHONY: peezy-extras
