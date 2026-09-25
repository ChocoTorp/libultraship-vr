#include "ship/resource/archive/TexturePackOptimizer.h"

#if defined(INCLUDE_MPQ_SUPPORT) && defined(QUESTSHIP_ASTC_OPTIMIZER)

#include <StormLib.h>
#include <zip.h>
#include <zlib.h>
#include "astcenc.h"
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>

#ifdef __ANDROID__
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace Ship {
namespace TexturePackOptimizer {

const char* const kOutputName = "zz_questship_astc";

namespace {

namespace fs = std::filesystem;

constexpr uint32_t kOTEX = 0x4F544558;
constexpr uint32_t kHeaderSize = 64;
constexpr uint32_t kFlagLoadAsRaw = 1;
constexpr uint32_t kFlagAstc = 4;
constexpr uint32_t kBlock = 4;         // ASTC 4x4
constexpr uint32_t kMinSide = 64;      // smaller textures gain little
constexpr double kMinPsnr = 36.0;      // below this the original keeps serving the texture
constexpr unsigned kWorkers = 2;       // leave the game thread and the driver their cores
constexpr size_t kMaxInFlight = 2;     // textures read ahead of the workers
constexpr size_t kMaxBytesInFlight = 48u << 20; // bounds memory: RGBA bytes read but not yet converted
constexpr int kStartDelayS = 30;       // let the game finish loading before competing for memory

std::atomic<int> sState{ 0 }; // 0 idle, 1 running, 2 done, 3 failed
std::atomic<size_t> sDone{ 0 }, sTotal{ 0 }, sWritten{ 0 };

std::string OutPath(const std::string& modsDir, const char* ext) {
    return (fs::path(modsDir) / (std::string(kOutputName) + ext)).string();
}

// The optimized archive is valid for exactly the set of packs it was built from (name + size).
std::string Fingerprint(const std::string& modsDir) {
    std::vector<std::string> lines;
    std::error_code ec;
    for (const auto& e : fs::recursive_directory_iterator(modsDir, ec)) {
        if (!e.is_regular_file()) {
            continue;
        }
        const std::string name = e.path().filename().string();
        const std::string ext = e.path().extension().string();
        if (name.rfind(kOutputName, 0) == 0 || (ext != ".otr" && ext != ".o2r")) {
            continue;
        }
        lines.push_back(fs::relative(e.path(), modsDir, ec).generic_string() + "|" +
                        std::to_string(e.file_size(ec)));
    }
    std::sort(lines.begin(), lines.end());
    std::string out;
    for (const auto& l : lines) {
        out += l + "\n";
    }
    return out;
}

std::string ReadText(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

uint32_t rd32(const uint8_t* p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}
float rdf(const uint8_t* p) {
    float v;
    memcpy(&v, p, 4);
    return v;
}
void wr32(std::vector<uint8_t>& v, uint32_t x) {
    const uint8_t* b = reinterpret_cast<const uint8_t*>(&x);
    v.insert(v.end(), b, b + 4);
}
void wr16(std::vector<uint8_t>& v, uint16_t x) {
    const uint8_t* b = reinterpret_cast<const uint8_t*>(&x);
    v.insert(v.end(), b, b + 2);
}

bool ReadMpqFile(HANDLE mpq, const std::string& path, std::vector<uint8_t>& out) {
    HANDLE f;
    if (!SFileOpenFileEx(mpq, path.c_str(), 0, &f)) {
        return false;
    }
    const DWORD size = SFileGetFileSize(f, nullptr);
    out.resize(size);
    DWORD read = 0;
    const bool ok = size > 0 && SFileReadFile(f, out.data(), size, &read, nullptr) && read == size;
    SFileCloseFile(f);
    return ok;
}

std::vector<std::string> MpqList(HANDLE mpq) {
    std::vector<std::string> paths;
    std::vector<uint8_t> list;
    if (!ReadMpqFile(mpq, "(listfile)", list)) {
        return paths;
    }
    std::string text(list.begin(), list.end());
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        std::string p = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = nl == std::string::npos ? text.size() : nl + 1;
        while (!p.empty() && (p.back() == '\r' || p.back() == ' ')) {
            p.pop_back();
        }
        if (!p.empty()) {
            paths.push_back(p);
        }
    }
    return paths;
}

struct Job {
    std::string path;
    std::vector<uint8_t> file;
    uint32_t pw = 0, ph = 0;
};

// Same eligibility as tools/texconv: HD RGBA8 raw replacements (not palette formats), >= 64 px.
bool Classify(Job& job) {
    const auto& file = job.file;
    if (file.size() < kHeaderSize + 28) {
        return false;
    }
    const uint8_t* h = file.data();
    if (h[0] != 0 || rd32(h + 4) != kOTEX || rd32(h + 8) != 1) {
        return false;
    }
    const uint8_t* b = h + kHeaderSize;
    const uint32_t type = rd32(b), w = rd32(b + 4), ht = rd32(b + 8), flags = rd32(b + 12);
    const float hs = rdf(b + 16), vs = rdf(b + 20);
    const uint32_t size = rd32(b + 24);
    if (type == 3 || type == 4 || !(flags & kFlagLoadAsRaw) || file.size() < kHeaderSize + 28 + (size_t)size) {
        return false;
    }
    if ((uint64_t)w * ht * 4 == size) {
        job.pw = w;
        job.ph = ht;
    } else {
        const uint32_t sw = (uint32_t)std::lround(w * hs), sh = (uint32_t)std::lround(ht * vs);
        if ((uint64_t)sw * sh * 4 != size) {
            return false;
        }
        job.pw = sw;
        job.ph = sh;
    }
    return job.pw >= kMinSide && job.ph >= kMinSide;
}

std::vector<uint8_t> Downsample(const std::vector<uint8_t>& src, uint32_t w, uint32_t h, uint32_t& ow, uint32_t& oh) {
    ow = std::max(1u, w / 2);
    oh = std::max(1u, h / 2);
    std::vector<uint8_t> dst((size_t)ow * oh * 4);
    for (uint32_t y = 0; y < oh; y++) {
        for (uint32_t x = 0; x < ow; x++) {
            float c[3] = { 0, 0, 0 }, cu[3] = { 0, 0, 0 }, a = 0;
            for (uint32_t dy = 0; dy < 2; dy++) {
                for (uint32_t dx = 0; dx < 2; dx++) {
                    const uint32_t sx = std::min(w - 1, x * 2 + dx), sy = std::min(h - 1, y * 2 + dy);
                    const uint8_t* p = &src[((size_t)sy * w + sx) * 4];
                    for (int k = 0; k < 3; k++) {
                        c[k] += p[k] * (float)p[3];
                        cu[k] += p[k];
                    }
                    a += p[3];
                }
            }
            uint8_t* d = &dst[((size_t)y * ow + x) * 4];
            for (int k = 0; k < 3; k++) {
                const float v = a > 0 ? c[k] / a : cu[k] / 4.0f; // alpha-weighted: no dark cutout halos
                d[k] = (uint8_t)std::lround(std::min(255.0f, std::max(0.0f, v)));
            }
            d[3] = (uint8_t)std::lround(a / 4.0f);
        }
    }
    return dst;
}

double Psnr(const uint8_t* a, const uint8_t* b, size_t n) {
    double se = 0;
    for (size_t i = 0; i < n; i++) {
        const double d = (double)a[i] - (double)b[i];
        se += d * d;
    }
    const double mse = se / n;
    return mse <= 0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

// Builds the OTEX v2 resource; empty on failure or when quality is too low.
std::vector<uint8_t> Convert(const Job& job, astcenc_context* ctx) {
    const uint8_t* pixels = job.file.data() + kHeaderSize + 28;
    std::vector<uint8_t> level(pixels, pixels + (size_t)job.pw * job.ph * 4);
    uint32_t w = job.pw, h = job.ph;

    std::vector<uint8_t> out(job.file.begin(), job.file.begin() + kHeaderSize);
    const uint32_t v2 = 2;
    memcpy(&out[8], &v2, 4);
    const uint8_t* b = job.file.data() + kHeaderSize;
    out.insert(out.end(), b, b + 12);             // type, width, height
    wr32(out, rd32(b + 12) | kFlagAstc);          // flags
    out.insert(out.end(), b + 16, b + 24);        // hByteScale, vPixelScale
    wr32(out, 0);                                 // no RGBA payload
    wr32(out, kBlock);
    wr32(out, kBlock);
    uint32_t levels = 1;
    for (uint32_t lw = w, lh = h; lw > 1 || lh > 1; levels++) {
        lw = std::max(1u, lw / 2);
        lh = std::max(1u, lh / 2);
    }
    wr32(out, levels);

    const astcenc_swizzle swz = { ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A };
    for (uint32_t l = 0; l < levels; l++) {
        const size_t blocks = (size_t)((w + kBlock - 1) / kBlock) * ((h + kBlock - 1) / kBlock);
        std::vector<uint8_t> comp(blocks * 16);
        void* slice = level.data();
        astcenc_image img{};
        img.dim_x = w;
        img.dim_y = h;
        img.dim_z = 1;
        img.data_type = ASTCENC_TYPE_U8;
        img.data = &slice;
        if (astcenc_compress_image(ctx, &img, &swz, comp.data(), comp.size(), 0) != ASTCENC_SUCCESS) {
            return {};
        }
        astcenc_compress_reset(ctx);
        if (l == 0) {
            std::vector<uint8_t> back(level.size());
            void* bslice = back.data();
            astcenc_image bimg = img;
            bimg.data = &bslice;
            if (astcenc_decompress_image(ctx, comp.data(), comp.size(), &bimg, &swz, 0) == ASTCENC_SUCCESS &&
                Psnr(level.data(), back.data(), back.size()) < kMinPsnr) {
                return {}; // keep the original at full quality
            }
            astcenc_decompress_reset(ctx);
        }
        wr32(out, w);
        wr32(out, h);
        wr32(out, (uint32_t)comp.size());
        out.insert(out.end(), comp.begin(), comp.end());
        if (l + 1 < levels) {
            uint32_t ow, oh;
            level = Downsample(level, w, h, ow, oh);
            w = ow;
            h = oh;
        }
    }
    return out;
}

// Minimal streaming ZIP writer (stored entries, sizes and CRC known up front): entries go to disk
// as they are produced, so memory stays bounded however large the pack is.
class ZipStream {
  public:
    explicit ZipStream(const std::string& path) : mFile(fopen(path.c_str(), "wb")) {
    }
    ~ZipStream() {
        if (mFile) {
            fclose(mFile);
        }
    }
    bool Ok() const {
        return mFile != nullptr && !mError;
    }
    void Add(const std::string& name, const std::vector<uint8_t>& data) {
        if (!Ok()) {
            return;
        }
        Entry e{ name, (uint32_t)crc32(0L, data.data(), (uInt)data.size()), (uint32_t)data.size(), mOffset };
        std::vector<uint8_t> h;
        wr32(h, 0x04034b50);
        wr16(h, 20);            // version needed
        wr16(h, 0x0800);        // UTF-8 names
        wr16(h, 0);             // stored
        wr16(h, 0);
        wr16(h, 0x21);          // time/date: 1980-01-01
        wr32(h, e.crc);
        wr32(h, e.size);
        wr32(h, e.size);
        wr16(h, (uint16_t)name.size());
        wr16(h, 0);
        h.insert(h.end(), name.begin(), name.end());
        Write(h.data(), h.size());
        Write(data.data(), data.size());
        mEntries.push_back(e);
    }
    bool Finish() {
        if (!Ok()) {
            return false;
        }
        const uint32_t cdStart = mOffset;
        for (const auto& e : mEntries) {
            std::vector<uint8_t> c;
            wr32(c, 0x02014b50);
            wr16(c, 20);
            wr16(c, 20);
            wr16(c, 0x0800);
            wr16(c, 0);
            wr16(c, 0);
            wr16(c, 0x21);
            wr32(c, e.crc);
            wr32(c, e.size);
            wr32(c, e.size);
            wr16(c, (uint16_t)e.name.size());
            wr16(c, 0);
            wr16(c, 0);
            wr16(c, 0);
            wr16(c, 0);
            wr32(c, 0);
            wr32(c, e.offset);
            c.insert(c.end(), e.name.begin(), e.name.end());
            Write(c.data(), c.size());
        }
        std::vector<uint8_t> end;
        wr32(end, 0x06054b50);
        wr16(end, 0);
        wr16(end, 0);
        wr16(end, (uint16_t)mEntries.size());
        wr16(end, (uint16_t)mEntries.size());
        wr32(end, mOffset - cdStart);
        wr32(end, cdStart);
        wr16(end, 0);
        Write(end.data(), end.size());
        const bool ok = Ok() && mEntries.size() < 0xFFFF && fflush(mFile) == 0;
        fclose(mFile);
        mFile = nullptr;
        return ok;
    }

  private:
    struct Entry {
        std::string name;
        uint32_t crc, size, offset;
    };
    void Write(const void* p, size_t n) {
        if (fwrite(p, 1, n, mFile) != n || (uint64_t)mOffset + n > 0xFFFFFFFFull) {
            mError = true;
        }
        mOffset += (uint32_t)n;
    }
    FILE* mFile;
    bool mError = false;
    uint32_t mOffset = 0;
    std::vector<Entry> mEntries;
};

void Run(std::string modsDir, std::vector<std::string> mods) {
#ifdef __ANDROID__
    setpriority(PRIO_PROCESS, gettid(), 10); // background: the game thread always wins
#endif
    // Starting while the game is still loading pushed the app over the Quest's memory limit.
    std::this_thread::sleep_for(std::chrono::seconds(kStartDelayS));
    const auto t0 = std::chrono::steady_clock::now();
    const std::string fingerprint = Fingerprint(modsDir);

    // Pass 1: which archive finally provides each path (the last one loaded wins, as in the game).
    std::unordered_map<std::string, size_t> owner;
    for (size_t i = 0; i < mods.size(); i++) {
        const std::string ext = fs::path(mods[i]).extension().string();
        if (ext == ".otr") {
            HANDLE mpq;
            if (SFileOpenArchive(mods[i].c_str(), 0, MPQ_OPEN_READ_ONLY, &mpq)) {
                for (auto& p : MpqList(mpq)) {
                    owner[p] = i;
                }
                SFileCloseArchive(mpq);
            }
        } else if (ext == ".o2r") {
            int err = 0;
            if (zip_t* za = zip_open(mods[i].c_str(), ZIP_RDONLY, &err)) {
                const zip_int64_t n = zip_get_num_entries(za, 0);
                for (zip_int64_t k = 0; k < n; k++) {
                    if (const char* name = zip_get_name(za, (zip_uint64_t)k, 0)) {
                        owner[name] = i;
                    }
                }
                zip_close(za);
            }
        }
    }
    size_t total = 0;
    for (auto& [p, i] : owner) {
        if (fs::path(mods[i]).extension() == ".otr") {
            total++;
        }
    }
    sTotal = total;
    if (total == 0) {
        sState = 2;
        return;
    }

    const std::string partPath = OutPath(modsDir, ".o2r.part");
    ZipStream zipOut(partPath);
    std::mutex m;
    std::condition_variable cv;
    std::deque<Job> queue;
    size_t bytesInFlight = 0; // file bytes read and not yet converted (queued + in the workers)
    bool producerDone = false;

    auto worker = [&]() {
#ifdef __ANDROID__
        setpriority(PRIO_PROCESS, gettid(), 10);
#endif
        astcenc_config cfg;
        astcenc_context* ctx = nullptr;
        const bool ctxOk = astcenc_config_init(ASTCENC_PRF_LDR, kBlock, kBlock, 1, ASTCENC_PRE_FAST, 0, &cfg) ==
                               ASTCENC_SUCCESS &&
                           astcenc_context_alloc(&cfg, 1, &ctx, nullptr) == ASTCENC_SUCCESS;
        for (;;) {
            Job job;
            {
                std::unique_lock<std::mutex> lock(m);
                cv.wait(lock, [&] { return !queue.empty() || producerDone; });
                if (queue.empty()) {
                    break;
                }
                job = std::move(queue.front());
                queue.pop_front();
            }
            cv.notify_all();
            const size_t jobBytes = job.file.size();
            std::vector<uint8_t> out = ctxOk ? Convert(job, ctx) : std::vector<uint8_t>{};
            job.file = {};
            {
                std::lock_guard<std::mutex> lock(m);
                if (!out.empty()) {
                    zipOut.Add(job.path, out);
                    sWritten++;
                }
                bytesInFlight -= jobBytes;
            }
            cv.notify_all();
            sDone++;
        }
        if (ctx) {
            astcenc_context_free(ctx);
        }
    };
    std::vector<std::thread> workers;
    for (unsigned t = 0; t < kWorkers; t++) {
        workers.emplace_back(worker);
    }

    // Pass 2: read each path from its owning .otr (one handle per archive, this thread only).
    for (size_t i = 0; i < mods.size(); i++) {
        if (fs::path(mods[i]).extension() != ".otr") {
            continue;
        }
        HANDLE mpq;
        if (!SFileOpenArchive(mods[i].c_str(), 0, MPQ_OPEN_READ_ONLY, &mpq)) {
            continue;
        }
        for (auto& p : MpqList(mpq)) {
            auto it = owner.find(p);
            if (it == owner.end() || it->second != i) {
                continue; // a later archive overrides this path
            }
            owner.erase(it); // listed twice in one archive: handle once
            Job job;
            job.path = p;
            if (!ReadMpqFile(mpq, p, job.file) || !Classify(job)) {
                sDone++;
                continue;
            }
            std::unique_lock<std::mutex> lock(m);
            // Wait for room: few queued, and a byte budget (one oversized texture may still pass
            // alone, when nothing else is in flight).
            cv.wait(lock, [&] {
                return queue.size() < kMaxInFlight &&
                       (bytesInFlight == 0 || bytesInFlight + job.file.size() <= kMaxBytesInFlight);
            });
            bytesInFlight += job.file.size();
            queue.push_back(std::move(job));
            lock.unlock();
            cv.notify_all();
        }
        SFileCloseArchive(mpq);
    }
    {
        std::lock_guard<std::mutex> lock(m);
        producerDone = true;
    }
    cv.notify_all();
    for (auto& w : workers) {
        w.join();
    }

    std::error_code ec;
    if (sWritten == 0) {
        fs::remove(partPath, ec);
        sState = 2;
        return;
    }
    if (!zipOut.Finish()) {
        SPDLOG_ERROR("[TexOpt] writing {} failed", partPath);
        fs::remove(partPath, ec);
        sState = 3;
        return;
    }
    // Manifest first, then the archive: a crash in between leaves no archive (harmless).
    std::ofstream(OutPath(modsDir, ".manifest"), std::ios::binary | std::ios::trunc) << fingerprint;
    fs::rename(partPath, OutPath(modsDir, ".o2r"), ec);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    SPDLOG_INFO("[TexOpt] optimized {} textures in {:.0f} s -> {}.o2r (active from the next launch)",
                sWritten.load(), secs, kOutputName);
    sState = ec ? 3 : 2;
}

} // namespace

void PrepareAtBoot(const std::string& modsDir) {
    std::error_code ec;
    fs::remove(OutPath(modsDir, ".o2r.part"), ec); // an interrupted run starts over
    const std::string out = OutPath(modsDir, ".o2r");
    if (!fs::exists(out, ec)) {
        return;
    }
    if (ReadText(OutPath(modsDir, ".manifest")) != Fingerprint(modsDir)) {
        SPDLOG_INFO("[TexOpt] texture packs changed; discarding the old optimized archive");
        fs::remove(out, ec);
        fs::remove(OutPath(modsDir, ".manifest"), ec);
    }
}

void StartIfNeeded(const std::string& modsDir, const std::vector<std::string>& modPathsInLoadOrder) {
    std::error_code ec;
    if (sState != 0 || fs::exists(OutPath(modsDir, ".o2r"), ec)) {
        return;
    }
    std::vector<std::string> mods;
    bool anyOtr = false;
    for (const auto& p : modPathsInLoadOrder) {
        if (fs::path(p).stem().string() == kOutputName) {
            continue;
        }
        anyOtr |= fs::path(p).extension() == ".otr";
        mods.push_back(p);
    }
    if (!anyOtr) {
        return;
    }
    sState = 1;
    SPDLOG_INFO("[TexOpt] optimizing installed texture packs in the background");
    std::thread(Run, modsDir, std::move(mods)).detach();
}

std::string Status() {
    switch (sState.load()) {
        case 1: {
            const size_t total = std::max<size_t>(1, sTotal.load());
            char buf[128];
            snprintf(buf, sizeof(buf), "Optimizing texture pack: %d%% (applies next launch)",
                     (int)(100.0 * std::min(sDone.load(), total) / total));
            return buf;
        }
        case 2:
            return sWritten > 0 ? "Texture pack optimized: restart the game to use it" : "";
        case 3:
            return "Texture pack optimization failed (see log)";
        default:
            return "";
    }
}

} // namespace TexturePackOptimizer
} // namespace Ship

#else

namespace Ship {
namespace TexturePackOptimizer {
const char* const kOutputName = "zz_questship_astc";
void PrepareAtBoot(const std::string&) {
}
void StartIfNeeded(const std::string&, const std::vector<std::string>&) {
}
std::string Status() {
    return "";
}
} // namespace TexturePackOptimizer
} // namespace Ship

#endif
