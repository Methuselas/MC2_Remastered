#ifndef GOS_POSTPROCESS_H
#define GOS_POSTPROCESS_H

#include "utils/gl_utils.h"

struct glsl_program;

class gosPostProcess {
public:
    gosPostProcess();
    ~gosPostProcess();

    void init(int w, int h);
    void destroy();
    void resize(int w, int h);

    // Bind the HDR scene FBO so all rendering goes into it
    void beginScene();

    // Composite the HDR scene onto the default framebuffer via fullscreen quad
    void endScene();

    void renderSkybox(float sunDirX, float sunDirY, float sunDirZ);

    bool isHdriReady() const { return hdriReady_; }

    // Renders the HDRI background as a fullscreen triangle.
    // Assumes scene FBO is bound. Writes only color attachment 0.
    // viewMat, projMat are column-major 4x4 floats (16 floats each).
    void renderHdriSkybox(const float* viewMat, const float* projMat);

    void runBloom();

    // Shadow mapping
    void initShadows();
    void destroyShadows();
    GLuint getSceneNormalTexture() const { return sceneNormalTex_; }
    GLuint getSceneDepthTexture() const { return sceneDepthTex_; }
    GLuint getSceneColorTexture() const { return sceneColorTex_; }
    GLuint getSceneFBO() const { return sceneFBO_; }
    // VFX-SOFT-PARTICLES-MVP-1: a copy of the resolved scene depth, taken
    // before the in-scene particle flush so particle shaders can sample it
    // WITHOUT a read-from-bound-attachment feedback loop (sceneDepthTex_ is the
    // active depth attachment during the flush). Lazily allocated on the first
    // copy (gated by MC2_VFX_SOFT_PARTICLES at the bridge) -> reads 0 and costs
    // nothing until then. DEPTH24_STENCIL8, full-res, matches sceneDepthTex_ so
    // glCopyImageSubData is a same-format copy.
    GLuint getSceneDepthCopyTexture() const { return sceneDepthCopyTex_; }
    void   copySceneDepthForParticles();
    // M1.5: readback hook for RenderWorld::lookupAtPixel.
    GLuint getSceneObjectIdTex() const { return sceneObjectIdTex_; }
    // WATER-REFLECTION-RESOURCE-1: 1/4-res reflection target (substrate only;
    // no producer renders into it until Phase C -> the texture reads black).
    GLuint getWaterReflectionTexture() const { return waterReflColorTex_; }
    GLuint getWaterReflectionFBO()     const { return waterReflFBO_; }
    int    getWaterReflectionWidth()   const { return waterReflW_; }
    int    getWaterReflectionHeight()  const { return waterReflH_; }
    // F3: explicit sentinel clear for GBuffer1 (attachment 1).
    // Sets attachment 1 to (0.5, 0.5, 1.0, 0.0) — flat-up encoded normal,
    // alpha = 0.0 (post-shadow eligible). Must be called while MRT is bound
    // (i.e., after beginScene's glDrawBuffers(2)). Defense-in-depth: every
    // visible pixel either gets overwritten by an explicit rc_gbuffer1_*
    // writer or inherits this sentinel.
    void clearGBuffer1();
    GLuint getShadowTexture() const { return shadowDepthTex_; }
    const float* getLightSpaceMatrix() const { return staticLightSpaceMatrix_; }
    GLuint getShadowFBO() const { return shadowFBO_; }
    int getShadowMapSize() const { return shadowMapSize_; }
    void beginShadowPass();
    void beginShadowPassNoClear();  // accumulate into existing shadow map
    void endShadowPass();
    bool shadowsEnabled_;

    // Shadow debug overlay
    bool showShadowDebug_;        // master toggle for debug overlay
    int shadowDebugMode_;         // 0=static, 1=dynamic
    void drawShadowDebugOverlay();

    // Shadow tuning — live-adjustable via ImGui; defaults match former hardcoded values.
    // Applied in beginShadowPass / beginShadowPassNoClear (static shadow pass only).
    float shadowBiasFactor_ = 2.0f;   // glPolygonOffset factor
    float shadowBiasUnits_  = 4.0f;   // glPolygonOffset units

    // Static world-fixed shadows: accumulate over multiple frames
    void buildStaticLightMatrix(float sunDirX, float sunDirY, float sunDirZ,
                                float mapHalfExtent);
    bool staticLightMatrixBuilt() const { return staticLightMatrixBuilt_; }
    void markStaticLightMatrixBuilt() { staticLightMatrixBuilt_ = true; }
    // VPL-#shadow C-1: per-mission re-arm so the one-shot full-map static
    // shadow rebuilds on the next mission (against fresh blocks[]) instead
    // of freezing the previous mission's shadow. Called from Terrain::destroy.
    void resetStaticLightMatrix() { staticLightMatrixBuilt_ = false; }
    void setMapHalfExtent(float extent) { mapHalfExtent_ = extent; }
    float getMapHalfExtent() const { return mapHalfExtent_; }

