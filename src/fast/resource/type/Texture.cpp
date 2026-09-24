#include "fast/resource/type/Texture.h"
#include "ship/Context.h"
#include "ship/resource/ResourceManager.h"
#include "ship/resource/archive/ArchiveManager.h"
#include <spdlog/spdlog.h>
#ifndef _WIN32
#include <sys/mman.h>
#endif

namespace Fast {
Texture::Texture() : Resource(std::shared_ptr<Ship::ResourceInitData>()) {
}

uint8_t* Texture::GetPointer() {
    // QuestShip: game code reading texture bytes (pause map, minimaps, ...) gets real RGBA pixels.
    return IsAstc ? CpuPixels() : ImageData;
}

size_t Texture::GetPointerSize() {
    return IsAstc ? CpuPixelsSize() : ImageDataSize;
}

uint8_t* Texture::AstcSentinel() {
    std::lock_guard<std::mutex> lock(mAstcMutex);
    if (mSentinel == nullptr) {
        mSentinelSize = std::max<size_t>((size_t)PixelWidth * PixelHeight * 4, 4096);
#ifndef _WIN32
        void* p = mmap(nullptr, mSentinelSize, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); // zero pages, no RAM
        mSentinel = (p == MAP_FAILED) ? nullptr : static_cast<uint8_t*>(p);
#endif
    }
    return mSentinel;
}

uint8_t* Texture::CpuPixels() {
    if (!IsAstc) {
        return ImageData;
    }
    std::lock_guard<std::mutex> lock(mAstcMutex);
    if (!mFallbackTried) {
        mFallbackTried = true;
        const std::string& path = GetInitData()->Path;
        auto rm = Ship::Context::GetRawInstance()->GetResourceManager();
        std::shared_ptr<Ship::Archive> from;
        auto file = rm->GetArchiveManager()->LoadFileFromLowerArchives(path, &from);
        if (file != nullptr) {
            mFallback = std::dynamic_pointer_cast<Texture>(rm->GetResourceLoader()->LoadResource(path, file));
        }
        if (mFallback == nullptr) {
            SPDLOG_ERROR("[ASTC] no RGBA fallback for {} (is the source pack still installed?)", path);
        } else {
            static bool sLogged = false;
            if (!sLogged) {
                sLogged = true;
                SPDLOG_INFO("[ASTC] RGBA fallback source: {}", from ? from->GetPath() : "?");
            }
        }
    }
    return mFallback ? mFallback->ImageData : nullptr;
}

uint32_t Texture::CpuPixelsSize() {
    return (IsAstc && CpuPixels() != nullptr) ? mFallback->ImageDataSize : (IsAstc ? 0 : ImageDataSize);
}

Texture::~Texture() {
    if (ImageData != nullptr && !mImageBuffer) {
        delete[] ImageData;
    }
#ifndef _WIN32
    if (mSentinel != nullptr) {
        munmap(mSentinel, mSentinelSize);
    }
#endif
}
} // namespace Fast
