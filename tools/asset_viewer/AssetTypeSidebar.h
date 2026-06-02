#pragma once
// Left sidebar: the modder-facing asset-type vocabulary.
// Textures and Materials are live in stage 2; the remaining 7 are deferred.
enum class AssetType { Textures, Materials };
class AssetTypeSidebar {
public:
    void draw();
    AssetType active() const { return active_; }
private:
    AssetType active_ = AssetType::Textures;
};
