#include "h264_decoder.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <codecapi.h>
#include <icodecapi.h>

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <emmintrin.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

namespace CastorVideo {

static void HLog(const char* fmt, ...) {
    FILE* f = std::fopen("castor_h264_udp.log", "a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(f, fmt, ap);
    va_end(ap);
    std::fputc('\n', f);
    std::fclose(f);
}

static const GUID CLSID_CMSH264DecoderMFT_Local = 
    { 0x62ce7e72, 0x4c71, 0x4d20, { 0xb1, 0x5d, 0x45, 0x28, 0x31, 0xa8, 0x7d, 0x9d } };
static const GUID IID_ICodecAPI_Local = 
    { 0x901db4c7, 0x31ce, 0x41a2, { 0x85, 0xdc, 0x8f, 0xa0, 0xbf, 0x41, 0xb8, 0xda } };
static const GUID CODECAPI_AVLowLatencyMode_Local = 
    { 0x9c27891a, 0xed7a, 0x40e1, { 0x88, 0xe8, 0xb2, 0x27, 0x27, 0xa0, 0x24, 0xee } };

// NV12 -> BGRA，SSE2 手写向量化（每次 8 像素）。YUV BT.601 limited range。
static inline void ConvertNV12ToBGRA_Fast(const uint8_t* nv12, int width, int height, int strideY, uint8_t* bgra) {
    if (strideY <= 0) strideY = width;
    const uint8_t* yPlane = nv12;
    const uint8_t* uvPlane = nv12 + static_cast<size_t>(strideY) * height;

    const __m128i zero = _mm_setzero_si128();
    const __m128i c16  = _mm_set1_epi16(16);
    const __m128i c128 = _mm_set1_epi16(128);
    const __m128i cR   = _mm_set1_epi32((409 << 16) | 298);   // 系数对 (298, 409)
    const __m128i cB   = _mm_set1_epi32((516 << 16) | 298);   // (298, 516)
    const __m128i cG1  = _mm_set1_epi32((static_cast<uint16_t>(-100) << 16) | static_cast<uint16_t>(298)); // (298, -100)
    const __m128i cG2  = _mm_set1_epi32(static_cast<uint16_t>(-208));                                       // (-208, 0)
    const __m128i cRnd = _mm_set1_epi32(128);
    const __m128i aFF  = _mm_set1_epi8(static_cast<char>(0xFF));
    const int w8 = width & ~7;

    for (int y = 0; y < height; ++y) {
        const uint8_t* yRow = yPlane + static_cast<size_t>(y) * strideY;
        const uint8_t* uvRow = uvPlane + static_cast<size_t>(y >> 1) * strideY;
        uint8_t* dst = bgra + static_cast<size_t>(y) * width * 4;
        int x = 0;

        for (; x < w8; x += 8) {
            __m128i Ym = _mm_sub_epi16(_mm_unpacklo_epi8(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(yRow + x)), zero), c16);
            __m128i UV = _mm_unpacklo_epi8(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(uvRow + x)), zero); // U0,V0..U3,V3

            // 每个 U/V 复制给相邻 2 像素
            __m128i lo = _mm_unpacklo_epi16(UV, UV);   // U0,U0,V0,V0,U1,U1,V1,V1
            __m128i hi = _mm_unpackhi_epi16(UV, UV);   // U2,U2,V2,V2,U3,U3,V3,V3
            __m128i Udup = _mm_unpacklo_epi64(_mm_shuffle_epi32(lo, _MM_SHUFFLE(2, 0, 2, 0)),
                                              _mm_shuffle_epi32(hi, _MM_SHUFFLE(2, 0, 2, 0)));
            __m128i Vdup = _mm_unpacklo_epi64(_mm_shuffle_epi32(lo, _MM_SHUFFLE(3, 1, 3, 1)),
                                              _mm_shuffle_epi32(hi, _MM_SHUFFLE(3, 1, 3, 1)));
            __m128i Um = _mm_sub_epi16(Udup, c128);
            __m128i Vm = _mm_sub_epi16(Vdup, c128);

            __m128i yvLo = _mm_unpacklo_epi16(Ym, Vm), yvHi = _mm_unpackhi_epi16(Ym, Vm);
            __m128i yuLo = _mm_unpacklo_epi16(Ym, Um), yuHi = _mm_unpackhi_epi16(Ym, Um);
            __m128i vzLo = _mm_unpacklo_epi16(Vm, zero), vzHi = _mm_unpackhi_epi16(Vm, zero);

            __m128i rLo = _mm_srai_epi32(_mm_add_epi32(_mm_madd_epi16(yvLo, cR), cRnd), 8);
            __m128i rHi = _mm_srai_epi32(_mm_add_epi32(_mm_madd_epi16(yvHi, cR), cRnd), 8);
            __m128i bLo = _mm_srai_epi32(_mm_add_epi32(_mm_madd_epi16(yuLo, cB), cRnd), 8);
            __m128i bHi = _mm_srai_epi32(_mm_add_epi32(_mm_madd_epi16(yuHi, cB), cRnd), 8);
            __m128i gLo = _mm_srai_epi32(_mm_add_epi32(_mm_add_epi32(_mm_madd_epi16(yuLo, cG1), _mm_madd_epi16(vzLo, cG2)), cRnd), 8);
            __m128i gHi = _mm_srai_epi32(_mm_add_epi32(_mm_add_epi32(_mm_madd_epi16(yuHi, cG1), _mm_madd_epi16(vzHi, cG2)), cRnd), 8);

            __m128i R8 = _mm_packus_epi16(_mm_packs_epi32(rLo, rHi), _mm_packs_epi32(rLo, rHi));
            __m128i G8 = _mm_packus_epi16(_mm_packs_epi32(gLo, gHi), _mm_packs_epi32(gLo, gHi));
            __m128i B8 = _mm_packus_epi16(_mm_packs_epi32(bLo, bHi), _mm_packs_epi32(bLo, bHi));

            // B8/G8/R8 的低 8 字节就是像素 0..7；直接按 16 位交错成两组 BGRA
            __m128i BG = _mm_unpacklo_epi8(B8, G8);   // 16B: B0,G0,...,B7,G7
            __m128i RA = _mm_unpacklo_epi8(R8, aFF);  // 16B: R0,FF,...,R7,FF
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dst), _mm_unpacklo_epi16(BG, RA));      // 像素 0-3
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + 16), _mm_unpackhi_epi16(BG, RA)); // 像素 4-7
            dst += 32;
        }
        for (; x < width; ++x) { // 余数标量兜底
            int Y = yRow[x] - 16;
            int u = uvRow[x & ~1] - 128;
            int v = uvRow[(x & ~1) + 1] - 128;
            int r = (298 * Y + 409 * v + 128) >> 8;
            int g = (298 * Y - 100 * u - 208 * v + 128) >> 8;
            int b = (298 * Y + 516 * u + 128) >> 8;
            dst[0] = static_cast<uint8_t>(b < 0 ? 0 : (b > 255 ? 255 : b));
            dst[1] = static_cast<uint8_t>(g < 0 ? 0 : (g > 255 ? 255 : g));
            dst[2] = static_cast<uint8_t>(r < 0 ? 0 : (r > 255 ? 255 : r));
            dst[3] = 255;
            dst += 4;
        }
    }
}

