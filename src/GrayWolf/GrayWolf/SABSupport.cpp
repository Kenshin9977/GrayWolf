#include "stdafx.h"

#include "SABSupport.h"

#include "BinaryReader.h"
#include "Strings.h"
#include "Sound.h"
#include "SettingsManager.h"
#include "WraithNameIndex.h"
#include "Encryption.h"
#include "FileSystems.h"

#include "CoDAssets.h"
#include "ICoDGame.h"
#include "DBGameFiles.h"
#include "ExportNaming.h"

#include <opus/opus.h>

#include <filesystem>
#include <format>
#include <iterator>
#include <unordered_map>

uint32_t HashSoundString(const std::string& Value)
{
    uint32_t Result = 5381;

    for (auto& Character : Value)
        Result = (uint32_t)(tolower(Character) + (Result << 6) + (Result << 16)) - Result;

    return Result;
}

uint64_t HashSoundStringV17(const std::string& Value)
{
    uint64_t Result = 0xCBF29CE484222325;

    for (uint32_t i = 0; i < Value.length(); i++)
    {
        Result ^= tolower(Value[i]);
        Result *= 0x100000001B3;
    }

    return Result;
}

bool SABSupport::ParseSAB(const std::string& FilePath)
{
    // TODO: make each SAB version its own class.

    auto Reader = BinaryReader();
    Reader.Open(FilePath);

    // Encrypted files, introduced in Black Ops 3
    HandleSABEncryption(Reader, FilePath);

    // As of Infinite Warfare, the SAB header remains unchanged, we can read it to determine the version
    auto Header = Reader.Read<SABFileHeader>();

    // Magic ('2UX#'), the same for all SAB files
    if (Header.Magic != 0x23585532)
    {
        return false;
    }

    // Every entry takes room in the file: a count beyond its length is a corrupt header
    if (Header.EntriesCount > Reader.GetLength())
    {
        return false;
    }

    // Name table offset sits right before the zone name (0x24C for the short-entry IW layout,
    // 0x250 otherwise). A zero offset, or a zero first entry, means the file has no names at all.
    Reader.SetPosition((Header.Version == 0x15 && Header.SizeOfAudioEntry == 0x38) ? 0x24C : 0x250);
    auto NameTableOffset = Reader.Read<uint64_t>();

    // If a file has names, every entry has one: a non-zero offset and a non-blank first entry tell.
    std::vector<std::string> SABFileNames;

    bool HasNames = (NameTableOffset > 0) && (Reader.Read<uint64_t>(NameTableOffset) != 0);

    // If we don't have names, we can pre-load a name index here, depending on the game
    WraithNameIndex SABNames;
    if (!HasNames)
    {
        // We have names for bo2 and bo3
        if (Header.Version == 0xE) { SABNames.LoadIndex((std::filesystem::path(FileSystems::GetApplicationPath()) / "package_index\\bo2_sab.wni").string()); }
        if (Header.Version == 0xF) { SABNames.LoadIndex((std::filesystem::path(FileSystems::GetApplicationPath()) / "package_index\\bo3_sab.wni").string()); }
    }

    if (HasNames)
    {
        SABFileNames.reserve(Header.EntriesCount);

        Reader.SetPosition(NameTableOffset);

        if (Header.Version == 0x15)
        {
            if (Header.SizeOfAudioEntry == 0x38)
            {
                for (uint32_t i = 0; i < Header.EntriesCount; i++)
                {
                    auto Name = Reader.ReadNullTerminatedString();
                    auto PositionAdvance = 256 - (Name.size() + 1);
                    SABFileNames.emplace_back(Name);
                    Reader.Advance(PositionAdvance);
                }
            }
            else
            {
                // BO4 SAB hashes are 63-bit (bit 63 never set, see WraithNameIndex::LoadIndex's KeyMask). A
                // dedicated bo4_sab.wni, not fnv1a_xsounds.wni, which mixes them with an unrelated 60-bit id
                // space; LoadIndex no-ops on a missing file.
                constexpr uint64_t Bo4SoundHashMask = 0x7FFFFFFFFFFFFFFFull;
                SABNames.LoadIndex((std::filesystem::path(FileSystems::GetApplicationPath()) / "package_index\\bo4_sab.wni").string(), Bo4SoundHashMask);
                // echo000/cod-name-db's fnv1a_xsounds (--update-name-db) names BO4 bank entries
                // too: a mixed id space only adds keys no lookup ever hits, so it is loaded as well.
                SABNames.LoadIndex((std::filesystem::path(FileSystems::GetApplicationPath()) / "package_index\\fnv1a_xsounds.wni").string(), Bo4SoundHashMask);

                for (uint32_t i = 0; i < Header.EntriesCount; i++)
                {
                    auto Hash = Reader.Read<uint64_t>() & Bo4SoundHashMask;
                    if (SABNames.NameDatabase.find(Hash) != SABNames.NameDatabase.end())
                    {
                        SABFileNames.emplace_back(SABNames.NameDatabase[Hash]);
                    }
                    else
                    {
                        SABFileNames.emplace_back(std::format("xsound_{:x}", Hash));
                    }
                    auto PositionAdvance = (Header.SizeOfNameEntry * 2) - 8;
                    Reader.Advance(PositionAdvance);
                }
            }
        }
        else
        {
            for (uint32_t i = 0; i < Header.EntriesCount; i++)
            {
                auto Name = Reader.ReadNullTerminatedString();
                auto PositionAdvance = ((size_t)Header.SizeOfNameEntry * 2) - (Name.size() + 1);
                SABFileNames.emplace_back(Name);
                Reader.Advance(PositionAdvance);
            }
        }
    }


    // Next step will be reading the entry table, this differs per-game, but we have it's
    // Size in the header, so it can be semi-generic
    Reader.SetPosition(Header.EntryTableOffset);

    // Everything the handlers below append belongs to THIS bank; remember where they start so we
    // can stamp it on them afterwards rather than threading the path through seven signatures.
    const size_t FirstNewAsset = (CoDAssets::GameAssets != nullptr)
        ? CoDAssets::GameAssets->LoadedAssets.size() : 0;

    // The game whose bank this is reads its sounds (ICoDGame.h); a bank no game claims is not read
    const ICoDGame* Owner = LiveGames::OwnerOf(PackageMark::Container::Sab, Header.Version, Header.SizeOfAudioEntry);
    if (Owner != nullptr)
    {
        CoDAssets::Game = Owner;
        switch (Header.Version)
        {
        case 0x4: HandleSABv4(Reader, Header, SABFileNames, SABNames); break;
        case 0xA: HandleSABv10(Reader, Header, SABFileNames, SABNames); break;
        case 0xE: HandleSABv14(Reader, Header, SABFileNames, SABNames); break;
        case 0xF: HandleSABv15(Reader, Header, SABFileNames, SABNames); break;
        case 0x11: HandleSABv17(Reader, Header, SABFileNames, SABNames); break;
        case 0x15:
            if (Header.SizeOfAudioEntry == 56)
                HandleSABv21v2(Reader, Header, SABFileNames, SABNames, FileSystems::GetFileName(FilePath));
            else
                HandleSABv21(Reader, Header, SABFileNames, SABNames, FileSystems::GetFileName(FilePath));
            break;
        default: break;
        }
    }

    // Stamp the originating bank so LoadSound can reopen the right file later, which is what makes
    // it safe to keep several banks loaded in one session.
    if (CoDAssets::GameAssets != nullptr)
    {
        auto& Loaded = CoDAssets::GameAssets->LoadedAssets;
        for (size_t i = FirstNewAsset; i < Loaded.size(); i++)
            if (auto* Sound = dynamic_cast<CoDSound_t*>(Loaded[i]))
                Sound->BankFile = FilePath;
    }

    SABFileNames.clear();
    SABFileNames.shrink_to_fit();

    return CoDAssets::Game != nullptr;
}

