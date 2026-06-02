#pragma once
class TexturePreview2D;
// Draws the active texture preview + its metadata readout.
class TextureInspectorPanel {
public:
    void draw(TexturePreview2D& surface);
};
