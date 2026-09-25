#include "fast/resource/type/Texture.h"
#include "ship/Context.h"
#include "ship/resource/ResourceManager.h"
#include "ship/resource/archive/ArchiveManager.h"
#include <spdlog/spdlog.h>
#ifdef _WIN32
#include <windows.h>
#else
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

namespace {
std::mutex sFreedSentinelMutex;
std::vector<std::pair<uint8_t*, size_t>> sFreedSentinels;

void ReleaseRange(uint8_t* p, size_t size) {
#ifdef _WIN32
    (void)size;
    VirtualFree(p, 0, MEM_RELEASE);
#else
    munmap(p, size);
#endif
}
} // namespace

uint8_t* Texture::AstcSentinel() {
    std::lock_guard<std::mutex> lock(mAstcMutex);
    if (mSentinel == nullptr && mSentinelSize == 0) {
        mSentinelSize = std::max<size_t>((size_t)PixelWidth * PixelHeight * 4, 4096);
#ifdef _WIN32
        mSentinel = static_cast<uint8_t*>(VirtualAlloc(nullptr, mSentinelSize, MEM_RESERVE, PAGE_NOACCESS));
#else
        void* p = mmap(nullptr, mSentinelSize, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); // zero pages, no RAM
        mSentinel = (p == MAP_FAILED) ? nullptr : static_cast<uint8_t*>(p);
#endif
        if (mSentinel == nullptr) {
            SPDLOG_WARN("[ASTC] could not reserve an address range; {} uses its RGBA fallback",
                        GetInitData()->Path);
        }
    }
    return mSentinel; // stays null after a failed attempt (mSentinelSize != 0): no retry per draw
}

bool Texture::SentinelContains(const void* p) const {
    const uint8_t* b = mSentinel;
    return b != nullptr && p >= b && p < b + mSentinelSize;
}

void Texture::DrainFreedSentinels(void (*evict)(const uint8_t* addr)) {
    std::vector<std::pair<uint8_t*, size_t>> freed;
    {
        std::lock_guard<std::mutex> lock(sFreedSentinelMutex);
        if (sFreedSentinels.empty()) {
            return;
        }
        freed.swap(sFreedSentinels);
    }
    for (auto& [p, size] : freed) {
        evict(p);
        ReleaseRange(p, size);
    }
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
        const size_t want = (size_t)PixelWidth * PixelHeight * 4;
        if (mFallback != nullptr && (mFallback->ImageData == nullptr || mFallback->ImageDataSize < want)) {
            SPDLOG_ERROR("[ASTC] fallback for {} has {} bytes, expected {}; ignoring it", path,
                         mFallback->ImageDataSize, want);
            mFallback = nullptr;
        }
        if (mFallback == nullptr) {
            SPDLOG_ERROR("[ASTC] no RGBA fallback for {} (is the source pack still installed?)", path);
            mZeroPixels.assign(want, 0); // game code and the renderer get a blank image, never null
        } else {
            static bool sLogged = false;
            if (!sLogged) {
                sLogged = true;
                SPDLOG_INFO("[ASTC] RGBA fallback source: {}", from ? from->GetPath() : "?");
            }
        }
    }
    return mFallback ? mFallback->ImageData : mZeroPixels.data();
}

uint32_t Texture::CpuPixelsSize() {
    if (!IsAstc) {
        return ImageDataSize;
    }
    CpuPixels();
    return mFallback ? mFallback->ImageDataSize : (uint32_t)mZeroPixels.size();
}

Texture::~Texture() {
    if (ImageData != nullptr && !mImageBuffer) {
        delete[] ImageData;
    }
    if (mSentinel != nullptr) {
        std::lock_guard<std::mutex> lock(sFreedSentinelMutex);
        sFreedSentinels.emplace_back(mSentinel, mSentinelSize);
    }
}
} // namespace Fast
