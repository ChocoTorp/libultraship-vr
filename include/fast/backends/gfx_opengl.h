#ifdef ENABLE_OPENGL
#pragma once

#include "gfx_rendering_api.h"
#include <deque>
#include "../interpreter.h"

#ifdef _MSC_VER
#include <SDL2/SDL.h>
// #define GL_GLEXT_PROTOTYPES 1
#include <GL/glew.h>
#elif FOR_WINDOWS
#include <GL/glew.h>
#include "SDL.h"
#define GL_GLEXT_PROTOTYPES 1
#include "SDL_opengl.h"
#elif __APPLE__
#include <SDL2/SDL.h>
#include <GL/glew.h>
#elif USE_OPENGLES
#include <SDL2/SDL.h>
#include <GLES3/gl3.h>
#else
#include <SDL2/SDL.h>
#define GL_GLEXT_PROTOTYPES 1
#include <SDL2/SDL_opengl.h>
#endif
namespace Fast {
struct ShaderProgram {
    GLuint openglProgramId;
    uint8_t numInputs;
    bool usedTextures[SHADER_MAX_TEXTURES];
    uint8_t numFloats;
    GLint attribLocations[16];
    uint8_t attribSizes[16];
    uint8_t numAttribs;
    GLint frameCountLocation;
    GLint noiseScaleLocation;
    GLint prim_depth_location;
    GLint texture_width_location;
    GLint texture_height_location;
    GLint texture_filtering_location;
    // QuestShip single-pass stereo: the same combiner compiled with a GL_OVR_multiview2 vertex
    // shader, built lazily and used whenever the bound target is a multiview framebuffer.
    uint64_t shaderId0 = 0;
    uint64_t shaderId1 = 0;
    bool multiview = false;
    ShaderProgram* mvTwin = nullptr;
    GLint vrViewProjLocation = -1;
    GLint vrWorldLocation = -1;
    // QuestShip: last values uploaded to this program (uniforms live in the program object), so
    // unchanged uniforms are not re-sent on every draw.
    uint32_t sentFrameCount = UINT32_MAX;
    float sentNoiseScale = -1.0f;
    float sentPrimDepth = -1.0f;
    uint32_t sentViewProjGen = 0;
    int8_t sentWorld = -1;
    GLint sentTex[6] = { -1, -1, -1, -1, -1, -1 }; // filtering[2], width[2], height[2]
};

struct FramebufferOGL {
    uint32_t width, height;
    bool has_depth_buffer;
    uint32_t msaa_level;
    bool invertY;

    GLuint fbo, clrbuf, clrbufMsaa, rbo;
};

struct TextureInfo {
    uint16_t width = 0;
    uint16_t height = 0;
    uint16_t filtering = 0;
    bool mipmapped = false; // QuestShip: has a mip chain (generated on upload, or prebuilt ASTC)
    // QuestShip: sampler state last set on this texture object (0 = unknown), so repeated
    // SetSamplerParameters calls with the same values skip the glTexParameter calls.
    GLint minFilter = 0, magFilter = 0, wrapS = 0, wrapT = 0;
    float aniso = 0.0f;
};

class GfxRenderingAPIOGL final : public GfxRenderingAPI {
  public:
    ~GfxRenderingAPIOGL() override = default;
    const char* GetName() override;
    int GetMaxTextureSize() override;
    GfxClipParameters GetClipParameters() override;
    void UnloadShader(ShaderProgram* oldPrg) override;
    void LoadShader(ShaderProgram* newPrg) override;
    ShaderProgram* CreateAndLoadNewShader(uint64_t shaderId0, uint64_t shaderId1) override;
    ShaderProgram* LookupShader(uint64_t shaderId0, uint64_t shaderId1) override;
    void ShaderGetInfo(ShaderProgram* prg, uint8_t* numInputs, bool usedTextures[2]) override;
    void ClearShaderCache() override;
    uint32_t NewTexture() override;
    void SelectTexture(int tile, uint32_t textureId) override;
    void UploadTexture(const uint8_t* rgba32Buf, uint32_t width, uint32_t height) override;
    bool UploadCompressedTexture(uint32_t blockX, uint32_t blockY, uint32_t levelCount, const uint32_t* widths,
                                 const uint32_t* heights, const uint8_t* const* data, const uint32_t* sizes) override;
    void SetSamplerParameters(int sampler, bool linear_filter, uint32_t cms, uint32_t cmt) override;
    void SetDepthTestAndMask(bool depth_test, bool z_upd) override;
    void SetCurrentPrimDepth(float depth) override;
    void SetZmodeDecal(bool decal) override;
    void SetViewport(int x, int y, int width, int height) override;
    void SetScissor(int x, int y, int width, int height) override;
    void SetUseAlpha(bool useAlpha) override;
    void DrawTriangles(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris) override;
    void Init() override;
    void OnResize() override;
    void StartFrame() override;
    void EndFrame() override;
    void FinishRender() override;
    int CreateFramebuffer() override;
    void UpdateFramebufferParameters(int fb_id, uint32_t width, uint32_t height, uint32_t msaa_level,
                                     bool opengl_invertY, bool render_target, bool has_depth_buffer,
                                     bool can_extract_depth) override;
    void StartDrawToFramebuffer(int fbId, float noiseScale) override;
    void CopyFramebuffer(int fbDstId, int fbSrcId, int srcX0, int srcY0, int srcX1, int srcY1, int dstX0, int dstY0,
                         int dstX1, int dstY1) override;
    void ClearFramebuffer(bool color, bool depth) override;
    void ClearDepthRegion(int x, int y, int w, int h) override;
    void ReadFramebufferToCPU(int fbId, uint32_t width, uint32_t height, uint16_t* rgba16Buf) override;
    void ResolveMSAAColorBuffer(int fbIdTarger, int fbIdSrc) override;
    std::unordered_map<std::pair<float, float>, uint16_t, hash_pair_ff>
    GetPixelDepth(int fb_id, const std::set<std::pair<float, float>>& coordinates) override;
    void* GetFramebufferTextureId(int fbId) override;
    void SelectTextureFb(int fbId) override;
    void DeleteTexture(uint32_t texId) override;
    void SetTextureFilter(FilteringMode mode) override;
    FilteringMode GetTextureFilter() override;
    void SetSrgbMode() override;
    ImTextureID GetTextureById(int id) override;

