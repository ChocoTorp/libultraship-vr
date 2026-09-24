#include "fast/resource/factory/TextureFactory.h"
#include "fast/resource/type/Texture.h"
#include "spdlog/spdlog.h"

namespace Fast {

std::shared_ptr<Ship::IResource>
ResourceFactoryBinaryTextureV0::ReadResource(std::shared_ptr<Ship::File> file,
                                             std::shared_ptr<Ship::ResourceInitData> initData) {
    if (!FileHasValidFormatAndReader(file, initData)) {
        return nullptr;
    }

    auto texture = std::make_shared<Texture>(initData);
    auto reader = std::get<std::shared_ptr<Ship::BinaryReader>>(file->Reader);

    texture->Type = (TextureType)reader->ReadUInt32();
    texture->Width = reader->ReadUInt32();
    texture->Height = reader->ReadUInt32();
    texture->ImageDataSize = reader->ReadUInt32();
    texture->mImageBuffer = file->Buffer;
    texture->ImageData = reinterpret_cast<uint8_t*>(file->Buffer->data() + reader->GetBaseAddress());

    return texture;
}

std::shared_ptr<Ship::IResource>
ResourceFactoryBinaryTextureV1::ReadResource(std::shared_ptr<Ship::File> file,
                                             std::shared_ptr<Ship::ResourceInitData> initData) {
    if (!FileHasValidFormatAndReader(file, initData)) {
        return nullptr;
    }

    auto texture = std::make_shared<Texture>(initData);
    auto reader = std::get<std::shared_ptr<Ship::BinaryReader>>(file->Reader);

    texture->Type = (TextureType)reader->ReadUInt32();
    texture->Width = reader->ReadUInt32();
    texture->Height = reader->ReadUInt32();
    texture->Flags = reader->ReadUInt32();
    texture->HByteScale = reader->ReadFloat();
    texture->VPixelScale = reader->ReadFloat();
    texture->ImageDataSize = reader->ReadUInt32();
    texture->mImageBuffer = file->Buffer;
    texture->ImageData = reinterpret_cast<uint8_t*>(file->Buffer->data() + reader->GetBaseAddress());

    return texture;
}

// QuestShip: OTEX v2 = v1 fields + ASTC mip chain (written by questship-texconv).
std::shared_ptr<Ship::IResource>
ResourceFactoryBinaryTextureV2::ReadResource(std::shared_ptr<Ship::File> file,
                                             std::shared_ptr<Ship::ResourceInitData> initData) {
    if (!FileHasValidFormatAndReader(file, initData)) {
        return nullptr;
    }

    auto texture = std::make_shared<Texture>(initData);
    auto reader = std::get<std::shared_ptr<Ship::BinaryReader>>(file->Reader);

    texture->Type = (TextureType)reader->ReadUInt32();
    texture->Width = reader->ReadUInt32();
    texture->Height = reader->ReadUInt32();
    texture->Flags = reader->ReadUInt32();
    texture->HByteScale = reader->ReadFloat();
    texture->VPixelScale = reader->ReadFloat();
    reader->ReadUInt32(); // RGBA payload size: always 0
    texture->AstcBlockX = reader->ReadUInt32();
    texture->AstcBlockY = reader->ReadUInt32();
    const uint32_t levels = reader->ReadUInt32();
    texture->mImageBuffer = file->Buffer;
    for (uint32_t l = 0; l < levels; l++) {
        Texture::AstcLevel level;
        level.Width = reader->ReadUInt32();
        level.Height = reader->ReadUInt32();
        level.Size = reader->ReadUInt32();
        level.Data = reinterpret_cast<const uint8_t*>(file->Buffer->data() + reader->GetBaseAddress());
        reader->Seek(level.Size, Ship::SeekOffsetType::Current);
        texture->AstcLevels.push_back(level);
    }
    if (texture->AstcLevels.empty()) {
        return nullptr;
    }
    texture->IsAstc = (texture->Flags & TEX_FLAG_ASTC) != 0;
    texture->PixelWidth = texture->AstcLevels[0].Width;
    texture->PixelHeight = texture->AstcLevels[0].Height;
    // The RGBA byte size the renderer's address math expects (as if it were the v1 texture).
    texture->ImageDataSize = texture->PixelWidth * texture->PixelHeight * 4;
    texture->ImageData = nullptr;
    return texture;
}
} // namespace Fast
