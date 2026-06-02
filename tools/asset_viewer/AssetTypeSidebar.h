#pragma once
// Left sidebar: the modder-facing asset-type vocabulary. Only "Textures" is
// enabled in stage 1; the rest are visible-but-disabled (directional, no logic).
enum class AssetType { Textures };
class AssetTypeSidebar {
public:
    void draw();
    AssetType active() const { return AssetType::Textures; }
};