    // SOH [VR] (QuestShip) The VR layer renders into OpenXR swapchain images through FBOs it owns.
    // Binding one here registers it as a framebuffer slot, so viewport/scissor math, clip Y-inversion
    // and noise all see the XR image's real size (GL convention, not inverted).
    void BindExternalFramebuffer(GLuint fbo, uint32_t width, uint32_t height, bool multiview = false);
    // QuestShip single-pass stereo: both eyes' view-projection (2 x row-major 4x4, row-vector
    // convention) for the multiview vertex shader, and world- vs clip-space positions.
    void SetStereoViewProj(const float* vp32);
    void SetStereoWorldSpace(bool worldSpace) override;
    void ClearCurrentFramebuffer(float r, float g, float b, float a, bool depth);

  private:
    void SetUniforms(ShaderProgram* prg) const;
    std::string BuildFsShader(const CCFeatures& cc_features);
    void BuildProgram(uint64_t shaderId0, uint64_t shaderId1, bool multiview, ShaderProgram* prg);
    ShaderProgram* MultiviewTwin(ShaderProgram* prg);
    void SetMultiviewTarget(bool multiview);
    void SetPerDrawUniforms();

    std::vector<TextureInfo> textures;
    GLuint mCurrentTextureIds[SHADER_MAX_TEXTURES] = {};
    GLuint mLastBoundTextures[SHADER_MAX_TEXTURES] = {};
    uint8_t mCurrentTile;
    int8_t mLastActiveTexture = -1;
    int8_t mLastBlendEnabled = -1;
    int8_t mLastScissorEnabled = -1;

    std::map<std::pair<uint64_t, uint32_t>, ShaderProgram> mShaderProgramPool;
    std::map<std::pair<uint64_t, uint32_t>, ShaderProgram> mMvShaderProgramPool; // QuestShip multiview twins
    ShaderProgram* mRequestedShader = nullptr; // what the interpreter asked for (base variant)
    bool mMultiviewTarget = false;
    bool mStereoWorld = false;
    float mStereoViewProj[32] = {};
    uint32_t mStereoViewProjGen = 1; // bumped whenever mStereoViewProj changes
    ShaderProgram* mCurrentShaderProgram = nullptr;
    ShaderProgram* mLastLoadedShader = nullptr;

    GLuint mOpenglVbo = 0;
    float mMaxAnisotropy = 0.0f; // QuestShip: 0 = EXT_texture_filter_anisotropic unavailable
    float mAnisotropy = 1.0f;    // gTextureAnisotropy, clamped, read once per frame
    GLint mMaxTextureSize = 0;   // cached GL_MAX_TEXTURE_SIZE
    void FinishTextureUpload(TextureInfo& info, bool mipmapped);
    bool mAstcSupported = false;  // QuestShip: GL_KHR_texture_compression_astc_ldr

    // QuestShip: persistent-mapped vertex ring (EXT_buffer_storage), see DrawTriangles.
    struct VboFence {
        GLsync fence;
        size_t start, end; // byte range written that frame (start > end = wrapped)
    };
    uint8_t* mVboMapped = nullptr;
    size_t mVboCursor = 0;
    size_t mVboFrameStart = 0;
    std::deque<VboFence> mVboFences;
    void VboWaitFor(size_t off, size_t bytes);
#if defined(__APPLE__) || defined(USE_OPENGLES)
    GLuint mOpenglVao;
#endif

    uint32_t mFrameCount = 0;

    std::vector<FramebufferOGL> mFrameBuffers;
    size_t mCurrentFrameBuffer = 0;
    size_t mExternalFrameBuffer = 0; // slot used by BindExternalFramebuffer, 0 = not created yet
    float mCurrentNoiseScale = 0.0f;
    FilteringMode mCurrentFilterMode = FILTER_THREE_POINT;

    GLint mMaxMsaaLevel = 1;
    GLuint mPixelDepthRb = 0;
    GLuint mPixelDepthFb = 0;
    size_t mPixelDepthRbSize = 0;
};

} // namespace Fast
#endif