std::unique_ptr<XSound> SABSupport::LoadSound(const CoDSound_t* SoundAsset)
{
    // The entry's own bank wins; the cache path is only the fallback for an entry without BankFile.
    const std::string SoundBankPath = !SoundAsset->BankFile.empty()
        ? SoundAsset->BankFile
        : CoDAssets::GamePackageCache->GetPackagesPath();

    auto Reader = BinaryReader();
    Reader.Open(SoundBankPath);

    HandleSABEncryption(Reader, SoundBankPath);

    Reader.SetPosition(SoundAsset->AssetPointer);

    auto SoundResult = std::make_unique<XSound>();

    auto HeaderSpace = 0;
    if (SoundAsset->DataType == SoundDataTypes::WAV_NeedsHeader)
    {
        HeaderSpace = Sound::GetMaximumWAVHeaderSize();
        SoundResult->DataType = SoundDataTypes::WAV_WithHeader;
    }
    if (SoundAsset->DataType == SoundDataTypes::FLAC_NeedsHeader)
    {
        HeaderSpace = Sound::GetMaximumFLACHeaderSize();
        SoundResult->DataType = SoundDataTypes::FLAC_WithHeader;
    }

    SoundResult->DataBuffer = new int8_t[(uint32_t)(HeaderSpace + SoundAsset->AssetSize)];
    SoundResult->DataSize = (uint32_t)(HeaderSpace + SoundAsset->AssetSize);
    
    uint64_t ReadResult = 0;
    auto AudioBuffer = Reader.Read(SoundAsset->AssetSize, ReadResult);

    std::memcpy((SoundResult->DataBuffer + HeaderSpace), AudioBuffer, ReadResult);

    delete[] AudioBuffer;

    if (SoundAsset->DataType == SoundDataTypes::WAV_NeedsHeader)
    {
        Sound::WriteWAVHeaderToStream(SoundResult->DataBuffer, SoundAsset->FrameRate, SoundAsset->ChannelsCount, (uint32_t)SoundAsset->AssetSize);
    }
    else if (SoundAsset->DataType == SoundDataTypes::FLAC_NeedsHeader)
    {
        Sound::WriteFLACHeaderToStream(SoundResult->DataBuffer, SoundAsset->FrameRate, SoundAsset->ChannelsCount, SoundAsset->FrameCount);
    }

    return SoundResult;
}

