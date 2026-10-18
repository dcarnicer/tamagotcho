#pragma once
// The firmware version: the release tag ("v0.1.0") in a release build, `git
// describe` ("v0.1.0-3-gabc1234-dirty", or a bare commit id) in a local or CI
// build, "dev" without git. scripts/version.py defines TGC_VERSION for
// Version.cpp only; everything else reads it here (docs/decisions.md,
// 2026-10-18 "About screen and firmware version").
namespace tgc {

const char* firmwareVersion();

}  // namespace tgc
