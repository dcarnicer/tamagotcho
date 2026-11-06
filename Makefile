# Tamagotcho — local release helpers.
#
# The GitHub release workflow builds the same artifacts on a tag. These targets
# are for building and packaging the dist/ directory locally.

PYTHON ?= python3
VERSION ?=
MERGED ?=

.PHONY: dist m5burner test clean-dist

# Build all four release environments and the M5Burner package.
dist:
	@test -n "$(VERSION)" || { echo "Usage: make dist VERSION=vX.Y.Z" >&2; exit 1; }
	$(PYTHON) tools/release/build_dist.py --version "$(VERSION)"

# Package an existing merged image (default: dist/tamagotcho-m5stack-merged.bin).
m5burner:
	@test -n "$(VERSION)" || { echo "Usage: make m5burner VERSION=vX.Y.Z [MERGED=path]" >&2; exit 1; }
	$(PYTHON) tools/release/build_dist.py --version "$(VERSION)" --m5burner-only \
		$(if $(MERGED),--merged "$(MERGED)",)

test:
	cd firmware && pio test -e native
	$(PYTHON) -m unittest discover -s tools/release -v

clean-dist:
	rm -rf dist