std::unique_ptr<XSound> SABSupport::LoadOpusSound(const CoDSound_t* SoundAsset, int32_t PreSkip)
{
    // GetPackagesPath() returns by value: bind a copy, not a reference to the temporary.
    // The entry's own bank wins (several banks parsed in one run, LoadSound's rule)
    const std::string SoundBankPath = !SoundAsset->BankFile.empty() ? SoundAsset->BankFile : CoDAssets::GamePackageCache->GetPackagesPath();

    auto Reader = BinaryReader();
    Reader.Open(SoundBankPath);

    HandleSABEncryption(Reader, SoundBankPath);

    Reader.SetPosition(SoundAsset->AssetPointer);

    std::unique_ptr<uint8_t[]> SoundBuffer = std::make_unique<uint8_t[]>((size_t)SoundAsset->AssetSize);

    uint64_t ReadResult = 0;
    Reader.Read(SoundBuffer.get(), SoundAsset->AssetSize, ReadResult);

    if (ReadResult != (size_t)SoundAsset->AssetSize)
        return nullptr;

    return OpusPackets(SoundBuffer.get(), SoundAsset->AssetSize, 0, SoundAsset->AssetSize, SoundAsset->FrameRate, SoundAsset->ChannelsCount, SoundAsset->FrameCount,
                       PreSkip);
}

std::unique_ptr<XSound> SABSupport::OpusPackets(const uint8_t* Buffer, size_t Available, size_t Offset, size_t Size, uint32_t FrameRate,
                                                uint32_t Channels, uint32_t FrameCount, int32_t PreSkip)
{
    if (Buffer == nullptr || Offset > Available || Size > UINT32_MAX) return nullptr;
    // what the buffer holds from Offset, and zeros after it: a last packet whose size runs past the
    // buffer decodes as it did when the decoder read the caller's buffer, never past an allocation
    constexpr size_t Slack = 0x10002;
    const size_t Held = Available - Offset;
    auto Result = std::make_unique<XSound>();
    Result->DataBuffer = new int8_t[std::max(Held, Size) + Slack]();
    std::memcpy(Result->DataBuffer, Buffer + Offset, Held);
    Result->DataSize = (uint32_t)Size;
    Result->DataType = SoundDataTypes::Opus_Interleaved;
    Result->FrameRate = FrameRate;
    Result->Channels = Channels;
    Result->FrameCount = FrameCount;
    Result->OpusPreSkip = PreSkip;
    return Result;
}

