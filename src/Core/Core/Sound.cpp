#include "stdafx.h"

#include "Sound.h"

#include "MemoryReader.h"

#include <functional>

// We don't need the FLAC DLL exports, we're a LIB
#define FLAC__NO_DLL 1

#include "FLAC\stream_decoder.h"
#include "FLAC\stream_encoder.h"

#include "opus.h"
#include "opus_multistream.h"
#include "ogg\ogg.h"

struct FLACStreamInfo
{
    int8_t* InputBuffer;
    uint64_t InputBufferSize;
    uint64_t CurrentPosition;

    uint64_t TotalSamples;
    uint32_t SampleRate;
    uint32_t ChannelsCount;
    uint32_t Bps;

    // The WAV file's bytes (header and samples), built whole and written with one call: a write
    // per frame went through the CRT's 4 KB buffer, a WriteFile every 4 KB
    std::vector<uint8_t>* ByteSink = nullptr;

    // When set, decoded samples are collected here instead of written as WAV, which is
    // what the Opus path needs since it has to encode rather than copy
    std::vector<int16_t>* SampleSink = nullptr;
};

struct WAVStreamInfo
{
    uint64_t TotalSamples;
    uint32_t SeekLength;
    uint32_t RawDataSize;
    uint32_t FormatSize;
    uint32_t SampleRate;
    uint16_t ChannelsCount;
    uint16_t Bps;
};

namespace
{
    template <typename T>
    void AppendRaw(std::vector<uint8_t>& Out, const T& Value)
    {
        const auto* Bytes = reinterpret_cast<const uint8_t*>(&Value);
        Out.insert(Out.end(), Bytes, Bytes + sizeof(T));
    }

    // A whole file written with one call (the CRT writes a block that large straight through)
    bool WriteWholeFile(const std::string& Path, const std::vector<uint8_t>& Bytes)
    {
        FILE* Output = nullptr;
        fopen_s(&Output, Path.c_str(), "wb");
        if (Output == nullptr) return false;
        const bool Wrote = Bytes.empty() || fwrite(Bytes.data(), 1, Bytes.size(), Output) == Bytes.size();
        return (fclose(Output) == 0) && Wrote;
    }
}

#pragma region FLACDecoder

FLAC__StreamDecoderSeekStatus FLACSeekStream(const FLAC__StreamDecoder *decoder, FLAC__uint64 absolute_byte_offset, void *client_data)
{
    FLACStreamInfo* StreamData = (FLACStreamInfo*)client_data;

    if (absolute_byte_offset <= StreamData->InputBufferSize)
    {
        StreamData->CurrentPosition = absolute_byte_offset;
        return FLAC__STREAM_DECODER_SEEK_STATUS_OK;
    }
    else
    {
        return FLAC__STREAM_DECODER_SEEK_STATUS_ERROR;
    }
}

FLAC__bool FLACEOFStream(const FLAC__StreamDecoder *decoder, void *client_data)
{
    FLACStreamInfo* StreamData = (FLACStreamInfo*)client_data;
    if (StreamData->CurrentPosition >= StreamData->InputBufferSize)
    {
        return true;
    }
    return false;
}

FLAC__StreamDecoderLengthStatus FLACLengthStream(const FLAC__StreamDecoder *decoder, FLAC__uint64 *stream_length, void *client_data)
{
    FLACStreamInfo* StreamData = (FLACStreamInfo*)client_data;
    *stream_length = (FLAC__uint64)StreamData->InputBufferSize;
    return FLAC__STREAM_DECODER_LENGTH_STATUS_OK;
}

FLAC__StreamDecoderTellStatus FLACTellStream(const FLAC__StreamDecoder *decoder, FLAC__uint64 *absolute_byte_offset, void *client_data)
{
    FLACStreamInfo* StreamData = (FLACStreamInfo*)client_data;
    *absolute_byte_offset = (FLAC__uint64)StreamData->CurrentPosition;
    return FLAC__STREAM_DECODER_TELL_STATUS_OK;
}

FLAC__StreamDecoderReadStatus FLACReadFromStream(const FLAC__StreamDecoder *decoder, FLAC__byte buffer[], size_t *bytes, void *client_data)
{
    FLACStreamInfo* StreamData = (FLACStreamInfo*)client_data;

    if (*bytes > 0)
    {
        auto DataRequested = *bytes;

        auto DataBufferRemainder = (int64_t)((int64_t)StreamData->InputBufferSize - (int64_t)StreamData->CurrentPosition);

        if ((StreamData->CurrentPosition + DataRequested) <= StreamData->InputBufferSize)
        {
            std::memcpy(buffer, StreamData->InputBuffer + StreamData->CurrentPosition, DataRequested);
            StreamData->CurrentPosition += DataRequested;
            return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
        }
        else if (DataBufferRemainder > 0)
        {
            *bytes = (FLAC__uint64)DataBufferRemainder;
            std::memcpy(buffer, StreamData->InputBuffer + StreamData->CurrentPosition, DataBufferRemainder);
            StreamData->CurrentPosition += DataBufferRemainder;
            return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
        }
        else
        {
            *bytes = 0;
            return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
        }
    }
    else
    {
        return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
    }
}

void FLACMetaDataCallback(const FLAC__StreamDecoder *decoder, const FLAC__StreamMetadata *metadata, void *client_data)
{
    (void)decoder, (void)client_data;

    FLACStreamInfo* StreamData = (FLACStreamInfo*)client_data;

    if (metadata->type == FLAC__METADATA_TYPE_STREAMINFO)
    {
        StreamData->TotalSamples = metadata->data.stream_info.total_samples;
        StreamData->SampleRate = metadata->data.stream_info.sample_rate;
        StreamData->ChannelsCount = metadata->data.stream_info.channels;
        StreamData->Bps = metadata->data.stream_info.bits_per_sample;

        // the whole stream's room at once (a header that claims more than 1 GB is not trusted)
        const uint64_t Values = StreamData->TotalSamples * StreamData->ChannelsCount;
        if (Values > 0 && Values < (1ull << 29))
        {
            if (StreamData->SampleSink != nullptr) StreamData->SampleSink->reserve((size_t)Values);
            if (StreamData->ByteSink != nullptr) StreamData->ByteSink->reserve((size_t)Values * 2 + 44);
        }
    }
}

void FLACErrorCallback(const FLAC__StreamDecoder *decoder, FLAC__StreamDecoderErrorStatus status, void *client_data)
{
    (void)decoder, (void)client_data, (void)status;
}