    // Dynamic object shadows: camera-centered, re-rendered every frame
    void initDynamicShadows();
    void destroyDynamicShadows();
    // camFitCornersMC2 = 8 raw-MC2 frustum corners (clipToWorld-unprojected
    // + Stuff->MC2 swizzled by the caller). Builder clamps + fits the ortho.
    void buildDynamicLightMatrix(float sunDirX, float sunDirY, float sunDirZ,
                                 const float camFitCornersMC2[8][3]);
    GLuint getDynamicShadowTexture() const { return dynShadowDepthTex_; }
    GLuint getDynamicShadowFBO() const { return dynShadowFBO_; }
    const float* getDynamicLightSpaceMatrix() const { return dynamicLightSpaceMatrix_; }
    int getDynamicShadowMapSize() const { return dynShadowMapSize_; }

    // Toggles and parameters
    float exposure_;
    bool bloomEnabled_;
    bool fxaaEnabled_;
    bool tonemapEnabled_;
    float bloomIntensity_;
    float bloomThreshold_;
    // HDR-POST-SCAFFOLD-1 (Track V, MC2_HDR_POST): master gate for the HDR
    // post stack. Resolved once from env at init(). When false (default),
    // bloom + ACES tonemap are force-disabled in the composite regardless of
    // their own member flags, so default output is byte-identical to legacy.
    bool hdrPostEnabled_;

    void runScreenShadow();
    bool screenShadowEnabled_;
    int screenShadowDebug_;  // 0=normal, 1=visualize

    // Scene state — set by terrain draw, cleared each frame in beginScene()
    bool sceneHasTerrain_;
    bool prevFrameHadTerrain_;  // for clear color: blue-grey in gameplay, black in menus
    void markTerrainDrawn() { sceneHasTerrain_ = true; }

    // God rays
    bool godrayEnabled_;
    void runGodRays();
    void setSunScreenPos(float x, float y) { sunScreenPos_[0] = x; sunScreenPos_[1] = y; }

    // Shoreline
    bool shorelineEnabled_;
    void runShoreline();

    // SSAO-GTAO-LITE-MVP-1 (Track V, MC2_SSAO). Half-res world-space AO.
    // Default OFF -> runSSAO() is skipped entirely (byte-identical). Resolved
    // from env at init; tunables ImGui- and profile-adjustable.
    bool  ssaoEnabled_;
    int   ssaoDebug_;       // 0 = multiplicative apply, 1 = show AO grayscale
    float aoRadius_;        // world-unit sample radius
    float aoStrength_;      // 0..2 occlusion strength
    float aoBias_;          // window-depth compare bias
    float aoPower_;         // contrast curve
    void runSSAO();

    // LOWLIGHT-NIGHTVISION-MVP-1: pure-postprocess night-vision tunables.
    // Active only when the selected ViewMode is LowLight (5); the composite
    // branch is otherwise skipped (Visual byte-identical). Seeded from
    // MC2_VIEWMODE_LOWLIGHT_GAIN / _TINT at init; ImGui- and profile-adjustable.
    float lowLightGain_    = 2.5f;                 // luminance amplification
    float lowLightTint_[3] = { 0.7f, 1.0f, 0.6f }; // green-phosphor NV tint

    void setInverseViewProj(const float* m) { memcpy(inverseViewProj_, m, 16 * sizeof(float)); }
    void setViewProj(const float* m) { memcpy(viewProj_, m, 16 * sizeof(float)); }
    const float* getInverseViewProj() const { return inverseViewProj_; }
    const float* getViewProj() const { return viewProj_; }
    int getWidth()  const { return width_; }
    int getHeight() const { return height_; }

private:
    void createFBOs(int w, int h);
    void destroyFBOs();
    void createFullscreenQuad();
    void destroyFullscreenQuad();

    // Scene FBO (full resolution, HDR)
    GLuint sceneFBO_;
    GLuint sceneColorTex_;
    GLuint sceneDepthTex_;
    GLuint sceneNormalTex_;
    GLuint sceneObjectIdTex_ = 0;   // M1.5 R32UI MRT attachment-2 (gated on MC2_OBJECT_ID_BUFFER)
    GLuint sceneDepthCopyTex_ = 0;  // VFX-SOFT-PARTICLES-MVP-1 lazy depth copy (DEPTH24_STENCIL8)

    // Bloom ping-pong FBOs (half resolution)
    GLuint bloomFBO_[2];
    GLuint bloomColorTex_[2];