std::unique_ptr<XSound> SABSupport::DecodeOpusInterleaved(const uint8_t* OpusBuffer, size_t OpusBufferSize, size_t OpusDataOffset, uint32_t FrameRate,
                                                          uint32_t Channels, uint32_t FrameCount, int32_t PreSkip)
{
    int ErrorCode = OPUS_OK;
    const std::unique_ptr<OpusDecoder, void (*)(OpusDecoder*)> Decoder(opus_decoder_create((opus_int32)FrameRate, (int)Channels, &ErrorCode),
                                                                       opus_decoder_destroy);
    if (ErrorCode != OPUS_OK || Decoder == nullptr)
        return nullptr;

    // a packet holds at most 120 ms; an empty one (the package's padding) is concealed as 20 ms
    const int MaxFrames = (int)(FrameRate / 1000 * 120), EmptyFrames = (int)(FrameRate / 50);
    std::vector<opus_int16> Block((size_t)MaxFrames * Channels);
    std::vector<opus_int16> Pcm;
    Pcm.reserve(((size_t)FrameCount + 2 * (size_t)EmptyFrames) * Channels);

    for (size_t At = 0; At < OpusBufferSize;)
    {
        uint16_t BlockSize = 0;
        std::memcpy(&BlockSize, OpusBuffer + OpusDataOffset + At, 2);
        At += 2;
        const int Decoded = opus_decode(Decoder.get(), OpusBuffer + OpusDataOffset + At, BlockSize, Block.data(), BlockSize == 0 ? EmptyFrames : MaxFrames, 0);
        // Any negative is a failure in Opus
        if (Decoded < 0)
            return nullptr;
        Pcm.insert(Pcm.end(), Block.begin(), Block.begin() + (size_t)Decoded * Channels);
        At += BlockSize;
    }

    // the sound: FrameCount samples after the pre-skip (the game's, at 48 kHz), what the decode holds
    // of them; an unmeasured pre-skip keeps every packet's samples
    size_t First = 0, Frames = Pcm.size() / Channels;
    if (PreSkip >= 0 && FrameCount > 0)
    {
        First = std::min<size_t>((size_t)PreSkip * FrameRate / 48000, Frames);
        Frames = std::min<size_t>(FrameCount, Frames - First);
    }
    const size_t PCMDataSize = Frames * Channels * sizeof(opus_int16);

    auto Result = std::make_unique<XSound>();
    Result->DataBuffer = new int8_t[PCMDataSize + (size_t)Sound::GetMaximumWAVHeaderSize()];
    Result->DataType = SoundDataTypes::WAV_WithHeader;
    Result->DataSize = (uint32_t)(PCMDataSize + Sound::GetMaximumWAVHeaderSize());
    Sound::WriteWAVHeaderToStream(Result->DataBuffer, FrameRate, Channels, (uint32_t)PCMDataSize);
    if (PCMDataSize != 0)
        std::memcpy(Result->DataBuffer + Sound::GetMaximumWAVHeaderSize(), Pcm.data() + First * Channels, PCMDataSize);
    return Result;
}


namespace
{
    // An entry's name: the bank's name list by position, else the name index by key, else its key
    std::string EntryName(uint32_t Index, uint64_t Key, const std::vector<std::string>& NameList, const WraithNameIndex& NameIndex)
    {
        if (NameList.size() > 0)
            return NameList[Index];
        if (NameIndex.NameDatabase.find(Key) != NameIndex.NameDatabase.end())
            return NameIndex.NameDatabase.at(Key);
        return std::format("_{:x}", Key);
    }

    // An entry's name among names keyed by their hash (the bank lists them out of entry order)
    template <typename TKey>
    std::string HashedEntryName(TKey Key, const std::unordered_map<TKey, std::string>& Names)
    {
        const auto Found = Names.find(Key);
        return Found != Names.end() ? Found->second : std::format("_{:x}", (uint64_t)Key);
    }

    // A sound named by its path in the bank
    CoDSound_t* NamedSound(const std::string& Name)
    {
        auto Sound = new CoDSound_t();
        Sound->AssetName = FileSystems::GetFileNamePurgeExtensions(Name);
        Sound->FullPath = FileSystems::GetDirectoryName(Name);
        return Sound;
    }