FLAC__StreamDecoderWriteStatus FLACWriteCallback(const FLAC__StreamDecoder *decoder, const FLAC__Frame *frame, const FLAC__int32 * const buffer[], void *client_data)
{
    (void)decoder;

    FLACStreamInfo* StreamData = (FLACStreamInfo*)client_data;
    const uint32_t TotalSize = (uint32_t)(StreamData->TotalSamples * StreamData->ChannelsCount * (StreamData->Bps / 8));

    if (StreamData->TotalSamples == 0)
    {
        return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    }

    // Collecting rather than writing, the Opus path encodes the whole stream at the end
    if (StreamData->SampleSink != nullptr)
    {
        const size_t SinkChannels = StreamData->ChannelsCount;
        const size_t SinkSamples = frame->header.blocksize;
        const size_t SinkOffset = StreamData->SampleSink->size();

        StreamData->SampleSink->resize(SinkOffset + (SinkSamples * SinkChannels));

        for (size_t i = 0; i < SinkSamples; i++)
        {
            for (size_t c = 0; c < SinkChannels; c++)
            {
                (*StreamData->SampleSink)[SinkOffset + (i * SinkChannels) + c] = (int16_t)buffer[c][i];
            }
        }

        return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
    }

    std::vector<uint8_t>& Out = *StreamData->ByteSink;

    if (frame->header.number.sample_number == 0)
    {
        const uint32_t SizeWithHeader = TotalSize + 36;

        Out.insert(Out.end(), { 'R', 'I', 'F', 'F' });
        AppendRaw(Out, SizeWithHeader);
        Out.insert(Out.end(), { 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ' });

        AppendRaw(Out, (uint32_t)16);
        AppendRaw(Out, (uint16_t)1);
        AppendRaw(Out, (uint16_t)StreamData->ChannelsCount);
        AppendRaw(Out, (uint32_t)StreamData->SampleRate);
        AppendRaw(Out, (uint32_t)(StreamData->SampleRate * StreamData->ChannelsCount * (StreamData->Bps / 8)));
        AppendRaw(Out, (uint16_t)(StreamData->ChannelsCount * (StreamData->Bps / 8)));
        AppendRaw(Out, (uint16_t)StreamData->Bps);

        Out.insert(Out.end(), { 'd', 'a', 't', 'a' });
        AppendRaw(Out, TotalSize);
    }

    // The block interleaved straight into the file's bytes, 16-bit little endian
    const size_t Channels = StreamData->ChannelsCount;
    const size_t Samples = frame->header.blocksize;
    const size_t At = Out.size();

    Out.resize(At + (Samples * Channels * sizeof(int16_t)));
    auto* Dest = reinterpret_cast<int16_t*>(Out.data() + At);

    if (Channels == 2)
    {
        const FLAC__int32* Left = buffer[0];
        const FLAC__int32* Right = buffer[1];
        for (size_t i = 0; i < Samples; i++)
        {
            Dest[2 * i] = (int16_t)Left[i];
            Dest[(2 * i) + 1] = (int16_t)Right[i];
        }
    }
    else
    {
        for (size_t c = 0; c < Channels; c++)
        {
            const FLAC__int32* Channel = buffer[c];
            for (size_t i = 0; i < Samples; i++)
            {
                Dest[(i * Channels) + c] = (int16_t)Channel[i];
            }
        }
    }

    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

#pragma endregion

bool Sound::ConvertSoundMemory(int8_t* SoundBuffer, uint64_t SoundSize, SoundFormat InFormat, const std::string& OutputFile, SoundFormat OutFormat)
{
    auto ConversionResult = false;

    switch (InFormat)
    {
    case SoundFormat::FLAC_WithHeader:
        if (OutFormat == SoundFormat::Standard_WAV)
        {
            ConversionResult = TranscodeFLACToWav(SoundBuffer, SoundSize, OutputFile);
        }
        else if (OutFormat == SoundFormat::Standard_OPUS)
        {
            ConversionResult = TranscodeFLACToOpus(SoundBuffer, SoundSize, OutputFile);
        }
        break;
    case SoundFormat::WAV_WithHeader:
        if (OutFormat == SoundFormat::Standard_FLAC)
        {
            ConversionResult = TranscodeWAVToFlac(SoundBuffer, SoundSize, OutputFile);
        }
        else if (OutFormat == SoundFormat::Standard_OPUS)
        {
            ConversionResult = TranscodeWAVToOpus(SoundBuffer, SoundSize, OutputFile);
        }
        break;
    }

    return ConversionResult;
}

bool Sound::DecodeFLACToWav(int8_t* SoundBuffer, uint64_t SoundSize, std::vector<uint8_t>& Wav)
{
    Wav.clear();

    auto FlacInfo = std::make_unique<FLACStreamInfo>();
    FlacInfo->InputBuffer = SoundBuffer;
    FlacInfo->InputBufferSize = SoundSize;
    FlacInfo->CurrentPosition = 0;
    FlacInfo->ByteSink = &Wav;

    FLAC__StreamDecoder* Decoder = FLAC__stream_decoder_new();

    if (Decoder == nullptr)
    {
        return false;
    }

    FLAC__stream_decoder_set_md5_checking(Decoder, false);

    const auto InitStatus = FLAC__stream_decoder_init_stream(Decoder, FLACReadFromStream, FLACSeekStream, FLACTellStream, FLACLengthStream, FLACEOFStream, FLACWriteCallback, FLACMetaDataCallback, FLACErrorCallback, FlacInfo.get());

    const bool IsOk = (InitStatus == FLAC__STREAM_DECODER_INIT_STATUS_OK);

    if (IsOk)
    {
        FLAC__stream_decoder_process_until_end_of_stream(Decoder);
    }

    FLAC__stream_decoder_delete(Decoder);

    return IsOk;
}

bool Sound::TranscodeFLACToWav(int8_t* SoundBuffer, uint64_t SoundSize, const std::string& OutputFile)
{
    // what the decoder gave, even nothing (a stream it does not read is an empty file, as before)
    std::vector<uint8_t> Wav;
    const bool IsOk = DecodeFLACToWav(SoundBuffer, SoundSize, Wav);

    return WriteWholeFile(OutputFile, Wav) && IsOk;
}

bool Sound::TranscodeWAVToFlac(int8_t* SoundBuffer, uint64_t SoundSize, const std::string& OutputFile)
{
    bool IsOk = true;
    FLAC__StreamEncoder* Encoder = nullptr;
    FLAC__StreamEncoderInitStatus InitStatus;

    auto WAVInfo = std::make_unique<WAVStreamInfo>();
    WAVInfo->TotalSamples = 0;
    WAVInfo->SeekLength = 0;

    auto WAVInfoReader = MemoryReader(SoundBuffer, SoundSize, true);
    bool KeepReading = true;
    
    while (KeepReading)
    {
        uint32_t ChunkID = WAVInfoReader.Read<uint32_t>();

        switch (ChunkID)
        {
            // RIFF Chunk, skip total size
        case 0x46464952: WAVInfoReader.Advance(4); break;

            // WAVEFmt Chunk, read info
        case 0x45564157:
        {
            WAVInfoReader.Advance(4);

            WAVInfo->FormatSize = WAVInfoReader.Read<uint32_t>();

            auto CurrentPosition = WAVInfoReader.GetPosition();
            WAVInfoReader.Advance(2);

            WAVInfo->ChannelsCount = WAVInfoReader.Read<uint16_t>();
            WAVInfo->SampleRate = WAVInfoReader.Read<uint32_t>();
            WAVInfoReader.Advance(6);
            WAVInfo->Bps = WAVInfoReader.Read<uint16_t>();

            WAVInfoReader.SetPosition(CurrentPosition + WAVInfo->FormatSize);
        }
        break;

            // Data Chunk, read size and end, we're at the data
        case 0x61746164: WAVInfo->RawDataSize = WAVInfoReader.Read<uint32_t>(); KeepReading = false; break;
            // DPDS Chunk, read length
        case 0x73647064: WAVInfo->SeekLength = WAVInfoReader.Read<uint32_t>(); WAVInfoReader.Advance(WAVInfo->SeekLength); break;

        default: KeepReading = false; break;
        }
    }

    if (((WAVInfo->Bps / 8) * WAVInfo->ChannelsCount) > 0)
    {
        WAVInfo->TotalSamples = (WAVInfo->RawDataSize / ((WAVInfo->Bps / 8) * WAVInfo->ChannelsCount));
    }

    Encoder = FLAC__stream_encoder_new();

    if (Encoder == nullptr)
    {
        return false;
    }

    FLAC__stream_encoder_set_verify(Encoder, false);
    FLAC__stream_encoder_set_compression_level(Encoder, 5);
    FLAC__stream_encoder_set_channels(Encoder, WAVInfo->ChannelsCount);
    FLAC__stream_encoder_set_bits_per_sample(Encoder, WAVInfo->Bps);
    FLAC__stream_encoder_set_sample_rate(Encoder, WAVInfo->SampleRate);

    InitStatus = FLAC__stream_encoder_init_file(Encoder, OutputFile.c_str(), nullptr, nullptr);

    if (InitStatus != FLAC__STREAM_ENCODER_INIT_STATUS_OK) { IsOk = false; }

    if (IsOk)
    {
        size_t Left = (size_t)WAVInfo->TotalSamples;
        auto Buffer = new FLAC__byte[1024 * ((WAVInfo->Bps / 8) * WAVInfo->ChannelsCount)];
        auto PCMBuffer = new FLAC__int32[1024 * 2];

        while (IsOk && Left)
        {
            size_t Need = (Left > 1024 ? (size_t)1024 : (size_t)Left);

            auto SizeToRead = (WAVInfo->ChannelsCount * (WAVInfo->Bps / 8)) * Need;

            if (WAVInfoReader.GetPosition() + SizeToRead <= WAVInfoReader.GetLength())
            {
                WAVInfoReader.Read(SizeToRead, (int8_t*)Buffer);

                for (size_t i = 0; i < (Need * WAVInfo->ChannelsCount); i++)
                {
                    PCMBuffer[i] = (FLAC__int32)(((FLAC__int16)(FLAC__int8)Buffer[2 * i + 1] << 8) | (FLAC__int16)Buffer[2 * i]);
                }

                IsOk = (FLAC__stream_encoder_process_interleaved(Encoder, PCMBuffer, (uint32_t)Need) == 1);
            }
            else
            {
                IsOk = false;
            }

            Left -= Need;
        }

        delete[] Buffer;
        delete[] PCMBuffer;
    }

    FLAC__stream_encoder_finish(Encoder);
    FLAC__stream_encoder_delete(Encoder);

    return IsOk;
}

namespace
{
    // Appends a little endian value to a byte vector, Ogg Opus headers are all little endian
    template <typename T>
    void AppendLE(std::vector<uint8_t>& Buffer, T Value)
    {
        for (size_t i = 0; i < sizeof(T); i++)
        {
            Buffer.push_back((uint8_t)((Value >> (i * 8)) & 0xFF));
        }
    }

    // Appends whatever pages libogg has ready for us to the file's bytes
    bool FlushOggPages(ogg_stream_state& Stream, std::vector<uint8_t>& Output, bool ForcePageBreak)
    {
        ogg_page Page;

        while (ForcePageBreak ? (ogg_stream_flush(&Stream, &Page) != 0)
                              : (ogg_stream_pageout(&Stream, &Page) != 0))
        {
            Output.insert(Output.end(), Page.header, Page.header + Page.header_len);
            Output.insert(Output.end(), Page.body, Page.body + Page.body_len);
        }

        return true;
    }
}

namespace
{
    // An Ogg stream's serial: unique within its file only, so taken from the file's name (not its
    // folder: the same export in another folder gives the same bytes)
    int OggSerialOf(const std::string& OutputFile)
    {
        const size_t Slash = OutputFile.find_last_of("/\\");
        int Serial = 0;
        for (const auto Character : OutputFile.substr(Slash == std::string::npos ? 0 : Slash + 1))
        {
            Serial = (Serial * 131) + (unsigned char)Character;
        }
        return Serial;
    }
}

bool Sound::EncodePCMToOpus(const std::vector<int16_t>& Samples, uint32_t ChannelsCount, uint32_t SampleRate, const std::string& OutputFile)
{
    // Opus only accepts these input rates. Anything else would need resampling, which we
    // deliberately do not do: silently changing the rate would be worse than refusing.
    if (SampleRate != 8000 && SampleRate != 12000 && SampleRate != 16000 && SampleRate != 24000 && SampleRate != 48000)
    {
        return false;
    }

    // mono and stereo without a channel mapping table, 3 to 8 channels with family 1's (surround)
    if (ChannelsCount < 1 || ChannelsCount > 8)
    {
        return false;
    }

    if (Samples.empty())
    {
        return false;
    }

    int EncoderError = OPUS_OK;
    OpusEncoder* Encoder = nullptr;
    OpusMSEncoder* Surround = nullptr;
    int Streams = 1, Coupled = 0;
    unsigned char Mapping[8] = {};
    if (ChannelsCount <= 2)
        Encoder = opus_encoder_create((opus_int32)SampleRate, (int)ChannelsCount, OPUS_APPLICATION_AUDIO, &EncoderError);
    else
        Surround = opus_multistream_surround_encoder_create((opus_int32)SampleRate, (int)ChannelsCount, 1, &Streams, &Coupled, Mapping,
                                                            OPUS_APPLICATION_AUDIO, &EncoderError);
    const auto Destroy = [&]() {
        if (Encoder != nullptr) opus_encoder_destroy(Encoder);
        if (Surround != nullptr) opus_multistream_encoder_destroy(Surround);
    };

    if ((Encoder == nullptr && Surround == nullptr) || EncoderError != OPUS_OK)
    {
        Destroy();
        return false;
    }

    // The decoder has to drop this many samples to line the output back up (the encoder's
    // lookahead, at the input rate; the pre-skip counts it at 48 kHz, as the granule positions do)
    opus_int32 Lookahead = 0;
    if (Encoder != nullptr) opus_encoder_ctl(Encoder, OPUS_GET_LOOKAHEAD(&Lookahead));
    else opus_multistream_encoder_ctl(Surround, OPUS_GET_LOOKAHEAD(&Lookahead));
    const opus_int32 PreSkip = (opus_int32)((int64_t)Lookahead * 48000 / (int64_t)SampleRate);

    // 64 kb/s a channel (128 for stereo): game sounds are short and noisy, and the encoder's
    // automatic rate smears transients; a surround stream keeps the encoder's own allocation
    if (Encoder != nullptr) opus_encoder_ctl(Encoder, OPUS_SET_BITRATE((opus_int32)(64000 * ChannelsCount)));

    // the file's bytes, written with one call at the end
    std::vector<uint8_t> Output;
    Output.reserve(4096 + (Samples.size() / ChannelsCount / ((size_t)SampleRate / 50)) * 170 * (size_t)ChannelsCount);

    // The serial only has to be unique within a physical stream, and we write exactly one,
    // so derive it from the name to keep runs reproducible rather than using a random value
    const int OggSerial = OggSerialOf(OutputFile);

    ogg_stream_state Stream;
    if (ogg_stream_init(&Stream, OggSerial) != 0)
    {
        WriteWholeFile(OutputFile, Output);
        Destroy();
        return false;
    }

    bool IsOk = true;
    int64_t PacketIndex = 0;

    // Identification header, see RFC 7845
    {
        std::vector<uint8_t> Head;
        Head.insert(Head.end(), { 'O', 'p', 'u', 's', 'H', 'e', 'a', 'd' });
        Head.push_back(1);                              // version
        Head.push_back((uint8_t)ChannelsCount);
        AppendLE<uint16_t>(Head, (uint16_t)PreSkip);
        AppendLE<uint32_t>(Head, SampleRate);           // original rate, informational
        AppendLE<uint16_t>(Head, 0);                    // output gain
        Head.push_back(Surround != nullptr ? 1 : 0);    // channel mapping family
        if (Surround != nullptr)
        {
            Head.push_back((uint8_t)Streams);
            Head.push_back((uint8_t)Coupled);
            Head.insert(Head.end(), Mapping, Mapping + ChannelsCount);
        }

        ogg_packet Packet = {};
        Packet.packet = Head.data();
        Packet.bytes = (long)Head.size();
        Packet.b_o_s = 1;
        Packet.granulepos = 0;
        Packet.packetno = PacketIndex++;

        ogg_stream_packetin(&Stream, &Packet);
        IsOk = FlushOggPages(Stream, Output, true);
    }

    // Comment header, mandatory even when empty
    if (IsOk)
    {
        const std::string Vendor = "Greyhound";

        std::vector<uint8_t> Tags;
        Tags.insert(Tags.end(), { 'O', 'p', 'u', 's', 'T', 'a', 'g', 's' });
        AppendLE<uint32_t>(Tags, (uint32_t)Vendor.size());
        Tags.insert(Tags.end(), Vendor.begin(), Vendor.end());
        AppendLE<uint32_t>(Tags, 0);                    // user comment count

        ogg_packet Packet = {};
        Packet.packet = Tags.data();
        Packet.bytes = (long)Tags.size();
        Packet.granulepos = 0;
        Packet.packetno = PacketIndex++;

        ogg_stream_packetin(&Stream, &Packet);
        IsOk = FlushOggPages(Stream, Output, true);
    }

    // 20ms frames, the usual trade off between overhead and latency
    const size_t FrameSamples = SampleRate / 50;
    const size_t FrameValues = FrameSamples * ChannelsCount;
    // enough frames for the lookahead too: the decoder drops the pre-skip and must still give
    // every sample
    const size_t TotalFrames = (Samples.size() + (size_t)Lookahead * ChannelsCount + FrameValues - 1) / FrameValues;

    std::vector<int16_t> FrameBuffer(FrameValues);
    std::vector<uint8_t> PacketBuffer(4000 * (size_t)ChannelsCount);

    for (size_t Frame = 0; IsOk && Frame < TotalFrames; Frame++)
    {
        const size_t Offset = Frame * FrameValues;
        const size_t Available = Offset < Samples.size() ? (std::min)(FrameValues, Samples.size() - Offset) : 0;

        // The encoder needs whole frames, so pad the tail with silence
        if (Available) std::memcpy(FrameBuffer.data(), Samples.data() + Offset, Available * sizeof(int16_t));
        if (Available < FrameValues)
        {
            std::memset(FrameBuffer.data() + Available, 0, (FrameValues - Available) * sizeof(int16_t));
        }

        const auto Encoded = Encoder != nullptr
            ? opus_encode(Encoder, FrameBuffer.data(), (int)FrameSamples, PacketBuffer.data(), (opus_int32)PacketBuffer.size())
            : opus_multistream_encode(Surround, FrameBuffer.data(), (int)FrameSamples, PacketBuffer.data(), (opus_int32)PacketBuffer.size());

        if (Encoded < 0)
        {
            IsOk = false;
            break;
        }

        const bool IsLastFrame = (Frame + 1) == TotalFrames;

        // Granule positions are always counted at 48kHz regardless of the input rate; the last one
        // ends the stream where the samples end, not at the padded frame's end
        const int64_t Values = IsLastFrame ? (int64_t)(Samples.size() / ChannelsCount) : (int64_t)((Frame + 1) * FrameSamples);

        ogg_packet Packet = {};
        Packet.packet = PacketBuffer.data();
        Packet.bytes = Encoded;
        Packet.e_o_s = IsLastFrame ? 1 : 0;
        Packet.granulepos = Values * 48000 / (int64_t)SampleRate + PreSkip;
        Packet.packetno = PacketIndex++;

        ogg_stream_packetin(&Stream, &Packet);
        IsOk = FlushOggPages(Stream, Output, IsLastFrame);
    }

    ogg_stream_clear(&Stream);
    Destroy();

    return WriteWholeFile(OutputFile, Output) && IsOk;
}

bool Sound::DecodeFLACToPCM(int8_t* SoundBuffer, uint64_t SoundSize, std::vector<int16_t>& Samples, uint32_t& ChannelsCount, uint32_t& SampleRate)
{
    // Decode the whole stream into memory, the encoders want contiguous PCM
    Samples.clear();

    auto FlacInfo = std::make_unique<FLACStreamInfo>();
    FlacInfo->InputBuffer = SoundBuffer;
    FlacInfo->InputBufferSize = SoundSize;
    FlacInfo->CurrentPosition = 0;
    FlacInfo->SampleSink = &Samples;

    auto Decoder = FLAC__stream_decoder_new();

    if (Decoder == nullptr)
    {
        return false;
    }

    FLAC__stream_decoder_set_md5_checking(Decoder, false);

    const auto InitStatus = FLAC__stream_decoder_init_stream(Decoder, FLACReadFromStream, FLACSeekStream, FLACTellStream, FLACLengthStream, FLACEOFStream, FLACWriteCallback, FLACMetaDataCallback, FLACErrorCallback, FlacInfo.get());

    bool IsOk = (InitStatus == FLAC__STREAM_DECODER_INIT_STATUS_OK);

    if (IsOk)
    {
        FLAC__stream_decoder_process_until_end_of_stream(Decoder);
    }

    ChannelsCount = FlacInfo->ChannelsCount;
    SampleRate = FlacInfo->SampleRate;

    FLAC__stream_decoder_delete(Decoder);

    return IsOk && !Samples.empty() && ChannelsCount > 0 && SampleRate > 0;
}

bool Sound::TranscodeFLACToOpus(int8_t* SoundBuffer, uint64_t SoundSize, const std::string& OutputFile)
{
    std::vector<int16_t> Samples;
    uint32_t ChannelsCount = 0, SampleRate = 0;
    if (!DecodeFLACToPCM(SoundBuffer, SoundSize, Samples, ChannelsCount, SampleRate))
    {
        return false;
    }

    return EncodePCMToOpus(Samples, ChannelsCount, SampleRate, OutputFile);
}

namespace
{
    // A file in memory for the FLAC encoder: writes at the position, which a seek moves
    struct FlacMemoryFile
    {
        std::vector<uint8_t> Bytes;
        size_t Position = 0;
    };

    FLAC__StreamEncoderWriteStatus FlacMemoryWrite(const FLAC__StreamEncoder*, const FLAC__byte Buffer[], size_t Size, uint32_t, uint32_t, void* Client)
    {
        auto* File = static_cast<FlacMemoryFile*>(Client);
        if (File->Position + Size > File->Bytes.size()) File->Bytes.resize(File->Position + Size);
        if (Size) std::memcpy(File->Bytes.data() + File->Position, Buffer, Size);
        File->Position += Size;
        return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
    }

    FLAC__StreamEncoderSeekStatus FlacMemorySeek(const FLAC__StreamEncoder*, FLAC__uint64 Offset, void* Client)
    {
        static_cast<FlacMemoryFile*>(Client)->Position = (size_t)Offset;
        return FLAC__STREAM_ENCODER_SEEK_STATUS_OK;
    }

    FLAC__StreamEncoderTellStatus FlacMemoryTell(const FLAC__StreamEncoder*, FLAC__uint64* Offset, void* Client)
    {
        *Offset = static_cast<FlacMemoryFile*>(Client)->Position;
        return FLAC__STREAM_ENCODER_TELL_STATUS_OK;
    }
}

bool Sound::EncodePCMToFLAC(const std::vector<int16_t>& Samples, uint32_t ChannelsCount, uint32_t SampleRate, const std::string& OutputFile)
{
    if (ChannelsCount < 1 || ChannelsCount > 8 || SampleRate == 0 || Samples.empty())
    {
        return false;
    }

    FLAC__StreamEncoder* Encoder = FLAC__stream_encoder_new();

    if (Encoder == nullptr)
    {
        return false;
    }

    FLAC__stream_encoder_set_verify(Encoder, false);
    FLAC__stream_encoder_set_compression_level(Encoder, 5);
    FLAC__stream_encoder_set_channels(Encoder, ChannelsCount);
    FLAC__stream_encoder_set_bits_per_sample(Encoder, 16);
    FLAC__stream_encoder_set_sample_rate(Encoder, SampleRate);
    FLAC__stream_encoder_set_total_samples_estimate(Encoder, Samples.size() / ChannelsCount);

    // built in memory with the seek and tell a file gives (the encoder rewrites STREAMINFO at the
    // end, as init_file does), then written with one call
    FlacMemoryFile Output;
    Output.Bytes.reserve(4096 + Samples.size());
    bool IsOk = FLAC__stream_encoder_init_stream(Encoder, FlacMemoryWrite, FlacMemorySeek, FlacMemoryTell, nullptr, &Output) == FLAC__STREAM_ENCODER_INIT_STATUS_OK;

    // whole frames only: a truncated last frame (an odd byte count) is left out
    const size_t Frames = Samples.size() / ChannelsCount;
    std::vector<FLAC__int32> Block;
    for (size_t Done = 0; IsOk && Done < Frames;)
    {
        const size_t Need = (std::min)((size_t)4096, Frames - Done);
        Block.resize(Need * ChannelsCount);
        for (size_t i = 0; i < Block.size(); i++)
        {
            Block[i] = Samples[Done * ChannelsCount + i];
        }
        IsOk = FLAC__stream_encoder_process_interleaved(Encoder, Block.data(), (uint32_t)Need) != 0;
        Done += Need;
    }

    IsOk = (FLAC__stream_encoder_finish(Encoder) != 0) && IsOk;
    FLAC__stream_encoder_delete(Encoder);

    // written finished or not, as the file init_file opened first was
    return WriteWholeFile(OutputFile, Output.Bytes) && IsOk;
}

bool Sound::WriteOggOpus(const std::vector<std::pair<const uint8_t*, uint32_t>>& Packets, uint32_t ChannelsCount, uint32_t InputRate,
                         uint16_t PreSkip, uint64_t EndGranule, const std::string& OutputFile)
{
    if (Packets.empty() || ChannelsCount < 1 || ChannelsCount > 2)
    {
        return false;
    }

    // each packet's duration at 48 kHz, from its TOC byte and frame count
    std::vector<int> Durations;
    Durations.reserve(Packets.size());
    uint64_t Total = 0;
    for (const auto& [Data, Size] : Packets)
    {
        const int N = Size == 0 ? OPUS_INVALID_PACKET : opus_packet_get_nb_samples(Data, (opus_int32)Size, 48000);
        if (N <= 0)
        {
            return false;
        }
        Durations.push_back(N);
        Total += (uint64_t)N;
    }
    const uint64_t End = (EndGranule != 0 && EndGranule < Total) ? EndGranule : Total;

    // the file's bytes, written with one call at the end
    std::vector<uint8_t> Output;
    size_t Room = 4096;
    for (const auto& Packet : Packets) Room += Packet.second + 1;
    Output.reserve(Room + (Room / 255) + 64);

    // one stream per file: its serial from the file's name, as EncodePCMToOpus, for reproducible files
    const int OggSerial = OggSerialOf(OutputFile);

    ogg_stream_state Stream;
    if (ogg_stream_init(&Stream, OggSerial) != 0)
    {
        WriteWholeFile(OutputFile, Output);
        return false;
    }

    bool IsOk = true;
    int64_t PacketIndex = 0;

    {
        std::vector<uint8_t> Head;
        Head.insert(Head.end(), { 'O', 'p', 'u', 's', 'H', 'e', 'a', 'd' });
        Head.push_back(1);                              // version
        Head.push_back((uint8_t)ChannelsCount);
        AppendLE<uint16_t>(Head, PreSkip);
        AppendLE<uint32_t>(Head, InputRate);            // original rate, informational
        AppendLE<uint16_t>(Head, 0);                    // output gain
        Head.push_back(0);                              // channel mapping family

        ogg_packet Packet = {};
        Packet.packet = Head.data();
        Packet.bytes = (long)Head.size();
        Packet.b_o_s = 1;
        Packet.granulepos = 0;
        Packet.packetno = PacketIndex++;

        ogg_stream_packetin(&Stream, &Packet);
        IsOk = FlushOggPages(Stream, Output, true);
    }

    if (IsOk)
    {
        const std::string Vendor = "GrayWolf (remuxed)";

        std::vector<uint8_t> Tags;
        Tags.insert(Tags.end(), { 'O', 'p', 'u', 's', 'T', 'a', 'g', 's' });
        AppendLE<uint32_t>(Tags, (uint32_t)Vendor.size());
        Tags.insert(Tags.end(), Vendor.begin(), Vendor.end());
        AppendLE<uint32_t>(Tags, 0);                    // user comment count

        ogg_packet Packet = {};
        Packet.packet = Tags.data();
        Packet.bytes = (long)Tags.size();
        Packet.granulepos = 0;
        Packet.packetno = PacketIndex++;

        ogg_stream_packetin(&Stream, &Packet);
        IsOk = FlushOggPages(Stream, Output, true);
    }

    uint64_t SoFar = 0;
    for (size_t K = 0; IsOk && K < Packets.size(); K++)
    {
        SoFar += (uint64_t)Durations[K];
        const bool IsLast = (K + 1) == Packets.size();

        ogg_packet Packet = {};
        Packet.packet = const_cast<unsigned char*>(Packets[K].first);
        Packet.bytes = (long)Packets[K].second;
        Packet.e_o_s = IsLast ? 1 : 0;
        Packet.granulepos = (ogg_int64_t)(IsLast ? End : (std::min)(SoFar, End));
        Packet.packetno = PacketIndex++;

        ogg_stream_packetin(&Stream, &Packet);
        IsOk = FlushOggPages(Stream, Output, IsLast);
    }

    ogg_stream_clear(&Stream);

    return WriteWholeFile(OutputFile, Output) && IsOk;
}

bool Sound::TranscodeWAVToOpus(int8_t* SoundBuffer, uint64_t SoundSize, const std::string& OutputFile)
{
    // Walk the RIFF chunks to find the format and the PCM payload
    auto Reader = MemoryReader(SoundBuffer, SoundSize, true);

    uint32_t FormatSize = 0, SampleRate = 0, RawDataSize = 0;
    uint16_t ChannelsCount = 0, Bps = 0;
    bool KeepReading = true;

    while (KeepReading && (Reader.GetPosition() + 4) <= Reader.GetLength())
    {
        const uint32_t ChunkID = Reader.Read<uint32_t>();

        switch (ChunkID)
        {
        case 0x46464952: Reader.Advance(4); break;      // RIFF
        case 0x45564157:                                // WAVE
        {
            Reader.Advance(4);
            FormatSize = Reader.Read<uint32_t>();

            const auto FormatStart = Reader.GetPosition();
            Reader.Advance(2);
            ChannelsCount = Reader.Read<uint16_t>();
            SampleRate = Reader.Read<uint32_t>();
            Reader.Advance(6);
            Bps = Reader.Read<uint16_t>();

            Reader.SetPosition(FormatStart + FormatSize);
        }
        break;
        case 0x61746164: RawDataSize = Reader.Read<uint32_t>(); KeepReading = false; break;   // data
        default: KeepReading = false; break;
        }
    }

    // We only deal with 16bit PCM here, which is what every path feeding this produces
    if (Bps != 16 || ChannelsCount == 0 || SampleRate == 0 || RawDataSize == 0)
    {
        return false;
    }

    const auto Remaining = Reader.GetLength() - Reader.GetPosition();
    const auto ReadSize = (size_t)((std::min)((uint64_t)RawDataSize, (uint64_t)Remaining));

    std::vector<int16_t> Samples(ReadSize / sizeof(int16_t));
    Reader.Read(ReadSize, (int8_t*)Samples.data());

    return EncodePCMToOpus(Samples, ChannelsCount, SampleRate, OutputFile);
}

namespace
{
    // An Ogg Opus file as RFC 7845 lays it out: OpusHead (Channels, PreSkip; channel mapping
    // families 0 and 1, family 0 past two channels refused when Strict), OpusTags, then audio
    // packets decoded to 48 kHz PCM16, each given to Audio(Samples, Frames); Granule(position) for
    // each page with one after the head. False when it is not one, holds no audio or no channel.
    bool ReadOggOpus(const std::string& InputFile, bool Strict, uint32_t& Channels, uint32_t& PreSkip,
                     const std::function<void(int64_t Position)>& Granule, const std::function<void(const int16_t* Samples, int Frames)>& Audio)
    {
        FILE* Input = nullptr;
        fopen_s(&Input, InputFile.c_str(), "rb");
        if (Input == nullptr) return false;

        ogg_sync_state Sync; ogg_sync_init(&Sync);
        ogg_stream_state Stream; bool StreamStarted = false, IsOk = true, SawAudio = false;
        int64_t PacketIndex = 0;
        OpusMSDecoder* Decoder = nullptr;
        std::vector<int16_t> DecodeBuffer;

        while (IsOk)
        {
            char* Buffer = ogg_sync_buffer(&Sync, 8192);
            const auto Read = fread(Buffer, 1, 8192, Input);
            if (Read == 0) break;
            ogg_sync_wrote(&Sync, (long)Read);

            ogg_page Page;
            while (IsOk && ogg_sync_pageout(&Sync, &Page) == 1)
            {
                if (!StreamStarted) { if (ogg_stream_init(&Stream, ogg_page_serialno(&Page)) != 0) { IsOk = false; break; } StreamStarted = true; }
                if (ogg_stream_pagein(&Stream, &Page) != 0) { IsOk = false; break; }
                if (ogg_page_granulepos(&Page) >= 0 && PacketIndex >= 1) Granule(ogg_page_granulepos(&Page));

                ogg_packet Packet;
                while (IsOk && ogg_stream_packetout(&Stream, &Packet) == 1)
                {
                    if (PacketIndex == 0)
                    {
                        if (Packet.bytes < 19 || std::memcmp(Packet.packet, "OpusHead", 8) != 0) { IsOk = false; break; }
                        Channels = Packet.packet[9];
                        PreSkip = (uint32_t)Packet.packet[10] | ((uint32_t)Packet.packet[11] << 8);
                        const uint8_t Family = Packet.packet[18];
                        int Streams = 1, Coupled = Channels == 2 ? 1 : 0;
                        unsigned char Mapping[255] = { 0, 1 };
                        if (Family != 0)
                        {
                            if (Packet.bytes < 21 + (long)Channels) { IsOk = false; break; }
                            Streams = Packet.packet[19];
                            Coupled = Packet.packet[20];
                            std::memcpy(Mapping, Packet.packet + 21, Channels);
                        }
                        else if (Strict && (Channels < 1 || Channels > 2)) { IsOk = false; break; }
                        int Err = OPUS_OK;
                        Decoder = opus_multistream_decoder_create(48000, (int)Channels, Streams, Coupled, Mapping, &Err);
                        if (Decoder == nullptr || Err != OPUS_OK) { IsOk = false; break; }
                        DecodeBuffer.resize(5760 * (size_t)Channels);
                    }
                    else if (PacketIndex == 1)
                    {
                        if (Packet.bytes < 8 || std::memcmp(Packet.packet, "OpusTags", 8) != 0) { IsOk = false; break; }
                    }
                    else
                    {
                        const auto Decoded = opus_multistream_decode(Decoder, Packet.packet, (opus_int32)Packet.bytes, DecodeBuffer.data(), 5760, 0);
                        if (Decoded < 0) { IsOk = false; break; }
                        Audio(DecodeBuffer.data(), Decoded);
                        SawAudio = true;
                    }
                    PacketIndex++;
                }
            }
        }

        if (Decoder != nullptr) opus_multistream_decoder_destroy(Decoder);
        if (StreamStarted) ogg_stream_clear(&Stream);
        ogg_sync_clear(&Sync);
        fclose(Input);
        return IsOk && SawAudio && Channels > 0;
    }
}

bool Sound::ValidateOpusFile(const std::string& InputFile, uint32_t& ChannelsOut, uint64_t& SamplesOut)
{
    ChannelsOut = 0;
    SamplesOut = 0;
    uint32_t PreSkip = 0;
    return ReadOggOpus(InputFile, false, ChannelsOut, PreSkip, [](int64_t) {}, [&SamplesOut](const int16_t*, int Frames) { SamplesOut += (uint64_t)Frames; });
}

bool Sound::DecodeOpusToWav(const std::string& InputFile, const std::string& OutputWav)
{
    // RFC 7845 as a player reads it: OpusHead's pre-skip dropped from the start, the last page's
    // granule position ending the stream (end trimming)
    uint32_t Channels = 0, PreSkip = 0;
    int64_t LastGranule = -1;
    std::vector<int16_t> Pcm;
    if (!ReadOggOpus(InputFile, true, Channels, PreSkip, [&LastGranule](int64_t Position) { LastGranule = Position; },
                     [&](const int16_t* Samples, int Frames) { Pcm.insert(Pcm.end(), Samples, Samples + (size_t)Frames * Channels); }))
        return false;

    // the samples the granule positions say the stream holds, after the pre-skip
    uint64_t Frames = Pcm.size() / Channels;
    if (LastGranule >= 0 && (uint64_t)LastGranule < Frames) Frames = (uint64_t)LastGranule;
    const uint64_t Skip = (std::min)((uint64_t)PreSkip, Frames);
    const size_t First = (size_t)(Skip * Channels), Count = (size_t)((Frames - Skip) * Channels);

    // Write a 48 kHz PCM16 WAV.
    FILE* Out = nullptr; fopen_s(&Out, OutputWav.c_str(), "wb");
    if (Out == nullptr) return false;
    const uint32_t Rate = 48000; const uint16_t Bits = 16;
    const uint32_t DataBytes = (uint32_t)(Count * sizeof(int16_t));
    const uint16_t BlockAlign = (uint16_t)(Channels * (Bits / 8));
    auto U32 = [&](uint32_t v) { fwrite(&v, 4, 1, Out); };
    auto U16 = [&](uint16_t v) { fwrite(&v, 2, 1, Out); };
    fwrite("RIFF", 1, 4, Out); U32(36 + DataBytes); fwrite("WAVE", 1, 4, Out);
    fwrite("fmt ", 1, 4, Out); U32(16); U16(1); U16((uint16_t)Channels); U32(Rate); U32(Rate * BlockAlign); U16(BlockAlign); U16(Bits);
    fwrite("data", 1, 4, Out); U32(DataBytes);
    fwrite(Pcm.data() + First, 1, DataBytes, Out);
    fclose(Out);
    return true;
}

void Sound::WriteFLACHeaderToStream(int8_t* Buffer, uint32_t FrameRate, uint32_t ChannelsCount, uint32_t FrameCount)
{
    uint32_t ResultSize = 0;

    auto HeaderBuffer = BuildFLACHeader(FrameRate, ChannelsCount, FrameCount, ResultSize);

    if (HeaderBuffer != nullptr && ResultSize > 0)
    {
        std::memcpy(Buffer, HeaderBuffer.get(), ResultSize);
    }
}

void Sound::WriteWAVHeaderToStream(int8_t* Buffer, uint32_t FrameRate, uint32_t ChannelsCount, uint32_t TotalSize)
{
    uint32_t ResultSize = 0;

    auto HeaderBuffer = BuildWAVHeader(FrameRate, ChannelsCount, TotalSize, ResultSize);

    if (HeaderBuffer != nullptr && ResultSize > 0)
    {
        std::memcpy(Buffer, HeaderBuffer.get(), ResultSize);
    }
}

const uint32_t Sound::GetMaximumFLACHeaderSize()
{
    return 42;
}

const uint32_t Sound::GetMaximumWAVHeaderSize()
{
    return 44;
}

std::unique_ptr<int8_t[]> Sound::BuildFLACHeader(uint32_t FrameRate, uint32_t ChannelsCount, uint32_t FrameCount, uint32_t& ResultSize)
{
    uint32_t MaximumSize = GetMaximumFLACHeaderSize();
    auto HeaderBuffer = std::make_unique<int8_t[]>(MaximumSize);
    std::memset(HeaderBuffer.get(), 0, MaximumSize);

    uint32_t CurrentPosition = 0;

    std::memcpy(HeaderBuffer.get(), "fLaC", 4);
    CurrentPosition += 4;

    const uint16_t MinimumBlockSize = 0x80, MaximumBlockSize = 0, MinimumFrameSize = 0x22, MaximumFramesize = 0x4, SampleRate = 0x4;

    std::memcpy(HeaderBuffer.get() + CurrentPosition, &MinimumBlockSize, 1);
    std::memcpy(HeaderBuffer.get() + CurrentPosition + 1, &MaximumBlockSize, 2);
    CurrentPosition += 3;

    std::memcpy(HeaderBuffer.get() + CurrentPosition, &MinimumFrameSize, 1);
    std::memcpy(HeaderBuffer.get() + CurrentPosition + 1, &MaximumFramesize, 2);
    CurrentPosition += 3;

    std::memcpy(HeaderBuffer.get() + CurrentPosition, &SampleRate, 2);
    CurrentPosition += 2;

    // Skip 6 bytes padding
    CurrentPosition += 6;

    {
        uint64_t FlacFlags = 0;
        
        FlacFlags += ((uint64_t)FrameRate << 44);
        FlacFlags += ((uint64_t)(ChannelsCount - 1) << 41);
        FlacFlags += ((uint64_t)(16 - 1) << 36);
        FlacFlags += ((uint64_t)FrameCount);

        FlacFlags = _byteswap_uint64(FlacFlags);

        std::memcpy(HeaderBuffer.get() + CurrentPosition, &FlacFlags, 8);
        CurrentPosition += 8;
    }

    // Skip 16 byte checksum
    CurrentPosition += 16;

    ResultSize = MaximumSize;
    return HeaderBuffer;
}

std::unique_ptr<int8_t[]> Sound::BuildWAVHeader(uint32_t FrameRate, uint32_t ChannelsCount, uint32_t TotalSize, uint32_t& ResultSize)
{
    uint32_t MaximumSize = GetMaximumWAVHeaderSize();
    auto HeaderBuffer = std::make_unique<int8_t[]>(MaximumSize);
    std::memset(HeaderBuffer.get(), 0, MaximumSize);

    uint32_t CurrentPosition = 0;

    std::memcpy(HeaderBuffer.get(), "RIFF", 4);
    CurrentPosition += 4;

    auto TotalLength = TotalSize + 36;
    std::memcpy(HeaderBuffer.get() + CurrentPosition, &TotalLength, 4);
    CurrentPosition += 4;

    std::memcpy(HeaderBuffer.get() + CurrentPosition, "WAVEfmt ", 8);
    CurrentPosition += 8;

    const uint32_t FormatData = 16, Encoding = 1, BitsPerSample = 16, BlockAlign = (ChannelsCount * (16 / 8));
    const uint32_t AverageBps = (FrameRate * BlockAlign);

    std::memcpy(HeaderBuffer.get() + CurrentPosition, &FormatData, 4);
    CurrentPosition += 4;

    std::memcpy(HeaderBuffer.get() + CurrentPosition, &Encoding, 2);
    CurrentPosition += 2;

    std::memcpy(HeaderBuffer.get() + CurrentPosition, &ChannelsCount, 2);
    CurrentPosition += 2;

    std::memcpy(HeaderBuffer.get() + CurrentPosition, &FrameRate, 4);
    CurrentPosition += 4;

    std::memcpy(HeaderBuffer.get() + CurrentPosition, &AverageBps, 4);
    CurrentPosition += 4;

    std::memcpy(HeaderBuffer.get() + CurrentPosition, &BlockAlign, 2);
    CurrentPosition += 2;

    std::memcpy(HeaderBuffer.get() + CurrentPosition, &BitsPerSample, 2);
    CurrentPosition += 2;

    std::memcpy(HeaderBuffer.get() + CurrentPosition, "data", 4);
    CurrentPosition += 4;

    std::memcpy(HeaderBuffer.get() + CurrentPosition, &TotalSize, 4);
    CurrentPosition += 4;

    ResultSize = MaximumSize;
    return HeaderBuffer;
}