    // WATER-REFLECTION-RESOURCE-1: quarter-res reflection target (color + depth).
    // Allocated in createFBOs, freed in destroyFBOs, registered in
    // RenderResourceRegistry. No producer until Phase C (reads black).
    GLuint waterReflFBO_      = 0;
    GLuint waterReflColorTex_ = 0;
    GLuint waterReflDepthTex_ = 0;
    int    waterReflW_        = 0;
    int    waterReflH_        = 0;

    // Fullscreen quad
    GLuint quadVAO_;
    GLuint quadVBO_;

    // Composite shader
    glsl_program* compositeProg_;

    // Skybox
    glsl_program* skyboxProg_;

    // HDRI-SKY-1: background sky from a single equirect HDRI.
    // All HDRI state is null/false when MC2_HDRI_SKY=0 or load fails.
    GLuint        hdriTex_         = 0;
    glsl_program* hdriSkyboxProg_  = nullptr;
    bool          hdriEnabled_     = false;  // resolved once from env at init
    bool          hdriReady_       = false;  // true iff tex + program both valid
    GLuint        hdriDummyVao_    = 0;      // fallback when quadVAO_ unavailable

    // Bloom shaders
    glsl_program* bloomThresholdProg_;
    glsl_program* bloomBlurProg_;

    // Dimensions
    int width_;
    int height_;

    bool initialized_;

    // Shadow map
    GLuint shadowFBO_;
    GLuint shadowDepthTex_;
    GLuint shadowDummyColorTex_;  // AMD needs a color attachment for rasterization
    glsl_program* shadowDepthProg_;
    int shadowMapSize_;
    int savedViewport_[4];
    float staticLightSpaceMatrix_[16]; // world-fixed ortho, built once at map load
    bool staticLightMatrixBuilt_;      // true after light matrix is built (first frame)
    float mapHalfExtent_;              // half the map size in world units

    // Dynamic object shadow FBO (1024x1024, camera-centered, per-frame)
    GLuint dynShadowFBO_;
    GLuint dynShadowDepthTex_;
    GLuint dynShadowDummyColorTex_;
    int dynShadowMapSize_;
    float dynamicLightSpaceMatrix_[16];
    glsl_program* shadowDebugProg_;

    // Post-process screen shadow
    glsl_program* screenShadowProg_;
    float inverseViewProj_[16];
    float viewProj_[16];

    // God ray
    glsl_program* godrayProg_;
    GLuint godrayFBO_;
    GLuint godrayColorTex_;  // half-res
    float sunScreenPos_[2];

    // Shoreline
    glsl_program* shorelineProg_;

    // SSAO (half resolution)
    GLuint        ssaoFBO_      = 0;
    GLuint        ssaoColorTex_ = 0;   // GL_R16F half-res AO
    int           ssaoW_        = 0;
    int           ssaoH_        = 0;
    glsl_program* ssaoProg_      = nullptr;
    glsl_program* ssaoApplyProg_ = nullptr;
};

gosPostProcess* getGosPostProcess();

// MISSION-VISUAL-TUNING-1: free-function accessors for profile system.
float gos_GetExposure();
void  gos_SetExposure(float v);

// VIEWMODE-POSTPROCESS-PRESENTATION-1: view-mode framework accessors.
// Gate MC2_VIEWMODE_FRAMEWORK resolved once at init(). When OFF, GetSelectedViewMode
// returns 0 (Visual) regardless of what ImGui has set. Pattern mirrors gos_SetExposure.
bool gos_IsViewmodeFrameworkEnabled();
int  gos_GetSelectedViewMode();   // returns 0 when gate OFF
void gos_SetSelectedViewMode(int m);  // clamped to RenderCore::ViewMode range

// LOWLIGHT-NIGHTVISION-MVP-1 tunables (profile + ImGui). Clamped to safe ranges.
void  gos_SetLowLightGain(float v);
float gos_GetLowLightGain();
void  gos_SetLowLightTintG(float v); // green channel — the most impactful axis

// TRACK-V post stack accessors (resolved from env at init; see gos_postprocess.cpp).
bool gos_IsHdrPostEnabled();
// BLOOM-MVP-1 tunables (profile + ImGui).
void  gos_SetBloomThreshold(float v);
void  gos_SetBloomIntensity(float v);
float gos_GetBloomThreshold();
float gos_GetBloomIntensity();
// SSAO-GTAO-LITE-MVP-1 tunables (profile + ImGui).
bool  gos_IsSsaoEnabled();
void  gos_SetSsaoRadius(float v);
void  gos_SetSsaoStrength(float v);
void  gos_SetSsaoBias(float v);
float gos_GetSsaoRadius();
float gos_GetSsaoStrength();
float gos_GetSsaoBias();

#endif // GOS_POSTPROCESS_H