struct H264Decoder::Impl {
    IMFTransform* decoder = nullptr;
    IMFMediaBuffer* outMediaBuffer = nullptr;
    IMFSample* outSample = nullptr;
    DWORD outBufferSize = 0;

    int currentWidth = 0;
    int currentHeight = 0;
    int currentStride = 0;
    bool initialized = false;
    bool streamingStarted = false;   // BEGIN_STREAMING 是否已发
    bool outputIsArgb = false;       // 输出是否为 ARGB32（是则不需我们转换）
    int64_t sampleTimeUs = 0;        // 单调递增的样本时间戳

    // 从 Annex-B 码流里抽取 SPS(0x67)/PPS(0x68) 作为 MF_MT_MPEG_SEQUENCE_HEADER
    static bool extractCsd(const uint8_t* data, size_t size, std::vector<uint8_t>& csd) {
        csd.clear();
        size_t i = 0;
        size_t spsStart = SIZE_MAX, ppsEnd = SIZE_MAX;
        auto findStart = [&](size_t from) -> size_t {
            for (size_t k = from; k + 3 < size; ++k) {
                if (data[k] == 0 && data[k + 1] == 0 && data[k + 2] == 1) return k;
                if (k + 4 < size && data[k] == 0 && data[k + 1] == 0 && data[k + 2] == 0 && data[k + 3] == 1) return k;
            }
            return SIZE_MAX;
        };
        (void)i;
        size_t pos = findStart(0);
        while (pos != SIZE_MAX) {
            size_t nalStart = (data[pos + 2] == 1) ? pos + 3 : pos + 4;
            if (nalStart >= size) break;
            const uint8_t nalType = data[nalStart] & 0x1F;
            size_t next = findStart(nalStart);
            size_t nalEnd = (next == SIZE_MAX) ? size : next;
            if (nalType == 7) { if (spsStart == SIZE_MAX) spsStart = pos; }
            else if (nalType == 8) { ppsEnd = nalEnd; }
            else if (spsStart != SIZE_MAX && ppsEnd != SIZE_MAX) break;
            pos = next;
        }
        if (spsStart != SIZE_MAX && ppsEnd != SIZE_MAX && ppsEnd > spsStart) {
            csd.assign(data + spsStart, data + ppsEnd);
            return true;
        }
        return false;
    }

