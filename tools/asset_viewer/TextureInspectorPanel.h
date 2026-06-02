#pragma once
class TexturePreview2D;
// STAGE 1: inspects TexturePreview2D concretely (uses hasError()/metadata()/
// sourcePath(), which are NOT on PreviewSurface). STAGE 2: when a second
// PreviewSurface impl (MaterialPreviewPBR) arrives, lift the needed accessors
// onto PreviewSurface or specialize this panel per asset type.
// Draws the active texture preview + its metadata readout.
class TextureInspectorPanel {
public:
    void draw(TexturePreview2D& surface);
};
