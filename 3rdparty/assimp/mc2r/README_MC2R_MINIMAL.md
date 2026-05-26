# MC2R Assimp Minimal Vendor

This is a pruned Assimp source vendor for MC2R.

Kept importers:
- ASE
- OBJ
- FBX
- glTF/glTF2

Removed:
- tests
- tools
- samples
- most docs
- unused AssetLib importer directories
- large unused contrib payloads such as googletest/draco/tinyusdz

Build intent:
- static library only
- Windows + Linux
- importer support only
- no parent-project BUILD_SHARED_LIBS leakage