    // The sample rate a v14+ entry's rate index names (left unset for an unknown index)
    void SetFrameRate(CoDSound_t* Sound, uint8_t RateIndex)
    {
        constexpr uint32_t Rates[] = { 8000, 12000, 16000, 24000, 32000, 44100, 48000, 96000, 192000 };
        if (RateIndex < std::size(Rates))
            Sound->FrameRate = Rates[RateIndex];
    }

    // The samples' place in the bank and their format (FrameRate already set)
    void SetSamples(CoDSound_t* Sound, uint32_t FrameCount, uint8_t Channels, uint64_t Offset, uint32_t Size, SoundDataTypes DataType)
    {
        Sound->FrameCount = FrameCount;
        Sound->ChannelsCount = Channels;
        Sound->AssetPointer = Offset;
        Sound->AssetSize = Size;
        Sound->AssetStatus = WraithAssetStatus::Loaded;
        Sound->IsFileEntry = true;
        Sound->Length = (uint32_t)(1000.0f * (float)(Sound->FrameCount / (float)(Sound->FrameRate)));
        Sound->DataType = DataType;
    }

    // Adds the sound to the loaded assets, unless it has no samples and "skipblankaudio" is on
    void AddSound(CoDSound_t* Sound, bool SkipBlankAudio)
    {
        if (SkipBlankAudio && Sound->AssetSize <= 0)
        {
            delete Sound;
            return;
        }
        CoDAssets::GameAssets->LoadedAssets.push_back(Sound);
    }

    bool SkipBlankAudio()
    {
        return SettingsManager::GetSetting("skipblankaudio", "false") == "true";
    }

    template <typename TKey>
    std::unordered_map<TKey, std::string> NamesByHash(const std::vector<std::string>& NameList, TKey (*Hash)(const std::string&))
    {
        std::unordered_map<TKey, std::string> Names;
        for (auto& Name : NameList)
            Names[Hash(Name)] = Name;
        return Names;
    }
}

void SABSupport::HandleSABv4(BinaryReader& Reader, const SABFileHeader& Header, const std::vector<std::string>& NameList, const WraithNameIndex& NameIndex)
{
    const auto SkipBlank = SkipBlankAudio();
    for (uint32_t i = 0; i < Header.EntriesCount; i++)
    {
        auto Entry = Reader.Read<SABv4Entry>();
        auto LoadedSound = NamedSound(EntryName(i, Entry.Key, NameList, NameIndex));
        LoadedSound->FrameRate = Entry.FrameRate;
        // After the seek table, which is not needed; every Infinite Warfare (v4) entry is a FLAC without its header
        SetSamples(LoadedSound, Entry.FrameCount, Entry.ChannelCount, Entry.Offset + Entry.SeekTableLength, Entry.Size, SoundDataTypes::FLAC_NeedsHeader);
        AddSound(LoadedSound, SkipBlank);
    }
}

void SABSupport::HandleSABv10(BinaryReader& Reader, const SABFileHeader& Header, const std::vector<std::string>& NameList, const WraithNameIndex& NameIndex)
{
    const auto SkipBlank = SkipBlankAudio();
    const auto SoundNames = NamesByHash<uint32_t>(NameList, HashSoundString);
    for (uint32_t i = 0; i < Header.EntriesCount; i++)
    {
        auto Entry = Reader.Read<SABv4Entry>();
        auto LoadedSound = NamedSound(HashedEntryName(Entry.Key, SoundNames));
        LoadedSound->BankKey = Entry.Key;
        LoadedSound->FrameRate = Entry.FrameRate;
        SetSamples(LoadedSound, Entry.FrameCount, Entry.ChannelCount, Entry.Offset + Entry.SeekTableLength, Entry.Size, SoundDataTypes::FLAC_NeedsHeader);
        AddSound(LoadedSound, SkipBlank);
    }
}