    bool Init() {
        if (initialized) return true;
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        MFStartup(MF_VERSION);

        HRESULT hr = CoCreateInstance(CLSID_CMSH264DecoderMFT_Local, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_IMFTransform, reinterpret_cast<void**>(&decoder));
        if (FAILED(hr) || !decoder) return false;

        // 启用微软原生低延迟模式 (0 缓存与 0 前瞻队列)
        ICodecAPI* codecApi = nullptr;
        if (SUCCEEDED(decoder->QueryInterface(IID_ICodecAPI_Local, reinterpret_cast<void**>(&codecApi)))) {
            VARIANT val;
            VariantInit(&val);
            val.vt = VT_UI4;
            val.ulVal = 1;
            codecApi->SetValue(&CODECAPI_AVLowLatencyMode_Local, &val);
            codecApi->Release();
        }

        // 异步 MFT 解锁（部分解码器要求）
        IMFAttributes* attrs = nullptr;
        if (SUCCEEDED(decoder->GetAttributes(&attrs)) && attrs) {
            UINT32 isAsync = 0;
            if (SUCCEEDED(attrs->GetUINT32(MF_TRANSFORM_ASYNC, &isAsync)) && isAsync) {
                attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
            }
            attrs->Release();
        }

        // 配置输入类型: H.264（先设基础类型；拿到 SPS/PPS 后再带序列头重设）
        IMFMediaType* inType = nullptr;
        hr = MFCreateMediaType(&inType);
        if (SUCCEEDED(hr)) {
            inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            inType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
            hr = decoder->SetInputType(0, inType, 0);
            inType->Release();
        }

        if (FAILED(hr)) {
            Shutdown();
            return false;
        }

        // 只发 FLUSH；BEGIN_STREAMING 推迟到拿到 SPS/PPS、带序列头设好输入类型之后
        decoder->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);

        EnsureOutputSample(4147200); // 默认预分配 4MB 输出缓冲 (可容纳 1080P NV12)

