# Tamagotcho — local release helpers.
#
# The GitHub release workflow builds the same artifacts on a tag. These targets
# are for building the dist/ directory locally.

PYTHON ?= python3
VERSION ?=

.PHONY: dist test clean-dist

# Build all four release environments and the release assets.
dist:
	@test -n "$(VERSION)" || { echo "Usage: make dist VERSION=vX.Y.Z" >&2; exit 1; }
	$(PYTHON) tools/release/build_dist.py --version "$(VERSION)"

test:
	cd firmware && pio test -e native
	$(PYTHON) -m unittest discover -s tools/release -v

clean-dist:
	rm -rf dist