void SABSupport::HandleSABv14(BinaryReader& Reader, const SABFileHeader& Header, const std::vector<std::string>& NameList, const WraithNameIndex& NameIndex)
{
    const auto SkipBlank = SkipBlankAudio();
    for (uint32_t i = 0; i < Header.EntriesCount; i++)
    {
        auto Entry = Reader.Read<SABv14Entry>();
        auto LoadedSound = NamedSound(EntryName(i, Entry.Key, NameList, NameIndex));
        SetFrameRate(LoadedSound, Entry.FrameRateIndex);
        // Format 0: PCM without its header, 8: FLAC with it
        const auto DataType = Entry.Format == 0 ? SoundDataTypes::WAV_NeedsHeader : Entry.Format == 8 ? SoundDataTypes::FLAC_WithHeader : LoadedSound->DataType;
        SetSamples(LoadedSound, Entry.FrameCount, Entry.ChannelCount, Entry.Offset, Entry.Size, DataType);
        AddSound(LoadedSound, SkipBlank);
    }
}

void SABSupport::HandleSABv15(BinaryReader& Reader, const SABFileHeader& Header, const std::vector<std::string>& NameList, const WraithNameIndex& NameIndex)
{
    const auto SkipBlank = SkipBlankAudio();
    for (uint32_t i = 0; i < Header.EntriesCount; i++)
    {
        auto Entry = Reader.Read<SABv15Entry>();
        auto LoadedSound = NamedSound(EntryName(i, Entry.Key, NameList, NameIndex));
        SetFrameRate(LoadedSound, Entry.FrameRateIndex);
        // Every Black Ops 3 (v15) entry is a FLAC with its header
        SetSamples(LoadedSound, Entry.FrameCount, Entry.ChannelCount, Entry.Offset, Entry.Size, SoundDataTypes::FLAC_WithHeader);
        AddSound(LoadedSound, SkipBlank);
    }
}

void SABSupport::HandleSABv17(BinaryReader& Reader, const SABFileHeader& Header, const std::vector<std::string>& NameList, const WraithNameIndex& NameIndex)
{
    const auto SkipBlank = SkipBlankAudio();
    const auto SoundNames = NamesByHash<uint64_t>(NameList, HashSoundStringV17);
    for (uint32_t i = 0; i < Header.EntriesCount; i++)
    {
        auto Entry = Reader.Read<SABv17Entry>();
        auto LoadedSound = NamedSound(HashedEntryName(Entry.Key, SoundNames));
        LoadedSound->FrameRate = Entry.FrameRate;
        // After the primed part and the seek table; every Vanguard (v17) entry is converted to WAV
        SetSamples(LoadedSound, Entry.FrameCount, Entry.ChannelCount, Entry.PrimedSize + Entry.Offset + Entry.SeekTableLength, Entry.Size,
                   SoundDataTypes::WAV_NeedsHeader);
        AddSound(LoadedSound, SkipBlank);
    }
}

void SABSupport::HandleSABv21(BinaryReader& Reader, const SABFileHeader& Header, const std::vector<std::string>& NameList, const WraithNameIndex& NameIndex, const std::string FileName)
{
    const auto SkipBlank = SkipBlankAudio();
    auto UseIndex = SettingsManager::GetSetting("usesabindexbo4", "false") == "true";

    for (uint32_t i = 0; i < Header.EntriesCount; i++)
    {
        auto Entry = Reader.Read<SABv21Entry>();
        auto LoadedSound = new CoDSound_t();

        const auto Name = EntryName(i, Entry.Key, NameList, NameIndex);
        if (NameList.size() > 0 && Name.starts_with("xsound_") && UseIndex)
        {
            LoadedSound->AssetName = std::format("{}.{}", FileName.c_str(), i);
            LoadedSound->FullPath = "";
        }
        else
        {
            // a name from the bank's list is a path; one from the index or the key is kept whole
            LoadedSound->AssetName = NameList.size() > 0 ? FileSystems::GetFileNamePurgeExtensions(Name) : Name;
            LoadedSound->FullPath = FileSystems::GetDirectoryName(Name);
        }

        SetFrameRate(LoadedSound, Entry.FrameRateIndex);
        // Every Black Ops 4 (v21) entry is a FLAC with its header
        SetSamples(LoadedSound, Entry.FrameCount, Entry.ChannelCount, Entry.Offset, Entry.Size, SoundDataTypes::FLAC_WithHeader);
        AddSound(LoadedSound, SkipBlank);
    }
}

