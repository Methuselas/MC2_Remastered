// RenderCore/RenderDebugView.cpp
#include "RenderDebugView.h"

const char* RenderDebugViewName(RenderDebugView v) {
    switch (v) {
        case RenderDebugView::Final:         return "Final";
        case RenderDebugView::Albedo:        return "Albedo";
        case RenderDebugView::Normal:        return "Normal";
        case RenderDebugView::Roughness:     return "Roughness";
        case RenderDebugView::Metallic:      return "Metallic";
        case RenderDebugView::LightingOnly:  return "Lighting Only";
        case RenderDebugView::IblOnly:       return "IBL Only";
        case RenderDebugView::SpecularOnly:  return "Specular Only";
        case RenderDebugView::MaterialIdx:   return "Material Idx";
        case RenderDebugView::TexArrayLayer: return "Tex Array Layer";
        default:                             return "Unknown";
    }
}

const char* RenderDebugViewDescription(RenderDebugView v) {
    switch (v) {
        case RenderDebugView::Final:         return "Full shading result";
        case RenderDebugView::Albedo:        return "Raw albedo texture, no lighting";
        case RenderDebugView::Normal:        return "World normal mapped to [0,1]";
        case RenderDebugView::Roughness:     return "Roughness factor as grayscale";
        case RenderDebugView::Metallic:      return "Metallic factor as grayscale";
        case RenderDebugView::LightingOnly:  return "Lighting term only (no albedo)";
        case RenderDebugView::IblOnly:       return "IBL ambient contribution only";
        case RenderDebugView::SpecularOnly:  return "Direct specular contribution only";
        case RenderDebugView::MaterialIdx:   return "Material index as hashed palette";
        case RenderDebugView::TexArrayLayer: return "Texture array layer as hashed palette";
        default:                             return "";
    }
}
