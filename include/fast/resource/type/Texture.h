#pragma once

#include "ship/resource/Resource.h"
#include <memory>
#include <mutex>
#include <vector>

#define TEX_FLAG_LOAD_AS_RAW (1 << 0)
#define TEX_FLAG_LOAD_AS_IMG (1 << 1)
// QuestShip: OTEX v2 - pixels are GPU-compressed ASTC with a prebuilt mip chain (questship-texconv)
#define TEX_FLAG_ASTC (1 << 2)

namespace Fast {
enum class TextureType {
    Error = 0,
    RGBA32bpp = 1,
    RGBA16bpp = 2,
    Palette4bpp = 3,
    Palette8bpp = 4,
    Grayscale4bpp = 5,
    Grayscale8bpp = 6,
    GrayscaleAlpha4bpp = 7,
    GrayscaleAlpha8bpp = 8,
    GrayscaleAlpha16bpp = 9,
};

class Texture final : public Ship::Resource<uint8_t> {
  public:
    using Resource::Resource;

    Texture();

    uint8_t* GetPointer() override;
    size_t GetPointerSize() override;

    TextureType Type;
    uint16_t Width, Height;
    uint32_t Flags = 0;
    float HByteScale = 1.0;
    float VPixelScale = 1.0;
    uint32_t ImageDataSize;
    uint8_t* ImageData = nullptr;
    // When set, ImageData points into this buffer and must not be delete[]-ed.
    std::shared_ptr<std::vector<char>> mImageBuffer;

    // QuestShip ASTC (OTEX v2). The texture has no CPU pixels of its own: ImageData stays null and
    // the renderer addresses it through a reserved, inaccessible address range (AstcSentinel) of
    // the RGBA byte size, so tile offsets and texture-cache keys work unchanged and a stray read
    // faults loudly. Anything that needs real pixels calls CpuPixels(), which lazily loads the
    // original RGBA version of the same path from the next archive down (the source pack).
    struct AstcLevel {
        uint32_t Width, Height, Size;
        const uint8_t* Data;
    };
    bool IsAstc = false;
    uint32_t AstcBlockX = 4, AstcBlockY = 4;
    uint32_t PixelWidth = 0, PixelHeight = 0; // actual RGBA/ASTC pixel dimensions (level 0)
    std::vector<AstcLevel> AstcLevels;
    uint8_t* AstcSentinel();
    uint8_t* CpuPixels();
    uint32_t CpuPixelsSize();

    ~Texture();

  private:
    std::mutex mAstcMutex;
    uint8_t* mSentinel = nullptr;
    size_t mSentinelSize = 0;
    std::shared_ptr<Texture> mFallback;
    bool mFallbackTried = false;
};
} // namespace Fast