void SABSupport::HandleSABv21v2(BinaryReader& Reader, const SABFileHeader& Header, const std::vector<std::string>& NameList, const WraithNameIndex& NameIndex, const std::string FileName)
{
    const auto SkipBlank = SkipBlankAudio();
    const auto SoundNames = NamesByHash<uint64_t>(NameList, HashSoundStringV17);

    // The entry table's offset is at 36
    Reader.SetPosition(36);
    Reader.SetPosition(Reader.Read<uint64_t>());

    for (uint32_t i = 0; i < Header.EntriesCount; i++)
    {
        auto Entry = Reader.Read<SABv17Entry>();
        auto LoadedSound = NamedSound(HashedEntryName(Entry.Key, SoundNames));
        LoadedSound->FrameRate = Entry.FrameRate;
        SetSamples(LoadedSound, Entry.FrameCount, Entry.ChannelCount, Entry.PrimedSize + Entry.Offset + Entry.SeekTableLength, Entry.Size,
                   SoundDataTypes::WAV_NeedsHeader);
        AddSound(LoadedSound, SkipBlank);
    }
}

void SABSupport::HandleSABEncryption(BinaryReader& Reader, const std::string& FilePath)
{
    // Currently, the only encrypted SAB files are in Black Ops 3
    // They are identified using zm_genesis and use the same key
    // The encryption is a SALSA20, the key is taken from a Base64 string
    // - Key 1: kZLU3vLqN1jnSgDBFiEmRguT0O1u7dxk2CGrqRReL8lKG7ECAAAAAA==

    if (FileSystems::GetFileName(FilePath).find("zm_genesis") != std::string::npos)
    {
        namespace fs = std::filesystem;
        std::error_code Ec;
        // The decrypted copy, kept in %TEMP%\greyhound_sab (never beside the bank, in the game's
        // folder): named after the bank, its path, size and time, so a changed bank is decrypted anew
        const fs::path Dir = fs::temp_directory_path(Ec) / "greyhound_sab";
        fs::create_directories(Dir, Ec);
        const auto Size = fs::file_size(FilePath, Ec);
        const auto Time = fs::last_write_time(FilePath, Ec).time_since_epoch().count();
        const std::string Decrypted =
            (Dir / std::format("{}_{}.wraithdec", fs::path(FilePath).filename().string(),
                               ExportNaming::Hex16(ExportNaming::Fnv1a(ExportNaming::Lower(fs::absolute(FilePath, Ec).string()) + "\n" +
                                                                       std::to_string(Size) + "\n" + std::to_string(Time)))))
                .string();
        // We must swap out for a decrypted reader
        if (!fs::is_regular_file(Decrypted, Ec))
        {
            // Key 1: 32-byte key, 8-byte IV
            uint8_t KeyData[32] = { 0x91, 0x92, 0xD4, 0xDE, 0xF2, 0xEA, 0x37, 0x58, 0xE7, 0x4A, 0x0, 0xC1, 0x16, 0x21, 0x26, 0x46, 0xB, 0x93, 0xD0, 0xED, 0x6E, 0xED, 0xDC, 0x64, 0xD8, 0x21, 0xAB, 0xA9, 0x14, 0x5E, 0x2F, 0xC9 };
            uint8_t IVData[8] = { 0x4A, 0x1B, 0xB1, 0x2, 0x0, 0x0, 0x0, 0x0 };

            uint64_t ResultSize = 0;
            auto FileBuffer = Reader.Read(Reader.GetLength(), ResultSize);
            Reader.Close();

            Salsa20Key Key;
            Key.Key = &KeyData[0];
            Key.IV = &IVData[0];
            Key.KeyLength = 256;
            Encryption::Salsa20Block(FileBuffer, (uint32_t)ResultSize, Key);

            // written aside then renamed: a reader never meets a half-written copy
            const std::string Temp = Decrypted + std::format(".{}.tmp", GetCurrentProcessId());
            auto Writer = BinaryWriter();

            Writer.Create(Temp);
            Writer.Write(FileBuffer, (uint32_t)ResultSize);
            Writer.Close();

            delete[] FileBuffer;
            fs::rename(Temp, Decrypted, Ec);
            if (Ec) fs::remove(Temp, Ec);
        }

        Reader.Open(Decrypted);
    }
}