        initialized = true;
        return true;
    }

    void EnsureOutputSample(DWORD neededSize) {
        if (outSample && outMediaBuffer && outBufferSize >= neededSize) return;

        if (outSample) { outSample->Release(); outSample = nullptr; }
        if (outMediaBuffer) { outMediaBuffer->Release(); outMediaBuffer = nullptr; }

        outBufferSize = neededSize > 0 ? neededSize : 4147200;
        MFCreateMemoryBuffer(outBufferSize, &outMediaBuffer);
        MFCreateSample(&outSample);
        if (outSample && outMediaBuffer) {
            outSample->AddBuffer(outMediaBuffer);
        }
    }

    void Shutdown() {
        if (outSample) { outSample->Release(); outSample = nullptr; }
        if (outMediaBuffer) { outMediaBuffer->Release(); outMediaBuffer = nullptr; }
        if (decoder) {
            decoder->Release();
            decoder = nullptr;
        }
        if (initialized) {
            MFShutdown();
            CoUninitialize();
            initialized = false;
        }
    }

    void ConfigureOutputMediaType() {
        if (!decoder) return;
        // 优先让解码器直接出 ARGB32（省掉我们那步 NV12->BGRA 转换）；不行退 NV12
        const GUID* prefs[2] = { &MFVideoFormat_ARGB32, &MFVideoFormat_NV12 };
        for (int pi = 0; pi < 2; ++pi) {
            IMFMediaType* pType = nullptr;
            for (DWORD i = 0; SUCCEEDED(decoder->GetOutputAvailableType(0, i, &pType)); ++i) {
                GUID subType{};
                if (SUCCEEDED(pType->GetGUID(MF_MT_SUBTYPE, &subType)) && subType == *prefs[pi]) {
                    if (SUCCEEDED(decoder->SetOutputType(0, pType, 0))) {
                        outputIsArgb = (pi == 0);
                        pType->Release();
                        pType = nullptr;

                        IMFMediaType* curType = nullptr;
                        if (SUCCEEDED(decoder->GetOutputCurrentType(0, &curType))) {
                            UINT32 cw = 0, ch = 0;
                            if (SUCCEEDED(MFGetAttributeSize(curType, MF_MT_FRAME_SIZE, &cw, &ch)) && cw > 0 && ch > 0) {
                                currentWidth = static_cast<int>(cw);
                                currentHeight = static_cast<int>(ch);
                            }
                            UINT32 stride = 0;
                            if (SUCCEEDED(curType->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride)) && stride > 0) {
                                currentStride = static_cast<int>(stride);
                            } else {
                                currentStride = outputIsArgb ? currentWidth * 4 : currentWidth;
                            }
                            curType->Release();
                        }
                        MFT_OUTPUT_STREAM_INFO streamInfo{};
                        decoder->GetOutputStreamInfo(0, &streamInfo);
                        EnsureOutputSample(streamInfo.cbSize);
                        HLog("输出类型: %s %dx%d stride=%d", outputIsArgb ? "ARGB32" : "NV12",
                             currentWidth, currentHeight, currentStride);
                        return;
                    }
                }
                pType->Release();
            }
        }
    }

    bool DecodeFrame(const uint8_t* data, size_t size, std::vector<uint8_t>& outBgra, int& outW, int& outH,
                     bool rawNv12, int* outStride) {
        if (!decoder || !data || size == 0) return false;

        IMFMediaBuffer* pBuffer = nullptr;
        HRESULT hr = MFCreateMemoryBuffer(static_cast<DWORD>(size), &pBuffer);
        if (FAILED(hr)) return false;

        BYTE* pDst = nullptr;
        pBuffer->Lock(&pDst, nullptr, nullptr);
        memcpy(pDst, data, size);
        pBuffer->Unlock();
        pBuffer->SetCurrentLength(static_cast<DWORD>(size));

        IMFSample* pSample = nullptr;
        hr = MFCreateSample(&pSample);
        if (FAILED(hr)) {
            pBuffer->Release();
            return false;
        }
        pSample->AddBuffer(pBuffer);
        pBuffer->Release();

        // 首次拿到 SPS/PPS：带序列头重设输入类型 + 开始流。没有 SPS/PPS 则丢弃本帧。
        if (!streamingStarted) {
            std::vector<uint8_t> csd;
            if (extractCsd(data, size, csd) && !csd.empty()) {
                IMFMediaType* it = nullptr;
                if (SUCCEEDED(MFCreateMediaType(&it)) && it) {
                    it->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
                    it->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
                    it->SetBlob(MF_MT_MPEG_SEQUENCE_HEADER, csd.data(), static_cast<UINT32>(csd.size()));
                    decoder->SetInputType(0, it, 0);
                    it->Release();
                }
                // 关键：输入类型设好后必须再设「输出类型」，否则一直是 MF_E_TRANSFORM_TYPE_NOT_SET
                ConfigureOutputMediaType();
                decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
                decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
                streamingStarted = true;
                HLog("decoder input type finalized: csd=%zu bytes, outType w=%d h=%d, streaming started",
                     csd.size(), currentWidth, currentHeight);
            } else {
                pSample->Release();
                return false;
            }
        }
        // MF 解码器需要单调时间戳，否则可能不产帧
        pSample->SetSampleTime(sampleTimeUs);
        pSample->SetSampleDuration(16666);
        sampleTimeUs += 16666;

        static int sLog = 0;
        const bool doLog = (sLog++ < 12);
        const int logId = sLog - 1;
        auto tEntry = std::chrono::steady_clock::now();

        hr = decoder->ProcessInput(0, pSample, 0);
        pSample->Release();
        if (doLog) HLog("decode#%d in: size=%zu hrIn=0x%08X", logId, size, static_cast<unsigned>(hr));
        if (FAILED(hr)) {
            return false;
        }

        if (outMediaBuffer) {
            outMediaBuffer->SetCurrentLength(0);
        }

        MFT_OUTPUT_DATA_BUFFER outputDataBuffer{};
        outputDataBuffer.dwStreamID = 0;
        outputDataBuffer.pSample = outSample;
        outputDataBuffer.dwStatus = 0;
        DWORD status = 0;

        hr = decoder->ProcessOutput(0, 1, &outputDataBuffer, &status);
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            ConfigureOutputMediaType();
            outputDataBuffer.pSample = outSample;
            outputDataBuffer.dwStatus = 0;
            hr = decoder->ProcessOutput(0, 1, &outputDataBuffer, &status);
        }
        if (doLog) HLog("decode#%d out: hrOut=0x%08X status=0x%08X w=%d h=%d", logId,
                       static_cast<unsigned>(hr), static_cast<unsigned>(status), currentWidth, currentHeight);
        auto tMft = std::chrono::steady_clock::now();

        if (SUCCEEDED(hr) && outMediaBuffer) {
            BYTE* pData = nullptr;
            DWORD curLen = 0;
            outMediaBuffer->Lock(&pData, nullptr, &curLen);
            if (pData && currentWidth > 0 && currentHeight > 0) {
                outW = currentWidth;
                outH = currentHeight;
                const int cw = currentWidth, ch = currentHeight;
                if (rawNv12) {
                    // GPU 路径：不做 CPU 转换，原始 NV12 拷出给调用方
                    const int stride = currentStride > 0 ? currentStride : cw;
                    if (outStride) *outStride = stride;
                    const size_t nv12Size = static_cast<size_t>(stride) * ch * 3 / 2;
                    outBgra.resize(nv12Size);
                    std::memcpy(outBgra.data(), pData, nv12Size);
                } else {
                    outBgra.resize(static_cast<size_t>(cw) * ch * 4);
                    if (outputIsArgb) {
                        // 解码器已直出 BGRA，仅按行进 stride 拷贝
                        const int srcStride = currentStride > 0 ? currentStride : cw * 4;
                        uint8_t* d = outBgra.data();
                        for (int ry = 0; ry < ch; ++ry) {
                            std::memcpy(d + static_cast<size_t>(ry) * cw * 4,
                                        pData + static_cast<size_t>(ry) * srcStride,
                                        static_cast<size_t>(cw) * 4);
                        }
                    } else {
                        const int stride = currentStride > 0 ? currentStride : cw;
                        ConvertNV12ToBGRA_Fast(pData, cw, ch, stride, outBgra.data());
                    }
                }
                outMediaBuffer->Unlock();

                auto tConv = std::chrono::steady_clock::now();
                static double sMftSum = 0, sConvSum = 0;
                static int sT = 0;
                sMftSum += std::chrono::duration_cast<std::chrono::microseconds>(tMft - tEntry).count() / 1000.0;
                sConvSum += std::chrono::duration_cast<std::chrono::microseconds>(tConv - tMft).count() / 1000.0;
                if (++sT >= 30) {
                    HLog("timing avg: MFT解码=%.2fms NV12->BGRA转换=%.2fms (%dx%d)",
                         sMftSum / sT, sConvSum / sT, currentWidth, currentHeight);
                    sMftSum = 0; sConvSum = 0; sT = 0;
                }
                return true;
            }
            outMediaBuffer->Unlock();
        }

        return false;
    }
};

H264Decoder::H264Decoder() : impl_(std::make_unique<Impl>()) {}
H264Decoder::~H264Decoder() { Shutdown(); }

bool H264Decoder::Init() { return impl_->Init(); }
void H264Decoder::Shutdown() { impl_->Shutdown(); }
bool H264Decoder::DecodeFrame(const uint8_t* data, size_t size, std::vector<uint8_t>& outBgra, int& outW, int& outH,
                              bool rawNv12, int* outStride) {
    return impl_->DecodeFrame(data, size, outBgra, outW, outH, rawNv12, outStride);
}

} // namespace CastorVideo
