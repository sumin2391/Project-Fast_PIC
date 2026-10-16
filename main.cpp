#include <winsock2.h>
#include "ImGui/imgui.h"
#include "ImGui/imgui_impl_win32.h"
#include "ImGui/imgui_impl_dx11.h"
#include <d3d11.h>
#include <tchar.h>

#include <iostream>
#include <cstdio>
#include <cwchar>
#include <vector>
#include <string>
#include <filesystem>
#include <fstream>
#include <limits>
#include <cstring>
#include <chrono>
#include <cmath>
#include <unordered_map>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <atomic>
#include <deque>
#include <algorithm>
#include <iomanip>
#include <sstream>
#include <ctime>
#include <windows.h>

#ifndef USE_LIBRAW
#define USE_LIBRAW
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

//  stb_image 라이브러리 연결
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <windows.h>
#include <shobjidl.h>
#include <propkey.h>     
#include <propvarutil.h> 
// WIC for high-performance image decoding
#include <wincodec.h>
#pragma comment(lib, "windowscodecs.lib")

#pragma execution_character_set("utf-8")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "propsys.lib")
#pragma comment(lib, "d3d11.lib") //  그래픽 가속(DirectX 11) 라이브러리 연결!
#pragma comment(linker, "/SUBSYSTEM:windows /ENTRY:mainCRTStartup")

namespace fs = std::filesystem;

extern ID3D11Device* g_pd3dDevice;
extern ID3D11ShaderResourceView* g_currentImageSRV;
extern int g_currentImageWidth;
extern int g_currentImageHeight;

// 간단한 성능 집계용 카운터(나노초 합계, 호출 횟수)
static std::atomic<long long> g_decodeTotalNs{ 0 };
static std::atomic<int> g_decodeCount{ 0 };
static std::atomic<long long> g_uploadTotalNs{ 0 };
static std::atomic<int> g_uploadCount{ 0 };

// --- V5에서 만든 데이터 구조체와 함수들 ---
struct ImageData {
    std::string filePath;
    std::wstring wFilePath;
    std::string fileName;
    std::string extension; // 필터링용 확장자 (.jpg, .arw 등)
    std::string captureTime;
    time_t captureTimeT;
    int orientation = 1;
    bool hasAF = false;
    float afX = 0.5f; float afY = 0.5f; float afW = 0.05f; float afH = 0.05f;
    int groupID;
    enum class Status { None = 0, Pick = 1, Hold = 2, Reject = 3 } status = Status::None;

    // 필터/정렬 및 화면 표시용 메타데이터
    bool exifLoaded = false;
    std::string cameraModel = "";
    std::string lensModel = "";
    double focalLength = 0.0;
    double fNumber = 0.0;
    double exposureTime = 0.0;
    std::string exposureTimeStr = "";
    int iso = 0;
};

bool GetAFPointViaExifTool(const std::wstring& filePath, int imgW, int imgH, int orientation,
    float& afX, float& afY, float& afW, float& afH) {
    if (imgW <= 0 || imgH <= 0) return false;

    // 소니 전용 필수 태그(-SonyImageWidth, -SonyImageHeight, -FocusPositionX 등) 추가
    std::wstring command = L"exiftool.exe -n -s -ImageWidth -ImageHeight -AFAreaXPosition -AFAreaYPosition -AFAreaWidth -AFAreaHeight -SonyImageWidth -SonyImageHeight -FocusPositionX -FocusPositionY -AFPosition -FlexibleSpotPosition -FocusLocation -AFPointSelected \"" + filePath + L"\"";
    FILE* pipe = _wpopen(command.c_str(), L"rt");
    if (!pipe) return false;

    std::vector<std::wstring> lines;
    wchar_t buffer[256];
    while (fgetws(buffer, (int)(sizeof(buffer) / sizeof(buffer[0])), pipe)) {
        std::wstring line(buffer);
        while (!line.empty() && (line.back() == L'\r' || line.back() == L'\n')) line.pop_back();
        lines.push_back(std::move(line));
    }
    const int exitCode = _pclose(pipe);
    if (exitCode != 0 || lines.empty()) return false;

    auto parseNumbers = [](const std::wstring& text, std::vector<double>& values) {
        const wchar_t* cursor = text.c_str();
        while (*cursor) {
            while (*cursor && !((*cursor >= L'0' && *cursor <= L'9') || *cursor == L'-' || *cursor == L'+' || *cursor == L'.')) ++cursor;
            if (!*cursor) break;
            wchar_t* end = nullptr;
            double value = wcstod(cursor, &end);
            if (end == cursor) { ++cursor; continue; }
            values.push_back(value);
            cursor = end;
        }
        };

    double exifImageWidth = 0.0, exifImageHeight = 0.0;
    double sonyImgW = 0.0, sonyImgH = 0.0;
    double x = -1.0, y = -1.0, w = 0.0, h = 0.0;

    // 우선순위에 따른 좌표 저장용 변수
    double pixelX = -1.0, pixelY = -1.0;
    double gridX = -1.0, gridY = -1.0;

    for (const std::wstring& line : lines) {
        std::vector<double> values;
        parseNumbers(line, values);
        if (values.empty()) continue;

        // 공통 이미지 크기 및 소니 센서 크기
        if (line.find(L"ImageWidth") != std::wstring::npos && line.find(L"Sony") == std::wstring::npos) exifImageWidth = values[0];
        else if (line.find(L"ImageHeight") != std::wstring::npos && line.find(L"Sony") == std::wstring::npos) exifImageHeight = values[0];
        else if (line.find(L"SonyImageWidth") != std::wstring::npos) sonyImgW = values[0];
        else if (line.find(L"SonyImageHeight") != std::wstring::npos) sonyImgH = values[0];

        // 니콘 및 공용 태그
        else if (line.find(L"AFAreaXPosition") != std::wstring::npos) pixelX = values[0];
        else if (line.find(L"AFAreaYPosition") != std::wstring::npos) pixelY = values[0];
        else if (line.find(L"AFAreaWidth") != std::wstring::npos) w = values[0];
        else if (line.find(L"AFAreaHeight") != std::wstring::npos) h = values[0];

        // 🌟 1순위: 소니 최신 태그 (FocusLocation 처리 강화)
        else if (line.find(L"FocusLocation") != std::wstring::npos) {
            if (values.size() >= 4) {
                // 예: 4608 3072 2808 1772 (센서가로, 센서세로, X, Y)
                sonyImgW = values[0];
                sonyImgH = values[1];
                pixelX = values[2];
                pixelY = values[3];
            }
            else if (values.size() >= 2) {
                pixelX = values[0];
                pixelY = values[1];
            }
        }
        // 🌟 FlexibleSpotPosition (값이 0 0 인 경우는 비활성화 상태이므로 무시)
        else if (line.find(L"FlexibleSpotPosition") != std::wstring::npos && values.size() >= 2) {
            if (values[0] != 0.0 || values[1] != 0.0) {
                if (pixelX < 0) { // FocusLocation에서 찾은 값이 없을 때만 백업으로 사용
                    pixelX = values[0];
                    pixelY = values[1];
                }
            }
        }

        // 🌟 2순위: 소니 구형/기타 그리드(Grid) 태그
        else if (line.find(L"FocusPositionX") != std::wstring::npos) gridX = values[0];
        else if (line.find(L"FocusPositionY") != std::wstring::npos) gridY = values[0];
        else if (line.find(L"AFPosition") != std::wstring::npos && values.size() >= 2) {
            gridX = values[0];
            gridY = values[1];
        }
    }

    double sourceW = (exifImageWidth > 0) ? exifImageWidth : imgW;
    double sourceH = (exifImageHeight > 0) ? exifImageHeight : imgH;

    // =========================================================
    // 🌟 좌표 결정 로직 (우선순위 및 비례식 적용)
    // =========================================================
    if (pixelX >= 0 && pixelY >= 0) {
        // 소니 최신 기종처럼 센서 기준 크기(sonyImgW)가 같이 넘어온 경우 비례식 적용
        if (sonyImgW > 0 && sonyImgH > 0) {
            x = (pixelX / sonyImgW) * sourceW;
            y = (pixelY / sonyImgH) * sourceH;
        }
        else {
            // 그 외 니콘 등은 픽셀 좌표 그대로 사용
            x = pixelX;
            y = pixelY;
        }
    }
    else if (gridX >= 0 && gridY >= 0) {
        // FocusPositionX/Y 처럼 그리드 좌표만 있을 때
        double gridW_ref = sourceW;
        double gridH_ref = sourceH;

        if (sonyImgW > 0) {
            gridW_ref = sonyImgW;
            gridH_ref = sonyImgH;
        }
        else {
            if (gridX <= 299.0 && gridY <= 299.0) { gridW_ref = 299.0; gridH_ref = 299.0; }
            else if (gridX <= 480.0 && gridY <= 320.0) { gridW_ref = 480.0; gridH_ref = 320.0; }
            else if (gridX <= 640.0 && gridY <= 480.0) { gridW_ref = 640.0; gridH_ref = 480.0; }
        }

        x = (gridX / gridW_ref) * sourceW;
        y = (gridY / gridH_ref) * sourceH;
    }

    if (x < 0 || y < 0) return false;

    if (w <= 0) w = sourceW * 0.05;
    if (h <= 0) h = sourceH * 0.05;

    // 0~1 사이의 정규화 비율로 변환하거나 퍼센트 환산
    auto normalize = [](double value, double dimension) {
        if (value >= 0.0 && value <= 1.0) return value;
        if (value >= 0.0 && value <= 100.0 && std::floor(value) != value) return value / 100.0;
        return value / dimension;
        };

    double nx = normalize(x, sourceW);
    double ny = normalize(y, sourceH);
    double nw = normalize(w, sourceW);
    double nh = normalize(h, sourceH);

    // 카메라 회전(Orientation) 방향에 따른 좌표계 보정
    if (orientation == 6) {
        const double oldX = nx;
        nx = 1.0 - ny;
        ny = oldX;
        std::swap(nw, nh);
    }
    else if (orientation == 8) {
        const double oldX = nx;
        nx = ny;
        ny = 1.0 - oldX;
        std::swap(nw, nh);
    }
    else if (orientation == 3) {
        nx = 1.0 - nx;
        ny = 1.0 - ny;
    }

    if (!std::isfinite(nx) || !std::isfinite(ny) || !std::isfinite(nw) || !std::isfinite(nh)) return false;
    afX = static_cast<float>((std::max)(0.0, (std::min)(1.0, nx)));
    afY = static_cast<float>((std::max)(0.0, (std::min)(1.0, ny)));
    afW = static_cast<float>((std::max)(0.005, (std::min)(1.0, nw)));
    afH = static_cast<float>((std::max)(0.005, (std::min)(1.0, nh)));

    return true;
}

bool isImageFile(const fs::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return ::tolower(c); });
    return (ext == ".jpg" || ext == ".jpeg" || ext == ".png" ||
        ext == ".raw" || ext == ".cr2" || ext == ".nef" || ext == ".arw");
}

// SRV로부터 텍스처 크기 얻기
void GetTextureSizeFromSRV(ID3D11ShaderResourceView* srv, int& outW, int& outH) {
    outW = outH = 0;
    if (!srv) return;
    ID3D11Resource* res = nullptr;
    srv->GetResource(&res);
    if (!res) return;
    ID3D11Texture2D* tex = nullptr;
    if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex)))) {
        D3D11_TEXTURE2D_DESC desc; tex->GetDesc(&desc);
        outW = (int)desc.Width; outH = (int)desc.Height;
        tex->Release();
    }
    res->Release();
}

// Read entire file into vector<unsigned char> using std::ifstream with std::filesystem::path
// This lets the standard library use native wide APIs on Windows and avoids manual codepage conversions.
static bool ReadFileToVector(const fs::path& path, std::vector<unsigned char>& out) {
    out.clear();
    std::ifstream ifs(path, std::ios::binary | std::ios::in);
    if (!ifs.is_open()) return false;
    ifs.seekg(0, std::ios::end);
    std::streamoff size = ifs.tellg();
    if (size <= 0) return false;
    ifs.seekg(0, std::ios::beg);
    out.resize((size_t)size);
    ifs.read(reinterpret_cast<char*>(out.data()), (std::streamsize)size);
    bool good = ifs.good();
    ifs.close();
    return good;
}

void RotateRGBABuffer(std::vector<unsigned char>& rgba, int& width, int& height, int orientation) {
    if (orientation != 3 && orientation != 6 && orientation != 8) return;
    if (width <= 0 || height <= 0 || rgba.size() != (size_t)width * (size_t)height * 4) return;

    const int srcWidth = width;
    const int srcHeight = height;
    const int dstWidth = (orientation == 6 || orientation == 8) ? srcHeight : srcWidth;
    const int dstHeight = (orientation == 6 || orientation == 8) ? srcWidth : srcHeight;
    std::vector<unsigned char> rotated((size_t)dstWidth * (size_t)dstHeight * 4);

    for (int y = 0; y < srcHeight; ++y) {
        for (int x = 0; x < srcWidth; ++x) {
            int dstX = x;
            int dstY = y;
            if (orientation == 3) {
                dstX = srcWidth - 1 - x;
                dstY = srcHeight - 1 - y;
            }
            else if (orientation == 6) {
                dstX = srcHeight - 1 - y;
                dstY = x;
            }
            else { // Orientation 8: 270 degrees clockwise
                dstX = y;
                dstY = srcWidth - 1 - x;
            }

            const size_t srcOffset = ((size_t)y * srcWidth + x) * 4;
            const size_t dstOffset = ((size_t)dstY * dstWidth + dstX) * 4;
            std::copy_n(rgba.data() + srcOffset, 4, rotated.data() + dstOffset);
        }
    }

    rgba.swap(rotated);
    width = dstWidth;
    height = dstHeight;
}

bool isRawFile(const fs::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return ::tolower(c); });
    return (ext == ".raw" || ext == ".cr2" || ext == ".nef" || ext == ".arw" || ext == ".dng" || ext == ".raf");
}

// --- RAW 처리 및 LRU 텍스처 캐시(간단 구현) ---
#ifdef USE_LIBRAW
#include <libraw/libraw.h>

// 기존 std::string 대신 std::wstring을 받도록 변경
ID3D11ShaderResourceView* LoadRawPreviewToSRV(const std::wstring& filename, int* outW, int* outH) {
    if (outW) *outW = 0;
    if (outH) *outH = 0;
    LibRaw RawProcessor;

    //  윈도우 유니코드(wide-char) 경로를 직접 전달하여 한글 깨짐 방지
    int ret = RawProcessor.open_file(filename.c_str());
    if (ret != LIBRAW_SUCCESS) {
        RawProcessor.recycle();
        return nullptr;
    }
    ret = RawProcessor.unpack();
    if (ret != LIBRAW_SUCCESS) {
        RawProcessor.recycle();
        return nullptr;
    }
    // 기본적인 처리: dcraw_process -> make mem image
    ret = RawProcessor.dcraw_process();
    if (ret != LIBRAW_SUCCESS) {
        RawProcessor.recycle();
        return nullptr;
    }
    libraw_processed_image_t* image = RawProcessor.dcraw_make_mem_image(&ret);
    if (!image || !image->data) {
        if (image) LibRaw::dcraw_clear_mem(image);
        RawProcessor.recycle();
        return nullptr;
    }

    int w = image->width;
    int h = image->height;
    int colors = image->colors; // typically 3

    // libraw gives packed RGB/RGBA depending on colors. Convert to RGBA8 for D3D texture.
    std::vector<unsigned char> rgba;
    rgba.resize((size_t)w * (size_t)h * 4);
    unsigned char* src = image->data;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            size_t si = (size_t)(y * w + x) * colors;
            size_t di = (size_t)(y * w + x) * 4;
            rgba[di + 0] = src[si + 0];
            rgba[di + 1] = src[si + 1];
            rgba[di + 2] = src[si + 2];
            rgba[di + 3] = 255;
        }
    }

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = (UINT)w;
    desc.Height = (UINT)h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = 0;

    D3D11_SUBRESOURCE_DATA initData{};
    initData.pSysMem = rgba.data();
    initData.SysMemPitch = w * 4;

    ID3D11Texture2D* tex = nullptr;
    HRESULT hr = g_pd3dDevice->CreateTexture2D(&desc, &initData, &tex);

    LibRaw::dcraw_clear_mem(image);
    RawProcessor.recycle();

    if (FAILED(hr) || !tex) return nullptr;

    ID3D11ShaderResourceView* srv = nullptr;
    hr = g_pd3dDevice->CreateShaderResourceView(tex, nullptr, &srv);
    tex->Release();
    if (FAILED(hr) || !srv) return nullptr;

    if (outW) *outW = w;
    if (outH) *outH = h;
    return srv;
}
#else
// LibRaw가 정의되어 있지 않으면 빈 스텁을 제공합니다.
ID3D11ShaderResourceView* LoadRawPreviewToSRV(const std::string& /*filename*/, int* /*outW*/, int* /*outH*/) {
    // LibRaw 미통합: 현재는 nullptr 반환 (caller는 다른 폴백을 시도해야 함)
    return nullptr;
}
#endif

class TextureCache {
public:
    TextureCache(size_t maxEntries = 64) : m_maxEntries(maxEntries), m_counter(0) {}
    ~TextureCache() { Clear(); }
    ID3D11ShaderResourceView* Get(const std::string& key, int& outW, int& outH) {
        std::lock_guard<std::mutex> lk(m_mutex);
        outW = outH = 0;
        auto it = m_map.find(key);
        if (it == m_map.end()) return nullptr;
        it->second.lastUsed = ++m_counter;
        outW = it->second.width; outH = it->second.height;
        it->second.srv->AddRef();
        return it->second.srv;
    }

    void Put(const std::string& key, ID3D11ShaderResourceView* srv, int w, int h) {
        if (!srv) return;
        std::lock_guard<std::mutex> lk(m_mutex);
        if (m_map.find(key) != m_map.end()) return; // 이미 있음
        if (m_map.size() >= m_maxEntries) EvictOne();
        srv->AddRef();
        Entry e; e.srv = srv; e.width = w; e.height = h; e.lastUsed = ++m_counter;
        m_map.emplace(key, e);
    }

    void Clear() {
        std::lock_guard<std::mutex> lk(m_mutex);
        for (auto& kv : m_map) {
            if (kv.second.srv) kv.second.srv->Release();
        }
        m_map.clear();
    }

private:
    struct Entry { ID3D11ShaderResourceView* srv = nullptr; int width = 0; int height = 0; uint64_t lastUsed = 0; };
    void EvictOne() {
        // 가장 오래 사용되지 않은 항목을 찾아 제거
        uint64_t minUsed = UINT64_MAX; std::string keyToEvict;
        for (auto& kv : m_map) {
            if (kv.second.lastUsed < minUsed) { minUsed = kv.second.lastUsed; keyToEvict = kv.first; }
        }
        if (!keyToEvict.empty()) {
            auto it = m_map.find(keyToEvict);
            if (it != m_map.end()) {
                if (it->second.srv) it->second.srv->Release();
                m_map.erase(it);
            }
        }
    }

    std::unordered_map<std::string, Entry> m_map;
    size_t m_maxEntries;
    uint64_t m_counter;
    std::mutex m_mutex;
};

static TextureCache g_textureCache(128);

// --- 비동기 디코딩 / 업로드 큐 ---
struct DecodeRequest { fs::path path; std::string key; int orientation = 1; int requestId = 0; };
struct UploadItem {
    std::string key;
    std::vector<unsigned char> rgba;
    int width = 0;
    int height = 0;
    int requestId = 0;
    bool decoded = false;
    bool hasAF = false;
    float afX = 0.5f;
    float afY = 0.5f;
    float afW = 0.05f;
    float afH = 0.05f;
};

static std::deque<DecodeRequest> g_decodeQueue;
static std::mutex g_decodeMutex;
static std::condition_variable g_decodeCv;
static std::deque<UploadItem> g_uploadQueue;
static std::mutex g_uploadMutex;
static std::vector<std::thread> g_decodeThreads;
static std::atomic<bool> g_decodeThreadRunning{ false };
static std::atomic<int> g_nextRequestId{ 0 };
static std::atomic<int> g_latestRequestId{ 0 };
static std::atomic<int> g_lastCompletedRequestId{ 0 };
// Cancellation token per request id. When non-zero and != current req id -> worker should abort current decode.
static std::atomic<int> g_cancelForRequestId{ 0 };
static std::atomic<int> g_initialPreloadRequestId{ 0 };
static std::atomic<int> g_initialPreloadTotal{ 0 };
static std::atomic<int> g_initialPreloadCompleted{ 0 };
//  원본 해상도 로딩 옵션을 실시간으로 스레드에 전달할 전역 플래그
static std::atomic<bool> g_loadFullResolution{ false };

// 큐 사이즈 조회 helpers
size_t GetDecodeQueueSize() { std::lock_guard<std::mutex> lk(g_decodeMutex); return g_decodeQueue.size(); }
size_t GetUploadQueueSize() { std::lock_guard<std::mutex> lk(g_uploadMutex); return g_uploadQueue.size(); }

// Export (비동기) 상태
static std::thread g_exportThread;
static std::atomic<bool> g_exportRunning{ false };
static std::atomic<bool> g_exportCancelFlag{ false };
static std::atomic<int> g_exportProgress{ 0 };
static std::atomic<int> g_exportTotal{ 0 };
static std::vector<std::string> g_exportLogs;
static std::mutex g_exportMutex;

// forward-declare helper
static std::string WideStringToUtf8(const std::wstring& w);

void StartExport(const std::wstring& destFolder, const std::vector<std::wstring>& files) {
    if (g_exportRunning.load()) return;
    if (g_exportThread.joinable()) g_exportThread.join();
    g_exportRunning = true;
    g_exportProgress = 0; g_exportTotal = (int)files.size();
    g_exportCancelFlag = false;
    {
        std::lock_guard<std::mutex> lk(g_exportMutex);
        g_exportLogs.emplace_back("[Info] 내보내기를 시작합니다.");
    }
    g_exportThread = std::thread([destFolder, files]() {
        try {
            std::filesystem::path exportDir = std::filesystem::path(destFolder);
            std::error_code ec;
            int idx = 0;
            for (const auto& src : files) {
                if (g_exportCancelFlag.load()) {
                    std::lock_guard<std::mutex> lk(g_exportMutex);
                    g_exportLogs.emplace_back("[Canceled] 사용자가 내보내기를 취소했습니다.");
                    break;
                }
                std::filesystem::path srcp = std::filesystem::path(src);
                std::wstring wname = srcp.filename().wstring();
                std::string name = WideStringToUtf8(wname);
                std::filesystem::path dest = exportDir / wname;
                // 충돌 처리: 파일명_1.ext
                int suffix = 1; std::error_code ec2;
                while (std::filesystem::exists(dest, ec2)) {
                    std::wstring stem = srcp.stem().wstring();
                    std::wstring ext = srcp.extension().wstring();
                    dest = exportDir / (stem + L"_" + std::to_wstring(suffix) + ext);
                    suffix++;
                }
                std::filesystem::copy_file(srcp, dest, std::filesystem::copy_options::overwrite_existing, ec);
                {
                    std::lock_guard<std::mutex> lk(g_exportMutex);
                    if (ec) g_exportLogs.emplace_back(std::string("[Fail] ") + name + ": " + ec.message());
                    else g_exportLogs.emplace_back(std::string("[OK] ") + name);
                }
                idx++; g_exportProgress = idx;
            }
            if (!g_exportCancelFlag.load()) {
                std::lock_guard<std::mutex> lk(g_exportMutex);
                g_exportLogs.emplace_back("[Info] 내보내기가 완료되었습니다.");
            }
        }
        catch (const std::exception& ex) {
            std::lock_guard<std::mutex> lk(g_exportMutex);
            g_exportLogs.emplace_back(std::string("[Fail] Export error: ") + ex.what());
        }
        g_exportRunning = false;
        });
}

// 디코딩 함수: 파일을 RGBA 메모리로 디코딩 (LibRaw 사용 가능)
bool DecodeImageToRGBA(const fs::path& path, std::vector<unsigned char>& outRGBA, int& outW, int& outH, int& orientation, int reqId) {
    outW = outH = 0; outRGBA.clear();
    constexpr int MAX_DIMENSION = 2048;
    auto t0 = std::chrono::high_resolution_clock::now();

    IWICImagingFactory* factory = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(hr) || !factory) return false;

    auto decodeToRGBA = [&](IWICBitmapDecoder* decoder) -> bool {
        if (!decoder) return false;
        IWICBitmapFrameDecode* frame = nullptr;
        HRESULT decodeHr = decoder->GetFrame(0, &frame);
        if (FAILED(decodeHr) || !frame) return false;

        UINT sourceWidth = 0, sourceHeight = 0;
        decodeHr = frame->GetSize(&sourceWidth, &sourceHeight);
        if (FAILED(decodeHr) || sourceWidth == 0 || sourceHeight == 0) {
            frame->Release();
            return false;
        }

        UINT targetWidth = sourceWidth;
        UINT targetHeight = sourceHeight;

        //  원본 화소로 불러오기(체크박스)가 꺼져 있을 때만(false) 이미지를 2048px로 최적화 축소합니다.
        if (!g_loadFullResolution.load()) {
            const UINT maxSourceDimension = (std::max)(sourceWidth, sourceHeight);
            if (maxSourceDimension > MAX_DIMENSION) {
                const double scale = static_cast<double>(MAX_DIMENSION) / maxSourceDimension;
                targetWidth = (std::max)(1U, static_cast<UINT>(std::lround(sourceWidth * scale)));
                targetHeight = (std::max)(1U, static_cast<UINT>(std::lround(sourceHeight * scale)));
            }
        }
        IWICBitmapScaler* scaler = nullptr;
        IWICBitmapSource* source = frame;
        if (targetWidth != sourceWidth || targetHeight != sourceHeight) {
            decodeHr = factory->CreateBitmapScaler(&scaler);
            if (SUCCEEDED(decodeHr) && scaler) {
                decodeHr = scaler->Initialize(frame, targetWidth, targetHeight, WICBitmapInterpolationModeFant);
            }
            if (FAILED(decodeHr) || !scaler) {
                if (scaler) scaler->Release();
                frame->Release();
                return false;
            }
            source = scaler;
        }

        IWICFormatConverter* converter = nullptr;
        decodeHr = factory->CreateFormatConverter(&converter);
        if (SUCCEEDED(decodeHr) && converter) {
            decodeHr = converter->Initialize(source, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
        }
        if (FAILED(decodeHr) || !converter) {
            if (converter) converter->Release();
            if (scaler) scaler->Release();
            frame->Release();
            return false;
        }

        UINT finalWidth = 0, finalHeight = 0;
        decodeHr = converter->GetSize(&finalWidth, &finalHeight);
        if (SUCCEEDED(decodeHr) && finalWidth > 0 && finalHeight > 0) {
            const size_t pixelBytes = static_cast<size_t>(finalWidth) * finalHeight * 4;
            if (pixelBytes <= (std::numeric_limits<UINT>::max)() && finalWidth <= (std::numeric_limits<UINT>::max)() / 4) {
                outRGBA.resize(pixelBytes);
                decodeHr = converter->CopyPixels(nullptr, finalWidth * 4, static_cast<UINT>(pixelBytes), outRGBA.data());
                if (SUCCEEDED(decodeHr)) {
                    outW = static_cast<int>(finalWidth);
                    outH = static_cast<int>(finalHeight);
                }
            }
            else {
                decodeHr = E_OUTOFMEMORY;
            }
        }
        else if (SUCCEEDED(decodeHr)) {
            decodeHr = E_FAIL;
        }

        converter->Release();
        if (scaler) scaler->Release();
        frame->Release();
        if (FAILED(decodeHr)) outRGBA.clear();
        return SUCCEEDED(decodeHr);
        };

    if (isRawFile(path)) {
#ifdef USE_LIBRAW
        LibRaw RawProcessor;
        if (g_cancelForRequestId.load() != 0 && g_cancelForRequestId.load() != reqId) { factory->Release(); return false; }
        const std::wstring widePath = path.wstring();
        if (RawProcessor.open_file(widePath.c_str()) != LIBRAW_SUCCESS) { RawProcessor.recycle(); factory->Release(); return false; }
        const int flip = RawProcessor.imgdata.sizes.flip;
        switch (flip) {
        case 3: orientation = 3; break;
        case 5: orientation = 8; break;
        case 6: orientation = 6; break;
        case 0: orientation = 1; break;
        default: break;
        }
        if (g_cancelForRequestId.load() != 0 && g_cancelForRequestId.load() != reqId) { RawProcessor.recycle(); factory->Release(); return false; }
        if (RawProcessor.unpack_thumb() != LIBRAW_SUCCESS) { RawProcessor.recycle(); factory->Release(); return false; }
        if (g_cancelForRequestId.load() != 0 && g_cancelForRequestId.load() != reqId) { RawProcessor.recycle(); factory->Release(); return false; }
        int ret = 0;
        libraw_processed_image_t* thumb = RawProcessor.dcraw_make_mem_thumb(&ret);
        if (!thumb || !thumb->data || thumb->data_size == 0 || thumb->data_size > (size_t)(std::numeric_limits<DWORD>::max)()) {
            if (thumb) LibRaw::dcraw_clear_mem(thumb);
            RawProcessor.recycle();
            factory->Release();
            return false;
        }
        if (g_cancelForRequestId.load() != 0 && g_cancelForRequestId.load() != reqId) {
            LibRaw::dcraw_clear_mem(thumb);
            RawProcessor.recycle();
            factory->Release();
            return false;
        }

        IWICStream* stream = nullptr;
        IWICBitmapDecoder* decoder = nullptr;
        hr = factory->CreateStream(&stream);
        if (SUCCEEDED(hr) && stream) {
            hr = stream->InitializeFromMemory(thumb->data, static_cast<DWORD>(thumb->data_size));
        }
        if (SUCCEEDED(hr) && stream) {
            hr = factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnLoad, &decoder);
        }
        const bool decoded = SUCCEEDED(hr) && decoder && decodeToRGBA(decoder);
        if (decoder) decoder->Release();
        if (stream) stream->Release();
        LibRaw::dcraw_clear_mem(thumb);
        RawProcessor.recycle();
        factory->Release();
        if (!decoded) return false;
        if (g_cancelForRequestId.load() != 0 && g_cancelForRequestId.load() != reqId) {
            outRGBA.clear();
            return false;
        }
        auto t1 = std::chrono::high_resolution_clock::now();
        g_decodeTotalNs.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        g_decodeCount.fetch_add(1);
        return true;
#else
        factory->Release();
        return false;
#endif
    }
    if (g_cancelForRequestId.load() != 0 && g_cancelForRequestId.load() != reqId) { factory->Release(); return false; }
    IWICBitmapDecoder* decoder = nullptr;
    std::wstring wpath = path.wstring();
    hr = factory->CreateDecoderFromFilename(wpath.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder);
    const bool decoded = SUCCEEDED(hr) && decoder && decodeToRGBA(decoder);
    if (decoder) decoder->Release();
    factory->Release();
    if (!decoded) return false;

    auto t1 = std::chrono::high_resolution_clock::now();
    auto durns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    g_decodeTotalNs.fetch_add(durns);
    g_decodeCount.fetch_add(1);

    return true;
}

//  GetExifData 함수 전방 선언 (컴파일러에게 미리 존재를 알림)
std::string GetExifData(const std::wstring& filePath, time_t& outUnixTime, int& outOrientation);

// 디코더 스레드 함수
void DecodeThreadFunc() {
    // Initialize COM for this worker thread for WIC
    HRESULT coHr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    // ... (이하 기존 코드 동일)
    g_decodeThreadRunning = true;
    while (g_decodeThreadRunning) {
        DecodeRequest req;
        {
            std::unique_lock<std::mutex> lk(g_decodeMutex);
            g_decodeCv.wait(lk, [] { return !g_decodeQueue.empty() || !g_decodeThreadRunning.load(); });
            if (!g_decodeThreadRunning && g_decodeQueue.empty()) break;
            req = g_decodeQueue.front(); g_decodeQueue.pop_front();
        }
        // 요청 취소 처리: 현재 최신 요청 아이디와 다르면 무시
        if (g_latestRequestId.load() != 0 && req.requestId != g_latestRequestId.load()) {
            continue; // 취소된 이전 요청
        }
        UploadItem item; item.key = req.key; item.requestId = req.requestId;

        //  지연 로딩: 이 스레드가 이미지를 화면에 올리기 직전에 진짜 EXIF 정보를 파싱합니다. (메인 화면 멈춤 방지)
        int currentOrientation = 1;
        time_t dummyTime = 0;
        GetExifData(req.path.wstring(), dummyTime, currentOrientation);

        bool ok = DecodeImageToRGBA(req.path, item.rgba, item.width, item.height, currentOrientation, req.requestId);
        if (ok) {
            if (isRawFile(req.path)) {
                item.hasAF = GetAFPointViaExifTool(req.path.wstring(), item.width, item.height, currentOrientation,
                    item.afX, item.afY, item.afW, item.afH);
            }
            RotateRGBABuffer(item.rgba, item.width, item.height, currentOrientation);
        }
        item.decoded = ok;
        {
            std::lock_guard<std::mutex> lk(g_uploadMutex);
            g_uploadQueue.push_back(std::move(item));
        }
    }
    if (SUCCEEDED(coHr)) CoUninitialize();
}

// 요청 생성 함수
int EnqueueDecodeRequest(const fs::path& path, const std::string& key, int orientation = 1) {
    int id = ++g_nextRequestId;
    {
        std::lock_guard<std::mutex> lk(g_decodeMutex);
        g_decodeQueue.push_back({ path, key, orientation, id });
    }
    g_decodeCv.notify_one();
    return id;
}

int EnqueueDecodeRequestExclusiveBatch(const std::vector<int>& indicesToLoad, const std::vector<ImageData>& images) {
    const int batchId = ++g_nextRequestId;
    {
        std::lock_guard<std::mutex> lk(g_decodeMutex);
        g_decodeQueue.clear();
        for (int index : indicesToLoad) {
            if (index < 0 || index >= (int)images.size()) continue;
            const ImageData& image = images[index];
            g_decodeQueue.push_back({ fs::path(image.wFilePath), image.filePath, image.orientation, batchId });
        }
        g_latestRequestId.store(batchId);
        g_cancelForRequestId.store(batchId);
    }
    {
        std::lock_guard<std::mutex> ulk(g_uploadMutex);
        g_uploadQueue.clear();
    }
    g_decodeCv.notify_one();
    return batchId;
}

// Exclusive: 큐를 비우고 해당 요청만 남겨 우선 처리하도록 함(빠른 스킵/취소 목적)
int EnqueueDecodeRequestExclusive(const fs::path& path, const std::string& key, int orientation = 1) {
    int id = ++g_nextRequestId;
    {
        std::lock_guard<std::mutex> lk(g_decodeMutex);
        g_decodeQueue.clear();
        g_decodeQueue.push_back({ path, key, orientation, id });
        g_latestRequestId.store(id);
        g_cancelForRequestId.store(id);
    }
    {
        // ★ 업로드 큐도 비웁니다: 빠른 스킵 후 랜덤 클릭 시 업로드 병목을 방지
        std::lock_guard<std::mutex> ulk(g_uploadMutex);
        g_uploadQueue.clear();
    }
    g_decodeCv.notify_one();
    return id;
}

// 메인 스레드에서 업로드 큐 처리: RGBA 버퍼 -> D3D SRV 생성 후 캐시에 저장
void ProcessUploadQueueAndIntegrate(const std::string& currentSelectedPath, std::vector<ImageData>& loadedImages) {
    std::deque<UploadItem> local;
    {
        std::lock_guard<std::mutex> lk(g_uploadMutex);
        if (g_uploadQueue.empty()) return;
        local.swap(g_uploadQueue);
    }
    for (auto& it : local) {
        if (it.hasAF) {
            auto imageIt = std::find_if(loadedImages.begin(), loadedImages.end(), [&](const ImageData& image) {
                return image.filePath == it.key;
                });
            if (imageIt != loadedImages.end()) {
                imageIt->hasAF = true;
                imageIt->afX = it.afX;
                imageIt->afY = it.afY;
                imageIt->afW = it.afW;
                imageIt->afH = it.afH;
            }
        }
        if (it.decoded && it.width > 0 && it.height > 0) {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = (UINT)it.width; desc.Height = (UINT)it.height; desc.MipLevels = 1; desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count = 1; desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA initData{}; initData.pSysMem = it.rgba.data(); initData.SysMemPitch = it.width * 4;
            ID3D11Texture2D* tex = nullptr; HRESULT hr = g_pd3dDevice->CreateTexture2D(&desc, &initData, &tex);
            // measure upload time (texture creation + SRV) and avoid creating duplicate texture if cache already contains key
            auto up0 = std::chrono::high_resolution_clock::now();
            int cachedW = 0, cachedH = 0;
            ID3D11ShaderResourceView* existing = g_textureCache.Get(it.key, cachedW, cachedH);
            if (existing) {
                // already cached; update current image SRV if needed
                existing->Release();
                if (!currentSelectedPath.empty() && currentSelectedPath == it.key && it.requestId == g_latestRequestId.load()) {
                    if (g_currentImageSRV) { g_currentImageSRV->Release(); }
                    g_currentImageSRV = existing; g_currentImageWidth = cachedW; g_currentImageHeight = cachedH;
                    g_currentImageSRV->AddRef();
                    g_lastCompletedRequestId.store(it.requestId);
                }
            }
            else {
                if (SUCCEEDED(hr) && tex) {
                    ID3D11ShaderResourceView* srv = nullptr; hr = g_pd3dDevice->CreateShaderResourceView(tex, nullptr, &srv);
                    tex->Release();
                    if (SUCCEEDED(hr) && srv) {
                        g_textureCache.Put(it.key, srv, it.width, it.height);
                        if (!currentSelectedPath.empty() && currentSelectedPath == it.key && it.requestId == g_latestRequestId.load()) {
                            if (g_currentImageSRV) { g_currentImageSRV->Release(); }
                            g_currentImageSRV = srv; g_currentImageWidth = it.width; g_currentImageHeight = it.height;
                            g_currentImageSRV->AddRef();
                            g_lastCompletedRequestId.store(it.requestId);
                        }
                        else {
                            srv->Release();
                        }
                    }
                }
            }
            auto up1 = std::chrono::high_resolution_clock::now();
            auto upns = std::chrono::duration_cast<std::chrono::nanoseconds>(up1 - up0).count();
            g_uploadTotalNs.fetch_add(upns);
            g_uploadCount.fetch_add(1);
        }
        else if (!currentSelectedPath.empty() && currentSelectedPath == it.key && it.requestId == g_latestRequestId.load()) {
            g_lastCompletedRequestId.store(it.requestId);
        }
        if (it.requestId == g_initialPreloadRequestId.load()) {
            g_initialPreloadCompleted.fetch_add(1);
        }
    }
}

time_t ParseTimeToUnix(const std::string& timeStr) {
    if (timeStr == "알 수 없음 (EXIF 없음)") return 0;
    std::tm tm = {};
    std::istringstream ss(timeStr);
    ss >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");
    if (ss.fail()) return 0;
    return std::mktime(&tm);
}

std::string GetExifData(const std::wstring& filePath, time_t& outUnixTime, int& outOrientation) {
    std::string dateTaken = "알 수 없음 (EXIF 없음)";
    outUnixTime = 0;
    outOrientation = 1;
    IShellItem2* pItem2 = nullptr;
    HRESULT hr = SHCreateItemFromParsingName(filePath.c_str(), NULL, IID_PPV_ARGS(&pItem2));
    if (SUCCEEDED(hr)) {
        PROPVARIANT propVar;
        PropVariantInit(&propVar);
        hr = pItem2->GetProperty(PKEY_Photo_DateTaken, &propVar);
        if (SUCCEEDED(hr) && propVar.vt == VT_FILETIME) {
            SYSTEMTIME st;
            FileTimeToSystemTime(&propVar.filetime, &st);
            char buffer[256];
            sprintf_s(buffer, "%04d-%02d-%02d %02d:%02d:%02d",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
            dateTaken = buffer;
            outUnixTime = ParseTimeToUnix(dateTaken);
        }
        PropVariantClear(&propVar);

        PropVariantInit(&propVar);
        hr = pItem2->GetProperty(PKEY_Photo_Orientation, &propVar);
        if (SUCCEEDED(hr)) {
            if (propVar.vt == VT_UI2) outOrientation = propVar.uiVal;
            else if (propVar.vt == VT_UI4) outOrientation = (int)propVar.ulVal;
            else if (propVar.vt == VT_I2) outOrientation = propVar.iVal;
            else if (propVar.vt == VT_I4) outOrientation = propVar.lVal;
        }
        PropVariantClear(&propVar);
        pItem2->Release();
    }
    return dateTaken;
}

std::string SelectFolderDialog() {
    std::string selectedPath = "";
    IFileOpenDialog* pFileOpen;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_ALL, IID_IFileOpenDialog, reinterpret_cast<void**>(&pFileOpen));
    if (SUCCEEDED(hr)) {
        DWORD dwOptions;
        pFileOpen->GetOptions(&dwOptions);
        pFileOpen->SetOptions(dwOptions | FOS_PICKFOLDERS);
        if (SUCCEEDED(pFileOpen->Show(NULL))) {
            IShellItem* pItem;
            if (SUCCEEDED(pFileOpen->GetResult(&pItem))) {
                PWSTR pszFilePath;
                if (SUCCEEDED(pItem->GetDisplayName(SIGDN_FILESYSPATH, &pszFilePath))) {
                    // 안전한 와이드 -> UTF-8 변환
                    int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, pszFilePath, -1, NULL, 0, NULL, NULL);
                    if (sizeNeeded > 0) {
                        std::string utf8Path(sizeNeeded - 1, '\0');
                        WideCharToMultiByte(CP_UTF8, 0, pszFilePath, -1, &utf8Path[0], sizeNeeded, NULL, NULL);
                        selectedPath = utf8Path;
                    }
                    else {
                        selectedPath.clear();
                    }
                    CoTaskMemFree(pszFilePath);
                }
                pItem->Release();
            }
        }
        pFileOpen->Release();
    }
    return selectedPath;
}

// Wide-character version of folder dialog
std::wstring SelectFolderDialogW() {
    std::wstring selectedPath = L"";
    IFileOpenDialog* pFileOpen;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_ALL, IID_IFileOpenDialog, reinterpret_cast<void**>(&pFileOpen));
    if (SUCCEEDED(hr)) {
        DWORD dwOptions;
        pFileOpen->GetOptions(&dwOptions);
        pFileOpen->SetOptions(dwOptions | FOS_PICKFOLDERS);
        if (SUCCEEDED(pFileOpen->Show(NULL))) {
            IShellItem* pItem;
            if (SUCCEEDED(pFileOpen->GetResult(&pItem))) {
                PWSTR pszFilePath;
                if (SUCCEEDED(pItem->GetDisplayName(SIGDN_FILESYSPATH, &pszFilePath))) {
                    selectedPath = pszFilePath;
                    CoTaskMemFree(pszFilePath);
                }
                pItem->Release();
            }
        }
        pFileOpen->Release();
    }
    return selectedPath;
}

static std::string WideStringToUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), NULL, 0, NULL, NULL);
    if (sizeNeeded <= 0) return std::string();
    std::string s(sizeNeeded, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], sizeNeeded, NULL, NULL);
    // WideCharToMultiByte does not add null when length provided; ensure proper sizing
    return std::string(s.c_str(), s.size());
}

// ---  DirectX 11 기본 공사 (Boilerplate) ---
static ID3D11Device* g_pd3dDevice = nullptr;
static ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
static IDXGISwapChain* g_pSwapChain = nullptr;
static ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;

void CreateRenderTarget() {
    ID3D11Texture2D* pBackBuffer;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
    pBackBuffer->Release();
}
void CleanupRenderTarget() {
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}
bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0, };
    HRESULT res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res != S_OK) return false;
    CreateRenderTarget();
    return true;
}
void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

// --- 이미지 로드 및 DirectX 텍스처 생성 유틸리티 ---
static ID3D11ShaderResourceView* g_currentImageSRV = nullptr;
static int g_currentImageWidth = 0;
static int g_currentImageHeight = 0;

void ReleaseCurrentImage() {
    if (g_currentImageSRV) { g_currentImageSRV->Release(); g_currentImageSRV = nullptr; }
    g_currentImageWidth = g_currentImageHeight = 0;
}

// 파일에서 이미지를 읽어 GPU 텍스처로 업로드합니다. 실패하면 nullptr 반환.
ID3D11ShaderResourceView* LoadTextureFromFileToSRV(const std::string& filename, int* outWidth, int* outHeight) {
    if (outWidth) *outWidth = 0;
    if (outHeight) *outHeight = 0;

    int w = 0, h = 0, channels = 0;
    // UTF-8/wide-path safe load: read file bytes then stbi_load_from_memory
    std::vector<unsigned char> filebuf;
    fs::path p(filename);
    if (!ReadFileToVector(p, filebuf)) return nullptr;
    unsigned char* data = stbi_load_from_memory(filebuf.data(), (int)filebuf.size(), &w, &h, &channels, 4);
    if (!data) return nullptr;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = (UINT)w;
    desc.Height = (UINT)h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = 0;

    D3D11_SUBRESOURCE_DATA initData{};
    initData.pSysMem = data;
    initData.SysMemPitch = w * 4;

    ID3D11Texture2D* tex = nullptr;
    HRESULT hr = g_pd3dDevice->CreateTexture2D(&desc, &initData, &tex);
    stbi_image_free(data);
    if (FAILED(hr) || !tex) return nullptr;

    ID3D11ShaderResourceView* srv = nullptr;
    hr = g_pd3dDevice->CreateShaderResourceView(tex, nullptr, &srv);
    tex->Release();
    if (FAILED(hr) || !srv) return nullptr;

    if (outWidth) *outWidth = w;
    if (outHeight) *outHeight = h;
    return srv;
}

// 윈도우 이벤트 처리기
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;
    switch (msg) {
    case WM_SIZE:
        if (g_pd3dDevice != nullptr && wParam != SIZE_MINIMIZED) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
            CreateRenderTarget();
        }
        return 0;
        //  창 최소 크기 제한 (WM_GETMINMAXINFO 추가)
    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = (MINMAXINFO*)lParam;
        mmi->ptMinTrackSize.x = 1024; // 최소 가로 픽셀
        mmi->ptMinTrackSize.y = 768;  // 최소 세로 픽셀
        return 0;
    }
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// --- 글래스모피즘(Glassmorphism) 스타일 적용 함수 ---
void ApplyGlassmorphismStyle() {
    ImGuiStyle& style = ImGui::GetStyle();

    // 1. 모서리를 둥글게 (부드럽게 가공된 유리 패널 느낌)
    style.WindowRounding = 12.0f;
    style.ChildRounding = 12.0f;
    style.FrameRounding = 8.0f;
    style.PopupRounding = 12.0f;
    style.GrabRounding = 8.0f;
    style.ScrollbarRounding = 12.0f;

    // 2. 테두리를 얇게 설정 (유리의 반사광 테두리 느낌)
    style.WindowBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.ChildBorderSize = 1.0f;

    // 3. 반투명(Alpha)이 가미된 색상 조합
    ImVec4 glassBackground = ImVec4(0.12f, 0.14f, 0.18f, 0.65f); // 반투명한 어두운 유리 베이스
    ImVec4 glassPanel = ImVec4(0.15f, 0.18f, 0.22f, 0.50f); // 더 투명한 자식 패널(왼쪽 리스트 등)
    ImVec4 glassBorder = ImVec4(1.00f, 1.00f, 1.00f, 0.15f); // 얇고 하얀 테두리 (빛 반사)
    ImVec4 glassButton = ImVec4(1.00f, 1.00f, 1.00f, 0.10f); // 반투명 버튼
    ImVec4 glassButtonHovered = ImVec4(1.00f, 1.00f, 1.00f, 0.20f); // 마우스 오버 시 밝아짐
    ImVec4 glassButtonActive = ImVec4(1.00f, 1.00f, 1.00f, 0.30f); // 클릭 시 더 밝아짐
    ImVec4 textWhite = ImVec4(0.95f, 0.95f, 0.95f, 1.00f);

    // 색상 적용
    style.Colors[ImGuiCol_Text] = textWhite;
    style.Colors[ImGuiCol_WindowBg] = glassBackground;
    style.Colors[ImGuiCol_ChildBg] = glassPanel;
    style.Colors[ImGuiCol_PopupBg] = glassBackground;
    style.Colors[ImGuiCol_Border] = glassBorder;

    style.Colors[ImGuiCol_FrameBg] = glassButton;
    style.Colors[ImGuiCol_FrameBgHovered] = glassButtonHovered;
    style.Colors[ImGuiCol_FrameBgActive] = glassButtonActive;

    style.Colors[ImGuiCol_Button] = glassButton;
    style.Colors[ImGuiCol_ButtonHovered] = glassButtonHovered;
    style.Colors[ImGuiCol_ButtonActive] = glassButtonActive;

    // 리스트 선택 및 헤더
    style.Colors[ImGuiCol_Header] = glassButtonHovered;
    style.Colors[ImGuiCol_HeaderHovered] = glassButtonActive;
    style.Colors[ImGuiCol_HeaderActive] = ImVec4(1.0f, 1.0f, 1.0f, 0.40f);

    // 스크롤바 반투명
    style.Colors[ImGuiCol_ScrollbarBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.10f);
    style.Colors[ImGuiCol_ScrollbarGrab] = glassButtonHovered;
    style.Colors[ImGuiCol_ScrollbarGrabHovered] = glassButtonActive;
    style.Colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(1.0f, 1.0f, 1.0f, 0.40f);
}
// =====================================================================
// 필터링, 정렬 및 백그라운드 메타데이터 엔진
// =====================================================================
#include <set>
static std::vector<ImageData> g_allImages; // 전체 원본 데이터 백업
static std::mutex g_dataMutex;

// UI 상태 변수들
static bool g_enableGrouping = true;
static int g_sortMode = 0; // 0:시간, 1:ISO, 2:F값, 3:셔터스피드, 4:화각
static std::string g_filterExt = "All";
static std::string g_filterCamera = "All";
static std::string g_filterLens = "All";

// 드롭다운 메뉴용 리스트
static std::vector<std::string> g_extList = { "All" };
static std::vector<std::string> g_cameraList = { "All" };
static std::vector<std::string> g_lensList = { "All" };

// 필터 및 정렬 적용 함수
void ApplyFiltersAndSort(std::vector<ImageData>& loadedImages, int& totalGroups) {
    std::lock_guard<std::mutex> lock(g_dataMutex);
    loadedImages.clear();

    // 1. 필터 적용
    for (const auto& img : g_allImages) {
        if (g_filterExt != "All" && img.extension != g_filterExt) continue;
        if (g_filterCamera != "All" && img.cameraModel != g_filterCamera && !img.cameraModel.empty()) continue;
        if (g_filterLens != "All" && img.lensModel != g_filterLens && !img.lensModel.empty()) continue;
        loadedImages.push_back(img);
    }

    // 2. 정렬 적용
    if (g_sortMode == 0) {
        std::sort(loadedImages.begin(), loadedImages.end(), [](const ImageData& a, const ImageData& b) { return a.captureTimeT < b.captureTimeT; });
    }
    else if (g_sortMode == 1) {
        std::sort(loadedImages.begin(), loadedImages.end(), [](const ImageData& a, const ImageData& b) { return a.iso < b.iso; });
    }
    else if (g_sortMode == 2) {
        std::sort(loadedImages.begin(), loadedImages.end(), [](const ImageData& a, const ImageData& b) { return a.fNumber < b.fNumber; });
    }
    else if (g_sortMode == 3) {
        std::sort(loadedImages.begin(), loadedImages.end(), [](const ImageData& a, const ImageData& b) { return a.exposureTime < b.exposureTime; });
    }
    else if (g_sortMode == 4) {
        std::sort(loadedImages.begin(), loadedImages.end(), [](const ImageData& a, const ImageData& b) { return a.focalLength < b.focalLength; });
    }

    // 3. 그룹화 적용 (시간순 정렬일 때만, 그리고 토글이 켜져 있을 때만 작동)
    totalGroups = 1;
    if (!loadedImages.empty()) {
        loadedImages[0].groupID = totalGroups;
        for (size_t i = 1; i < loadedImages.size(); ++i) {
            if (g_enableGrouping && g_sortMode == 0) {
                if (loadedImages[i].captureTimeT == 0 || loadedImages[i - 1].captureTimeT == 0 ||
                    std::difftime(loadedImages[i].captureTimeT, loadedImages[i - 1].captureTimeT) > 2.0) {
                    ++totalGroups;
                }
            }
            loadedImages[i].groupID = totalGroups;
        }
    }
}

// 윈도우 OS API 및 LibRaw를 이용한 하이브리드 EXIF 추출 엔진
void LoadExtendedExif(ImageData& img) {
    bool isRaw = isRawFile(fs::path(img.wFilePath));

    // 1. RAW 파일인 경우: 윈도우 API가 렌즈 정보를 누락하므로 LibRaw를 최우선으로 사용하여 추출
    if (isRaw) {
#ifdef USE_LIBRAW
        LibRaw RawProcessor;
        // open_file은 무거운 픽셀 데이터를 건드리지 않고 헤더(EXIF)만 즉시 파싱합니다.
        if (RawProcessor.open_file(img.wFilePath.c_str()) == LIBRAW_SUCCESS) {
            img.iso = (int)RawProcessor.imgdata.other.iso_speed;
            img.fNumber = RawProcessor.imgdata.other.aperture;
            img.focalLength = RawProcessor.imgdata.other.focal_len;
            img.exposureTime = RawProcessor.imgdata.other.shutter;

            if (img.exposureTime >= 1.0) {
                char buf[32]; snprintf(buf, sizeof(buf), "%.1fs", img.exposureTime); img.exposureTimeStr = buf;
            }
            else if (img.exposureTime > 0.0) {
                char buf[32]; snprintf(buf, sizeof(buf), "1/%ds", (int)std::round(1.0 / img.exposureTime)); img.exposureTimeStr = buf;
            }

            // 카메라 및 렌즈 모델 문자열 복사
            if (strlen(RawProcessor.imgdata.idata.model) > 0) img.cameraModel = RawProcessor.imgdata.idata.model;
            if (strlen(RawProcessor.imgdata.lens.Lens) > 0) img.lensModel = RawProcessor.imgdata.lens.Lens;

            RawProcessor.recycle();
        }
#endif
    }

    // 2. 윈도우 기본 API 
    // (JPG 파일이거나, LibRaw가 렌즈/카메라 정보를 찾지 못해 비어있을 경우에만 빈칸 땜빵용으로 실행)
    if (!isRaw || img.lensModel.empty() || img.cameraModel.empty()) {
        IShellItem2* pItem2 = nullptr;
        HRESULT hr = SHCreateItemFromParsingName(img.wFilePath.c_str(), NULL, IID_PPV_ARGS(&pItem2));
        if (SUCCEEDED(hr)) {
            PROPVARIANT propVar;

            if (img.iso == 0) {
                PropVariantInit(&propVar);
                if (SUCCEEDED(pItem2->GetProperty(PKEY_Photo_ISOSpeed, &propVar)) && propVar.vt == VT_UI2) img.iso = propVar.uiVal;
                PropVariantClear(&propVar);
            }
            if (img.fNumber == 0.0) {
                PropVariantInit(&propVar);
                if (SUCCEEDED(pItem2->GetProperty(PKEY_Photo_FNumber, &propVar)) && propVar.vt == VT_R8) img.fNumber = propVar.dblVal;
                PropVariantClear(&propVar);
            }
            if (img.focalLength == 0.0) {
                PropVariantInit(&propVar);
                if (SUCCEEDED(pItem2->GetProperty(PKEY_Photo_FocalLength, &propVar)) && propVar.vt == VT_R8) img.focalLength = propVar.dblVal;
                PropVariantClear(&propVar);
            }
            if (img.exposureTime == 0.0) {
                PropVariantInit(&propVar);
                if (SUCCEEDED(pItem2->GetProperty(PKEY_Photo_ExposureTime, &propVar)) && propVar.vt == VT_R8) {
                    img.exposureTime = propVar.dblVal;
                    if (img.exposureTime >= 1.0) { char buf[32]; snprintf(buf, sizeof(buf), "%.1fs", img.exposureTime); img.exposureTimeStr = buf; }
                    else if (img.exposureTime > 0) { char buf[32]; snprintf(buf, sizeof(buf), "1/%ds", (int)std::round(1.0 / img.exposureTime)); img.exposureTimeStr = buf; }
                }
                PropVariantClear(&propVar);
            }
            if (img.cameraModel.empty()) {
                PropVariantInit(&propVar);
                if (SUCCEEDED(pItem2->GetProperty(PKEY_Photo_CameraModel, &propVar)) && propVar.vt == VT_LPWSTR) img.cameraModel = WideStringToUtf8(propVar.pwszVal);
                PropVariantClear(&propVar);
            }
            if (img.lensModel.empty()) {
                PropVariantInit(&propVar);
                if (SUCCEEDED(pItem2->GetProperty(PKEY_Photo_LensModel, &propVar)) && propVar.vt == VT_LPWSTR) img.lensModel = WideStringToUtf8(propVar.pwszVal);
                PropVariantClear(&propVar);
            }

            pItem2->Release();
        }
    }

    img.exifLoaded = true;
}

// 백그라운드에서 EXIF를 조용히 수집하는 스레드
static std::thread g_exifThread;
static std::atomic<bool> g_exifTaskRunning{ false };
static std::mutex g_exifPriorityMutex;         // 🌟 추가: 우선순위 락
static std::wstring g_priorityExifWPath = L""; // 🌟 추가: 최우선으로 읽어올 파일 경로

void RunBackgroundExifScanner() {
    if (g_exifThread.joinable()) { g_exifTaskRunning = false; g_exifThread.join(); }
    g_exifTaskRunning = true;
    g_exifThread = std::thread([]() {
        CoInitializeEx(NULL, COINIT_MULTITHREADED);
        std::set<std::string> extSet, camSet, lensSet;

        bool allLoaded = false;
        while (g_exifTaskRunning && !allLoaded) {
            // 1. 가장 먼저 선택된 사진(우선순위)이 있는지 확인하고 처리
            std::wstring priorityPath;
            {
                std::lock_guard<std::mutex> lock(g_exifPriorityMutex);
                priorityPath = g_priorityExifWPath;
            }

            if (!priorityPath.empty()) {
                for (auto& img : g_allImages) {
                    if (img.wFilePath == priorityPath && !img.exifLoaded) {
                        LoadExtendedExif(img);
                        break;
                    }
                }
            }

            // 2. 순차적으로 스캔 진행
            bool loadedOne = false;
            for (size_t i = 0; i < g_allImages.size(); ++i) {
                if (!g_exifTaskRunning) break;

                // 순차 스캔 중 사용자가 다른 사진을 클릭하면 즉시 루프를 멈추고 새 사진으로 달려감
                {
                    std::lock_guard<std::mutex> lock(g_exifPriorityMutex);
                    if (g_priorityExifWPath != priorityPath && !g_priorityExifWPath.empty()) {
                        loadedOne = true;
                        break;
                    }
                }

                if (!g_allImages[i].exifLoaded) {
                    LoadExtendedExif(g_allImages[i]);

                    extSet.insert(g_allImages[i].extension);
                    if (!g_allImages[i].cameraModel.empty()) camSet.insert(g_allImages[i].cameraModel);
                    if (!g_allImages[i].lensModel.empty()) lensSet.insert(g_allImages[i].lensModel);

                    loadedOne = true;

                    // 화면 UI 갱신을 위해 50장마다 한 번씩 드롭다운 메뉴 업데이트
                    if (i % 50 == 0) {
                        std::lock_guard<std::mutex> lock(g_dataMutex);
                        g_extList = { "All" }; g_cameraList = { "All" }; g_lensList = { "All" };
                        for (const auto& e : extSet) g_extList.push_back(e);
                        for (const auto& c : camSet) g_cameraList.push_back(c);
                        for (const auto& l : lensSet) g_lensList.push_back(l);
                    }
                }
            }
            if (!loadedOne) allLoaded = true; // 더 이상 읽을 사진이 없으면 무한루프 종료
        }

        // 모든 스캔이 끝나면 최종 드롭다운 메뉴 100% 업데이트
        if (g_exifTaskRunning) {
            std::lock_guard<std::mutex> lock(g_dataMutex);
            g_extList = { "All" }; g_cameraList = { "All" }; g_lensList = { "All" };
            for (const auto& e : extSet) g_extList.push_back(e);
            for (const auto& c : camSet) g_cameraList.push_back(c);
            for (const auto& l : lensSet) g_lensList.push_back(l);
        }

        CoUninitialize();
        });
}
// =====================================================================

// --- 메인 함수 ---
int main() {
    std::system("chcp 65001 > nul");
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    // 1. 윈도우 그래픽 창 만들기
    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr, L"FastPICWindow", nullptr };
    ::RegisterClassExW(&wc);
    HWND hwnd = ::CreateWindowW(wc.lpszClassName, L"FastPIC", WS_OVERLAPPEDWINDOW, 100, 100, 1280, 800, nullptr, nullptr, wc.hInstance, nullptr);

    // 2. DirectX 11 켜기
    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D(); ::UnregisterClassW(wc.lpszClassName, wc.hInstance); return 1;
    }
    ::ShowWindow(hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);

    // 3. ImGui 환경 세팅
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;

    //  한글 폰트 로드 (윈도우 기본 맑은 고딕 사용)
    ImFont* font = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\malgun.ttf", 18.0f, nullptr, io.Fonts->GetGlyphRangesKorean());

    ApplyGlassmorphismStyle(); // 다크 글래스모피즘 스타일 적용
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    // 디코더 스레드 풀 시작: 하드웨어 코어 수에 맞춰 스레드 생성 (최소 4개)
    unsigned int hc = std::thread::hardware_concurrency();
    if (hc < 4) hc = 4;
    for (unsigned int i = 0; i < hc; ++i) {
        g_decodeThreads.emplace_back(DecodeThreadFunc);
    }

    //  UI용 상태 변수들
    std::string currentFolderPath = "선택된 폴더가 없습니다.";
    std::wstring wCurrentFolderPath;
    std::vector<ImageData> loadedImages;
    int totalGroups = 0;
    int selectedIndex = -1; // 선택된 리스트 인덱스 (메인 스코프에 선언)
    int selectedRequestId = 0; // 현재 선택된 항목에 대해 마지막으로 요청한 디코드 ID
    bool scrollToSelection = false; // 스크롤 이동 신호를 전달할 플래그
    float zoomLevel = 1.0f;
    ImVec2 panOffset(0.0f, 0.0f);
    static bool showAFPoint = false;
    static bool showHelpPopup = false;
    static bool showSupportPopup = false; // 🌟 추가: 후원 팝업 상태 및 QR 코드 텍스처 포인터
    int qrWidth = 0, qrHeight = 0;
    ID3D11ShaderResourceView* g_qrCodeSRV = LoadTextureFromFileToSRV("qr_code.png", &qrWidth, &qrHeight); // 프로그램 시작 시 qr_code.png 파일을 읽어와 GPU에 로드해 둡니다.
    bool initialPreloadActive = false;
    int initialPreloadBatchId = 0;
    int initialPreloadCount = 0;
    int initialPreloadDone = 0;
    bool showExportPopup = false;
    bool showHoldWarningPopup = false;
    int holdCount = 0;
    std::vector<std::wstring> pendingExportPicks;

    // 4. 메인 그래픽 렌더링 루프 (게임 엔진의 Tick 과 같습니다)
    bool done = false;
    while (!done) {
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            ::TranslateMessage(&msg); ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        // ImGui 프레임 시작
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // --- 키 입력 처리(네비게이션 및 라벨링) using ImGui repeat-aware APIs ---
        // 중앙화된 선택 처리 함수
        auto SelectIndex = [&](int newIndex) {
            if (loadedImages.empty()) return;
            if (newIndex < 0) newIndex = 0;
            if (newIndex >= (int)loadedImages.size()) newIndex = (int)loadedImages.size() - 1;
            if (newIndex == selectedIndex) return;
            selectedIndex = newIndex;
            //이미지 넘어갈때 줌 배율 위치값 초기화 변수인데 임시로 비활성화시킴
            //zoomLevel = 1.0f;
            //panOffset = ImVec2(0.0f, 0.0f);
            scrollToSelection = true; // 선택이 변경되었으므로 스크롤을 이동시키라는 신호 ON

            ReleaseCurrentImage();
            int w = 0, h = 0;
            auto& img = loadedImages[selectedIndex];
            ID3D11ShaderResourceView* cached = g_textureCache.Get(img.filePath, w, h);
            if (cached) {
                g_currentImageSRV = cached; g_currentImageWidth = w; g_currentImageHeight = h; selectedRequestId = 0;
            }

            // 🌟 1. EXIF 백그라운드 스레드에게 "이 사진부터 당장 읽어!" 라고 우선순위 지시
            {
                std::lock_guard<std::mutex> lock(g_exifPriorityMutex);
                g_priorityExifWPath = img.wFilePath;
            }

            // 🌟 2. 스마트 프리패칭 최적화 (병목 방지)
            std::vector<int> indicesToLoad;

            if (g_enableGrouping) {
                // 기존 로직: 그룹이 활성화되어 있을 때는 씬 전체를 로딩
                int currentGroup = img.groupID;
                auto itStart = std::lower_bound(loadedImages.begin(), loadedImages.end(), currentGroup,
                    [](const ImageData& item, int g) { return item.groupID < g; });
                auto itEnd = std::upper_bound(loadedImages.begin(), loadedImages.end(), currentGroup,
                    [](int g, const ImageData& item) { return g < item.groupID; });

                int startIdx = (int)std::distance(loadedImages.begin(), itStart);
                int endIdx = (int)std::distance(loadedImages.begin(), itEnd);

                if (!cached) indicesToLoad.push_back(selectedIndex);

                for (int index = startIdx; index < endIdx; ++index) {
                    if (index == selectedIndex) continue;
                    int cw = 0, ch = 0;
                    ID3D11ShaderResourceView* prefetched = g_textureCache.Get(loadedImages[index].filePath, cw, ch);
                    if (prefetched) prefetched->Release();
                    else indicesToLoad.push_back(index);
                }

                if (endIdx < (int)loadedImages.size()) {
                    int cw = 0, ch = 0;
                    ID3D11ShaderResourceView* prefetched = g_textureCache.Get(loadedImages[endIdx].filePath, cw, ch);
                    if (prefetched) prefetched->Release();
                    else indicesToLoad.push_back(endIdx);
                }
            }
            else {
                // 🌟 3. 그룹화 해제 시 최적화: 수천 장 동시 큐잉을 방지하고 앞뒤 5장만 미리 로딩
                if (!cached) indicesToLoad.push_back(selectedIndex);

                int startIdx = (std::max)(0, selectedIndex - 2);
                int endIdx = (std::min)((int)loadedImages.size(), selectedIndex + 6);

                for (int index = startIdx; index < endIdx; ++index) {
                    if (index == selectedIndex) continue;
                    int cw = 0, ch = 0;
                    ID3D11ShaderResourceView* prefetched = g_textureCache.Get(loadedImages[index].filePath, cw, ch);
                    if (prefetched) prefetched->Release();
                    else indicesToLoad.push_back(index);
                }
            }

            // 일꾼 스레드들에게 기존 작업을 멈추고 이 리스트를 최우선으로 처리하라고 명령!
            if (!indicesToLoad.empty()) {
                const int batchId = EnqueueDecodeRequestExclusiveBatch(indicesToLoad, loadedImages);
                selectedRequestId = cached ? 0 : batchId;
            }
            else if (!cached) {
                selectedRequestId = 0;
            }
            };

        // --- 방향키 네비게이션 (Up/Down: 사진 1장씩, Left/Right: 그룹 건너뛰기) ---
        if (!initialPreloadActive && !loadedImages.empty()) {
            // [ 위 화살표 ] : 1장 이전으로 (첫 이미지일 경우 그룹의 마지막 이미지로 순환)
            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true)) {
                if (selectedIndex > 0 && loadedImages[selectedIndex - 1].groupID == loadedImages[selectedIndex].groupID) {
                    SelectIndex(selectedIndex - 1);
                }
                else {
                    // std::upper_bound를 사용해 현재 그룹이 끝나는 다음 지점의 포인터(반복자)를 찾고 1을 빼서 마지막 인덱스 도출
                    int currentGroup = loadedImages[selectedIndex].groupID;
                    auto it = std::upper_bound(loadedImages.begin(), loadedImages.end(), currentGroup,
                        [](int g, const ImageData& img) { return g < img.groupID; });
                    int lastIndex = (int)std::distance(loadedImages.begin(), it) - 1;
                    SelectIndex(lastIndex);
                }
            }
            // [ 아래 화살표 ] : 1장 다음으로 (마지막 이미지일 경우 그룹의 첫 이미지로 순환)
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true)) {
                if (selectedIndex + 1 < (int)loadedImages.size() && loadedImages[selectedIndex + 1].groupID == loadedImages[selectedIndex].groupID) {
                    SelectIndex(selectedIndex + 1);
                }
                else {
                    // std::lower_bound를 사용해 현재 그룹이 시작되는 첫 지점의 포인터(반복자)를 즉시 도출
                    int currentGroup = loadedImages[selectedIndex].groupID;
                    auto it = std::lower_bound(loadedImages.begin(), loadedImages.end(), currentGroup,
                        [](const ImageData& img, int g) { return img.groupID < g; });
                    int firstIndex = (int)std::distance(loadedImages.begin(), it);
                    SelectIndex(firstIndex);
                }
            }
            // [ 왼쪽 화살표 ] : 이전 그룹의 첫 번째 사진으로 건너뛰기
            if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true)) {
                if (selectedIndex > 0) {
                    int currentGroup = loadedImages[selectedIndex].groupID;
                    int targetIndex = 0;
                    for (int i = selectedIndex - 1; i >= 0; --i) {
                        if (loadedImages[i].groupID < currentGroup) {
                            int targetGroup = loadedImages[i].groupID;
                            targetIndex = i;
                            // 찾은 이전 그룹의 가장 첫 번째 사진 인덱스를 탐색
                            for (int j = i; j >= 0; --j) {
                                if (loadedImages[j].groupID == targetGroup) targetIndex = j;
                                else break;
                            }
                            break;
                        }
                    }
                    SelectIndex(targetIndex);
                }
            }
            // [ 오른쪽 화살표 ] : 다음 그룹의 첫 번째 사진으로 건너뛰기
            if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true)) {
                int currentGroup = loadedImages[selectedIndex].groupID;
                int targetIndex = (int)loadedImages.size() - 1;
                for (int i = selectedIndex + 1; i < (int)loadedImages.size(); ++i) {
                    if (loadedImages[i].groupID > currentGroup) {
                        targetIndex = i;
                        break;
                    }
                }
                SelectIndex(targetIndex);
            }
        }

        // Label keys (1/2/3) - on press, change status then auto-skip via SelectIndex
        if (!initialPreloadActive && ImGui::IsKeyPressed(ImGuiKey_1, false)) {
            if (selectedIndex >= 0 && selectedIndex < (int)loadedImages.size()) {
                loadedImages[selectedIndex].status = ImageData::Status::Pick;
                if (selectedIndex + 1 < (int)loadedImages.size()) SelectIndex(selectedIndex + 1);
            }
        }
        if (!initialPreloadActive && ImGui::IsKeyPressed(ImGuiKey_2, false)) {
            if (selectedIndex >= 0 && selectedIndex < (int)loadedImages.size()) {
                loadedImages[selectedIndex].status = ImageData::Status::Hold;
                if (selectedIndex + 1 < (int)loadedImages.size()) SelectIndex(selectedIndex + 1);
            }
        }
        if (!initialPreloadActive && ImGui::IsKeyPressed(ImGuiKey_3, false)) {
            if (selectedIndex >= 0 && selectedIndex < (int)loadedImages.size()) {
                loadedImages[selectedIndex].status = ImageData::Status::Reject;
                if (selectedIndex + 1 < (int)loadedImages.size()) SelectIndex(selectedIndex + 1);
            }
        }
        // Export key (E)
        if (!showExportPopup && !showHoldWarningPopup && ImGui::IsKeyPressed(ImGuiKey_E, false)) {
            holdCount = 0;
            pendingExportPicks.clear();

            for (const auto& im : loadedImages) {
                if (im.status == ImageData::Status::Pick) {
                    pendingExportPicks.push_back(im.wFilePath);
                }
                else if (im.status == ImageData::Status::Hold) {
                    holdCount++;
                }
            }

            if (!pendingExportPicks.empty() && !g_exportRunning.load()) {
                if (holdCount > 0) {
                    showHoldWarningPopup = true;
                    ImGui::OpenPopup("보류 이미지 알림");
                }
                else {
                    std::wstring destPath = SelectFolderDialogW();
                    if (!destPath.empty()) {
                        {
                            std::lock_guard<std::mutex> lk(g_exportMutex);
                            g_exportLogs.clear();
                        }
                        g_exportCancelFlag = false;
                        StartExport(destPath, pendingExportPicks);
                        showExportPopup = true;
                        ImGui::OpenPopup("Export Progress");
                    }
                }
            }
            else if (pendingExportPicks.empty()) {
                std::lock_guard<std::mutex> lk(g_exportMutex);
                g_exportLogs.clear();
                g_exportLogs.emplace_back("[Info] 내보낼 Pick 항목이 없습니다.");
                showExportPopup = true;
                ImGui::OpenPopup("Export Progress");
            }
        }

        // ============================================
        //  여기가 우리가 만들 UI 창(Workspace)입니다!
        // ============================================

        // 창 크기를 윈도우 전체에 꽉 채웁니다.
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);

        ImGui::Begin("FastPIC - Culling Workspace", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar);

        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "=== FastPIC: Culling Workspace ===");
        ImGui::SameLine(ImGui::GetWindowWidth() - 260.0f);

        // 후원 버튼 (클릭 시 기본 브라우저로 Buy Me a Coffee 열기)
        if (ImGui::Button("Buy me a coffee", ImVec2(130, 0))) {
            ShellExecuteA(NULL, "open", "https://buymeacoffee.com/baesumin0211", NULL, NULL, SW_SHOWNORMAL);
        }
        ImGui::SameLine();

        // 기존 도움말 버튼
        if (ImGui::Button("도움말 (Help)", ImVec2(110, 0))) {
            showHelpPopup = true;
        }

        //  원본 해상도 토글 UI 추가
        static bool ui_loadFullResolution = false;
        if (ImGui::Checkbox("원본 화소로 불러오기 (고화질 / 메모리 사용량 증가)", &ui_loadFullResolution)) {
            g_loadFullResolution.store(ui_loadFullResolution);

            // 옵션이 변경되면 캐시를 모두 비우고 화면을 리프레시 (즉시 적용)
            g_textureCache.Clear();
            if (selectedIndex >= 0 && selectedIndex < (int)loadedImages.size()) {
                int tempIndex = selectedIndex;
                selectedIndex = -1; // 강제 초기화하여 SelectIndex가 무조건 다시 작동하게 만듦
                SelectIndex(tempIndex);
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("사양이 좋지 않은 컴퓨터는 체크를 권장하지 않습니다.\n(확대 시 화질은 매우 선명해지지만 램 용량을 많이 차지하게 됩니다)");
        }
        ImGui::Spacing();

        // 폴더 열기 버튼
        if (ImGui::Button("폴더 열기 (Folder Load)", ImVec2(200, 40))) {
            std::wstring wpath = SelectFolderDialogW();
            if (!wpath.empty()) {
                wCurrentFolderPath = wpath;
                currentFolderPath = WideStringToUtf8(wCurrentFolderPath);
                g_allImages.clear(); // 🌟 원본 백업 리스트 초기화
                selectedIndex = -1;
                selectedRequestId = 0;
                initialPreloadActive = false;
                ReleaseCurrentImage();

                // 필터 초기화
                g_filterExt = "All"; g_filterCamera = "All"; g_filterLens = "All"; g_sortMode = 0;

                try {
                    for (const auto& entry : fs::directory_iterator(wCurrentFolderPath)) {
                        if (entry.is_regular_file() && isImageFile(entry.path())) {
                            time_t unixTime = 0;
                            struct _stat64 fileInfo;
                            if (_wstat64(entry.path().wstring().c_str(), &fileInfo) == 0) {
                                unixTime = fileInfo.st_mtime;
                            }
                            char timeBuf[64] = "시간 정보 없음";
                            if (unixTime > 0) {
                                struct tm timeinfo;
                                localtime_s(&timeinfo, &unixTime);
                                strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", &timeinfo);
                            }

                            ImageData d;
                            d.filePath = WideStringToUtf8(entry.path().wstring());
                            d.wFilePath = entry.path().wstring();
                            d.fileName = WideStringToUtf8(entry.path().filename().wstring());

                            // 확장자 소문자로 저장
                            std::string ext = entry.path().extension().string();
                            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
                            d.extension = ext;

                            d.captureTime = timeBuf;
                            d.captureTimeT = unixTime;
                            d.orientation = 1;
                            if (isRawFile(entry.path())) d.hasAF = true;
                            d.groupID = 0;

                            g_allImages.push_back(d);
                            if (g_allImages.size() >= 10000) break;
                        }
                    }
                }
                catch (...) {}

                // 필터 및 정렬을 거쳐 loadedImages를 구성
                ApplyFiltersAndSort(loadedImages, totalGroups);

                // 백그라운드 메타데이터 스캐너 가동! (화면 지연 없음)
                RunBackgroundExifScanner();

                initialPreloadCount = (std::min)((int)loadedImages.size(), 10);
                // (이하 initialPreloadDone = 0; 부터 기존 프리로드 로직 동일하게 유지)

                std::sort(loadedImages.begin(), loadedImages.end(), [](const ImageData& a, const ImageData& b) { return a.captureTimeT < b.captureTimeT; });

                totalGroups = 1;
                if (!loadedImages.empty()) {
                    selectedIndex = 0;
                    loadedImages[0].groupID = totalGroups;
                    for (size_t i = 1; i < loadedImages.size(); ++i) {
                        if (loadedImages[i].captureTimeT == 0 || loadedImages[i - 1].captureTimeT == 0 ||
                            std::difftime(loadedImages[i].captureTimeT, loadedImages[i - 1].captureTimeT) > 2.0) {
                            ++totalGroups;
                        }
                        loadedImages[i].groupID = totalGroups;
                    }
                }

                initialPreloadCount = (std::min)((int)loadedImages.size(), 10);
                initialPreloadDone = 0;
                std::vector<int> preloadIndices;
                for (int i = 0; i < initialPreloadCount; ++i) {
                    int w = 0, h = 0;
                    ID3D11ShaderResourceView* cached = g_textureCache.Get(loadedImages[i].filePath, w, h);
                    if (cached) {
                        cached->Release();
                        ++initialPreloadDone;
                    }
                    else {
                        preloadIndices.push_back(i);
                    }
                }
                initialPreloadActive = !preloadIndices.empty();
                if (initialPreloadActive) {
                    initialPreloadBatchId = EnqueueDecodeRequestExclusiveBatch(preloadIndices, loadedImages);
                    g_initialPreloadRequestId.store(initialPreloadBatchId);
                    g_initialPreloadTotal.store((int)preloadIndices.size());
                    g_initialPreloadCompleted.store(0);
                    selectedRequestId = (selectedIndex == 0 && !preloadIndices.empty() && preloadIndices.front() == 0)
                        ? initialPreloadBatchId : 0;
                    ImGui::OpenPopup("준비 중...");
                }
            }
        }

        if (initialPreloadActive) {
            const int batchTotal = g_initialPreloadTotal.load();
            const int batchDone = g_initialPreloadCompleted.load();
            initialPreloadDone = (initialPreloadCount - batchTotal) + batchDone;
            ImGui::OpenPopup("준비 중...");
            if (ImGui::BeginPopupModal("준비 중...", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
                ImGui::Text("처음 사진을 준비하고 있습니다...");
                const float progress = initialPreloadCount > 0
                    ? (float)initialPreloadDone / (float)initialPreloadCount
                    : 1.0f;
                ImGui::ProgressBar(progress, ImVec2(320.0f, 0.0f), "");
                ImGui::Text("%d / %d", initialPreloadDone, initialPreloadCount);
                if (batchDone >= batchTotal) {
                    initialPreloadActive = false;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
        }

        ImGui::SameLine(); // 옆으로 이어서 텍스트 출력
        ImGui::Text("현재 경로: %s", currentFolderPath.c_str());
        ImGui::Separator();

        // 스캔 결과 리스트 출력 영역 (마우스 스크롤 가능)
        if (!loadedImages.empty()) {
            ImGui::Text("총 로드된 사진: %d장 | 생성된 그룹: %d개", (int)loadedImages.size(), totalGroups);
            ImGui::Spacing();

            // 필터 및 정렬 옵션 바 UI
            // 배경(ChildBg)과 드롭다운 팝업(PopupBg)의 투명도를 1.0f(완전 불투명)로 설정
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.12f, 0.14f, 0.18f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.15f, 0.18f, 0.22f, 1.0f));

            ImGui::BeginChild("FilterSortBar", ImVec2(0, 45), true, ImGuiWindowFlags_NoScrollbar);

            bool filterChanged = false;

            // 🌟 수정: 정렬 방식이 '시간순(0)'이 아닐 경우 체크박스를 강제 해제하고 회색으로 잠금(Disabled)
            bool isNotTimeSort = (g_sortMode != 0);
            if (isNotTimeSort) {
                if (g_enableGrouping) {
                    g_enableGrouping = false;
                    filterChanged = true;
                }
                ImGui::BeginDisabled(); // 비활성화 시작
            }

            // 그룹화 토글
            if (ImGui::Checkbox("씬(Scene) 단위 그룹화 켜기", &g_enableGrouping)) filterChanged = true;

            if (isNotTimeSort) {
                ImGui::EndDisabled(); // 비활성화 끝
            }
            ImGui::SameLine(0, 20.0f);

            // 정렬 드롭다운
            ImGui::PushItemWidth(120.0f);
            if (ImGui::Combo("정렬", &g_sortMode, "촬영 시간순\0ISO 순\0조리개(F) 순\0셔터스피드 순\0화각(mm) 순\0\0")) {
                filterChanged = true;
                if (g_sortMode != 0) {
                    g_enableGrouping = false; // 다른 정렬 방식으로 바뀌는 순간 즉시 연동 해제
                }
            }
            ImGui::PopItemWidth();
            ImGui::SameLine(0, 20.0f);

            // 헬퍼 람다: 드롭다운 렌더링
            auto RenderFilterCombo = [&](const char* label, std::string& currentVal, const std::vector<std::string>& list) {
                ImGui::PushItemWidth(140.0f);
                if (ImGui::BeginCombo(label, currentVal.c_str())) {
                    for (const auto& item : list) {
                        bool is_selected = (currentVal == item);
                        if (ImGui::Selectable(item.c_str(), is_selected)) { currentVal = item; filterChanged = true; }
                        if (is_selected) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
                ImGui::PopItemWidth();
                };

            RenderFilterCombo("포맷", g_filterExt, g_extList); ImGui::SameLine(0, 10.0f);
            RenderFilterCombo("카메라", g_filterCamera, g_cameraList); ImGui::SameLine(0, 10.0f);
            RenderFilterCombo("렌즈", g_filterLens, g_lensList);

            ImGui::EndChild();
            ImGui::PopStyleColor(2); // Push를 2번 했으므로 Pop도 2번 호출하여 스타일 복구

            // 필터가 변경되었으면 재적용하고 리스트를 갱신
            if (filterChanged) {
                // 1. 변경 전, 현재 보고 있던 사진의 고유 경로를 책갈피처럼 기억해 둡니다.
                std::wstring prevSelectedPath = L"";
                if (selectedIndex >= 0 && selectedIndex < (int)loadedImages.size()) {
                    prevSelectedPath = loadedImages[selectedIndex].wFilePath;
                }

                // 2. 필터 및 정렬 재적용 (리스트 재구성)
                ApplyFiltersAndSort(loadedImages, totalGroups);

                // 3. 새로 구성된 리스트에서 아까 보던 사진이 어디로 갔는지 찾아냅니다.
                int newSelectedIndex = -1;
                if (!prevSelectedPath.empty()) {
                    for (int i = 0; i < (int)loadedImages.size(); ++i) {
                        if (loadedImages[i].wFilePath == prevSelectedPath) {
                            newSelectedIndex = i;
                            break;
                        }
                    }
                }

                // 4. 사진이 여전히 목록에 존재한다면 화면을 유지한 채 인덱스만 업데이트!
                if (newSelectedIndex != -1) {
                    selectedIndex = newSelectedIndex;
                    scrollToSelection = true; // 왼쪽 리스트의 스크롤도 바뀐 위치로 부드럽게 따라가도록 설정
                }
                else {
                    // 확장자/렌즈 필터 등으로 인해 보던 사진이 목록에서 아예 제외된 경우에만 뷰어를 비웁니다.
                    selectedIndex = -1;
                    ReleaseCurrentImage();
                }
            }

            // 리스트를 담을 하위 스크롤 박스 생성
            // 좌우 패널로 분리: 왼쪽은 리스트, 오른쪽은 선택된 사진 뷰어
            ImGui::Columns(2, "TwoPane", true);

            //  좌측 패널 가로 크기 강제 제한 (최소 500px, 최대 500px)
            float leftWidth = ImGui::GetColumnWidth(0);
            if (leftWidth < 500.0f) ImGui::SetColumnWidth(0, 500.0f);
            if (leftWidth > 500.0f) ImGui::SetColumnWidth(0, 500.0f);

            // --- 왼쪽: 파일 리스트 ---
            ImGui::BeginChild("LeftPanel", ImVec2(0, 0), true);

            // 그룹 구분 헤더와 들여쓰기 처리를 위한 상태 (stateless per-item indent)
            int lastGroupID = -1;
            for (size_t i = 0; i < loadedImages.size(); ++i) {
                auto& img = loadedImages[i];
                // 1) 그룹이 바뀔 때 헤더 출력 (들여쓰기 밖에서 실행)
                if (img.groupID != lastGroupID) {
                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "--- [ Group %d ] ---", img.groupID);
                    ImGui::Spacing();
                    lastGroupID = img.groupID;
                }
                ImVec4 color = (img.groupID % 2 == 0) ? ImVec4(0.8f, 0.8f, 0.8f, 1.0f) : ImVec4(1.0f, 0.9f, 0.7f, 1.0f);
                char label[1024];
                snprintf(label, sizeof(label), "[G%03d] %s", img.groupID, img.fileName.c_str());

                // 상태 라벨 및 색상 결정
                const char* statusLabel = "";
                ImVec4 statusColor = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
                switch (img.status) {
                case ImageData::Status::Pick:
                    statusLabel = "[Pick]";
                    statusColor = ImVec4(0.2f, 0.9f, 0.2f, 1.0f);
                    break;
                case ImageData::Status::Hold:
                    statusLabel = "[Hold]";
                    statusColor = ImVec4(0.9f, 0.8f, 0.2f, 1.0f);
                    break;
                case ImageData::Status::Reject:
                    statusLabel = "[Reject]";
                    statusColor = ImVec4(0.9f, 0.3f, 0.3f, 1.0f);
                    break;
                default:
                    break;
                }

                // 2) 각 사진 항목은 stateless하게 정확히 20픽셀 들여쓰기 후 바로 되돌림
                ImGui::Indent(20.0f);

                //  현재 그리는 항목이 사용자에 의해 '선택된 항목'이고, 키보드 등으로 선택이 바뀐 순간인지 체크
                bool isSelected = (selectedIndex == (int)i);

                if (ImGui::Selectable(label, isSelected, ImGuiSelectableFlags_SpanAllColumns)) {
                    if (selectedIndex != (int)i) {
                        SelectIndex((int)i);
                    }
                }

                //  명시적으로 선택이 변경되어 신호가 켜졌을 때만 딱 한 번 화면 정중앙(0.5f)으로 스크롤 수행
                if (isSelected && scrollToSelection) {
                    ImGui::SetScrollHereY(0.5f);
                    scrollToSelection = false; // 스크롤을 완료했으므로 신호를 끕니다.
                }

                ImGui::SameLine();
                // 3색 신호등(Traffic Light) 스타일 상태 표시
                ImVec2 cursorPos = ImGui::GetCursorScreenPos();
                float contentRegionAvail = ImGui::GetContentRegionAvail().x;
                float lineHeight = ImGui::GetTextLineHeight();
                float baseY = cursorPos.y + lineHeight * 0.5f;

                // 밝은 색상과 어두운 색상 정의
                ImU32 colorGreen = IM_COL32(50, 230, 50, 255);
                ImU32 colorYellow = IM_COL32(230, 204, 50, 255);
                ImU32 colorRed = IM_COL32(230, 76, 76, 255);
                ImU32 colorDark = IM_COL32(60, 60, 60, 255);

                // 각 상태에 따라 색상 결정 (마치 신호등처럼 켜진 불 표시)
                ImU32 col1 = colorDark, col2 = colorDark, col3 = colorDark;
                switch (img.status) {
                case ImageData::Status::Pick:
                    col1 = colorGreen;
                    break;
                case ImageData::Status::Hold:
                    col2 = colorYellow;
                    break;
                case ImageData::Status::Reject:
                    col3 = colorRed;
                    break;
                default:
                    break;
                }

                // 우측 끝에서 좌측으로 -55.0f, -35.0f, -15.0f 떨어진 위치에 원 3개 그림
                float circle1X = cursorPos.x + contentRegionAvail - 55.0f;
                float circle2X = cursorPos.x + contentRegionAvail - 35.0f;
                float circle3X = cursorPos.x + contentRegionAvail - 15.0f;

                ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(circle1X, baseY), 5.0f, col1);
                ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(circle2X, baseY), 5.0f, col2);
                ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(circle3X, baseY), 5.0f, col3);

                ImGui::TextColored(color, "%s", img.captureTime.c_str());
                ImGui::Unindent(20.0f);
            }
            // 루프가 끝난 뒤 이전 구현에서 사용하던 indented 변수는 더 이상 필요함 없음
            ImGui::EndChild();

            ImGui::NextColumn();

            // --- 오른쪽: 이미지 뷰어 ---
            ImGui::BeginChild("RightPanel", ImVec2(0, 0), false,
                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            // 뷰어 메타데이터 정보창 (Viewer Info)
            {
                ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "=== Viewer Info ===");
                ImGui::Text("Size: %d x %d | Decode Q: %llu | Upload Q: %llu",
                    g_currentImageWidth, g_currentImageHeight,
                    (unsigned long long)GetDecodeQueueSize(), (unsigned long long)GetUploadQueueSize());
                ImGui::Separator();

                if (selectedIndex >= 0 && selectedIndex < (int)loadedImages.size()) {
                    ImageData displayImg = loadedImages[selectedIndex];

                    // 복사된 loadedImages 대신 백그라운드가 실시간 갱신 중인 원본(g_allImages)을 직접 조회
                    {
                        std::lock_guard<std::mutex> lock(g_dataMutex);
                        auto it = std::find_if(g_allImages.begin(), g_allImages.end(),
                            [&](const ImageData& a) { return a.wFilePath == displayImg.wFilePath; });
                        if (it != g_allImages.end() && it->exifLoaded) {
                            displayImg = *it;
                        }
                    }

                    ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.4f, 1.0f), "[ Camera & Lens ]");
                    ImGui::Text("기종 : %s", displayImg.cameraModel.empty() ? (displayImg.exifLoaded ? "정보 없음" : "Reading...") : displayImg.cameraModel.c_str());
                    ImGui::Text("렌즈 : %s", displayImg.lensModel.empty() ? (displayImg.exifLoaded ? "정보 없음" : "Reading...") : displayImg.lensModel.c_str());
                    ImGui::Spacing();

                    ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.4f, 1.0f), "[ Shooting Data ]");
                    ImGui::Text("설정 : %s | F/%.1f | ISO %d | %.0fmm",
                        displayImg.exposureTimeStr.empty() ? "-" : displayImg.exposureTimeStr.c_str(),
                        displayImg.fNumber, displayImg.iso, displayImg.focalLength);
                    ImGui::Separator();
                }
            }
            // 로딩 상태 판단: 선택된 인덱스가 있고, 현재 선택에 대한 요청 ID가 있으며 아직 완료된 요청보다 큰 경우 로딩 표시
            bool showLoading = false;
            if (selectedIndex >= 0 && selectedRequestId != 0 && selectedRequestId > g_lastCompletedRequestId.load()) showLoading = true;

            if (showLoading) {
                ImVec2 avail = ImGui::GetContentRegionAvail();
                // 간단한 점 애니메이션 (초 단위 시간 기반)
                int dots = (int)(ImGui::GetTime() * 3.0f) % 4;
                std::string loading = "로딩 중" + std::string(dots, '.');
                ImGui::SetCursorPosX((avail.x - ImGui::CalcTextSize(loading.c_str()).x) * 0.5f);
                ImGui::SetCursorPosY((avail.y - ImGui::GetTextLineHeight()) * 0.5f);
                ImGui::TextColored(ImVec4(0.9f, 0.9f, 0.6f, 1.0f), "%s", loading.c_str());
            }
            // 수정: selectedIndex가 안전한 범위 내에 있을 때만 실행되도록 조건 2줄 추가
            else if (g_currentImageSRV && g_currentImageWidth > 0 && g_currentImageHeight > 0 &&
                selectedIndex >= 0 && selectedIndex < (int)loadedImages.size()) {

                float imgW = (float)g_currentImageWidth;
                float imgH = (float)g_currentImageHeight;
                ImGui::Text("%s", loadedImages[selectedIndex].fileName.c_str());
                ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "%s", loadedImages[selectedIndex].filePath.c_str());
                ImGui::Separator();

                ImVec2 totalAvail = ImGui::GetContentRegionAvail();

                // ==============================================================
                // [ 뷰어 1: 메인 전체화면 뷰어 (전체 영역 100% 사용) ]
                // ==============================================================
                ImGui::BeginChild("Viewer1_Main", totalAvail, false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
                ImVec2 avail1 = ImGui::GetContentRegionAvail();
                float ratioW1 = avail1.x / imgW;
                float ratioH1 = avail1.y / imgH;
                float fitScale = (ratioW1 < ratioH1) ? ratioW1 : ratioH1;
                if (fitScale <= 0.0f) fitScale = 1.0f;

                const ImVec2 canvasMin1 = ImGui::GetCursorScreenPos();
                const ImVec2 canvasCenter1(canvasMin1.x + avail1.x * 0.5f, canvasMin1.y + avail1.y * 0.5f);
                ImGuiIO& viewerIO = ImGui::GetIO();
                const bool viewerHovered = ImGui::IsWindowHovered();

                if (viewerHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) {
                    zoomLevel = 1.0f;
                    panOffset = ImVec2(0.0f, 0.0f);
                }
                if (viewerHovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    zoomLevel = 1.0f;
                    panOffset = ImVec2(0.0f, 0.0f);
                }

                if (viewerHovered && viewerIO.MouseWheel != 0.0f) {
                    const float oldZoom = zoomLevel;
                    const float requestedZoom = oldZoom * (viewerIO.MouseWheel > 0.0f ? 1.2f : 1.0f / 1.2f);
                    const float newZoom = (requestedZoom < 0.1f) ? 0.1f : ((requestedZoom > 5.0f) ? 5.0f : requestedZoom);
                    if (newZoom != oldZoom) {
                        const ImVec2 mouse = viewerIO.MousePos;
                        const float scaleRatio = newZoom / oldZoom;
                        panOffset.x = mouse.x - canvasCenter1.x - (mouse.x - canvasCenter1.x - panOffset.x) * scaleRatio;
                        panOffset.y = mouse.y - canvasCenter1.y - (mouse.y - canvasCenter1.y - panOffset.y) * scaleRatio;
                        zoomLevel = newZoom;
                    }
                }
                if (viewerHovered && zoomLevel > 1.0f && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                    panOffset.x += viewerIO.MouseDelta.x;
                    panOffset.y += viewerIO.MouseDelta.y;
                }

                ImVec2 drawSize1(imgW * fitScale * zoomLevel, imgH * fitScale * zoomLevel);
                if (drawSize1.x <= avail1.x) {
                    panOffset.x = 0.0f;
                }
                else {
                    const float maxPanX = (drawSize1.x - avail1.x) * 0.5f;
                    panOffset.x = (panOffset.x < -maxPanX) ? -maxPanX : ((panOffset.x > maxPanX) ? maxPanX : panOffset.x);
                }
                if (drawSize1.y <= avail1.y) {
                    panOffset.y = 0.0f;
                }
                else {
                    const float maxPanY = (drawSize1.y - avail1.y) * 0.5f;
                    panOffset.y = (panOffset.y < -maxPanY) ? -maxPanY : ((panOffset.y > maxPanY) ? maxPanY : panOffset.y);
                }

                const float imageX1 = (drawSize1.x <= avail1.x)
                    ? canvasMin1.x + (avail1.x - drawSize1.x) * 0.5f
                    : canvasCenter1.x - drawSize1.x * 0.5f + panOffset.x;
                const float imageY1 = (drawSize1.y <= avail1.y)
                    ? canvasMin1.y + (avail1.y - drawSize1.y) * 0.5f
                    : canvasCenter1.y - drawSize1.y * 0.5f + panOffset.y;
                ImVec2 imageTopLeft1(imageX1, imageY1);
                const ImVec2 canvasMax1(canvasMin1.x + avail1.x, canvasMin1.y + avail1.y);

                ImGui::PushClipRect(canvasMin1, canvasMax1, true);
                ImGui::SetCursorScreenPos(imageTopLeft1);
                ImGui::Image((void*)g_currentImageSRV, drawSize1);

                if (showAFPoint && loadedImages[selectedIndex].hasAF) {
                    const ImageData& image = loadedImages[selectedIndex];
                    const ImVec2 pMin(
                        imageTopLeft1.x + (image.afX - image.afW * 0.5f) * drawSize1.x,
                        imageTopLeft1.y + (image.afY - image.afH * 0.5f) * drawSize1.y);
                    const ImVec2 pMax(
                        imageTopLeft1.x + (image.afX + image.afW * 0.5f) * drawSize1.x,
                        imageTopLeft1.y + (image.afY + image.afH * 0.5f) * drawSize1.y);
                    ImGui::GetWindowDrawList()->AddRect(pMin, pMax, IM_COL32(255, 0, 0, 255), 0.0f, 0, 2.0f);
                }
                ImGui::PopClipRect();

                // --------------------------------------------------------------
                //  AF Point 및 PiP 토글 UI (좌측 하단)
                // --------------------------------------------------------------
                static bool showPiP = false; // PiP 뷰어 표시 상태를 저장할 정적 변수 추가

                const char* textAF = "AF Point (초점)";
                const char* textPiP = "AF Focus 뷰어 (PiP)";

                // 두 개의 체크박스가 들어갈 너비를 계산
                float checkWidthAF = ImGui::CalcTextSize(textAF).x + 35.0f;
                float checkWidthPiP = ImGui::CalcTextSize(textPiP).x + 35.0f;
                float totalBgWidth = checkWidthAF + checkWidthPiP + 10.0f;

                const ImVec2 togglePos(canvasMin1.x + 20.0f, canvasMax1.y - ImGui::GetFrameHeight() - 15.0f);
                ImDrawList* drawList = ImGui::GetWindowDrawList();

                // 두 체크박스가 넉넉히 들어갈 수 있는 반투명 배경 박스 그리기
                drawList->AddRectFilled(
                    ImVec2(togglePos.x - 8.0f, togglePos.y - 5.0f),
                    ImVec2(togglePos.x + totalBgWidth, togglePos.y + ImGui::GetFrameHeight() + 5.0f),
                    IM_COL32(0, 0, 0, 160), 4.0f);

                ImGui::SetCursorScreenPos(togglePos);

                // AF 데이터가 없는 사진이면 체크박스를 비활성화하고 창을 강제로 닫음
                bool currentHasAF = loadedImages[selectedIndex].hasAF;
                if (!currentHasAF) {
                    showAFPoint = false;
                    showPiP = false;
                    ImGui::BeginDisabled(); // 컨트롤을 조작 불가능한 회색 상태로 잠금
                }

                // 체크박스 1: AF Point
                ImGui::Checkbox(textAF, &showAFPoint);
                ImGui::SameLine(0, 15.0f); // 체크박스 사이의 간격
                // 체크박스 2: PiP 뷰어 토글
                ImGui::Checkbox(textPiP, &showPiP);

                // 잠금 해제
                if (!currentHasAF) {
                    ImGui::EndDisabled();
                }

                // ==============================================================
                // [ 뷰어 2: AF 영역 확대 뷰어 (우측 하단 PiP 오버레이 - 정사각형) ]
                // ==============================================================

                // 사용자가 체크박스를 켰을 때만 PiP 창을 렌더링합니다.
                if (showPiP) {
                    // 1. PiP 크기를 정적으로 기억하여 사용자 조절이 영구 유지되도록 함
                    static float pipBaseSize = 0.0f;
                    static bool initPipSize = true;
                    if (initPipSize) {
                        pipBaseSize = (std::min)(avail1.x, avail1.y) * 0.35f; // 초기 기본 크기
                        initPipSize = false;
                    }

                    // 2. 최소/최대 크기 락(Lock) 설정 (너무 작아지거나 화면을 덮는 것 방지)
                    const float MIN_PIP_SIZE = 280.0f;
                    if (pipBaseSize < MIN_PIP_SIZE) pipBaseSize = MIN_PIP_SIZE;
                    if (pipBaseSize > avail1.x * 0.8f) pipBaseSize = avail1.x * 0.8f;
                    if (pipBaseSize > avail1.y * 0.8f) pipBaseSize = avail1.y * 0.8f;

                    ImVec2 pipSize(pipBaseSize, pipBaseSize);

                    // 메인 뷰어 우측 하단 구석에서 20px씩 여백을 두고 위치 고정
                    ImVec2 pipPos(canvasMax1.x - pipSize.x - 20.0f, canvasMax1.y - pipSize.y - 20.0f);

                    ImGui::SetCursorScreenPos(pipPos);
                    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.08f, 0.08f, 0.1f, 0.95f));
                    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.3f, 0.8f, 0.3f, 0.8f));
                    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 2.0f);

                    ImGui::BeginChild("Viewer2_PiP", pipSize, true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

                    ImVec2 avail2 = ImGui::GetContentRegionAvail();
                    const ImVec2 canvasMin2 = ImGui::GetCursorScreenPos();
                    const ImVec2 canvasMax2(canvasMin2.x + avail2.x, canvasMin2.y + avail2.y);
                    const ImVec2 canvasCenter2(canvasMin2.x + avail2.x * 0.5f, canvasMin2.y + avail2.y * 0.5f);

                    static float focusMagnifyScale = 1.00f;
                    ImVec2 drawSize2(imgW * focusMagnifyScale, imgH * focusMagnifyScale);

                    float focusX = 0.5f;
                    float focusY = 0.5f;
                    if (loadedImages[selectedIndex].hasAF) {
                        focusX = loadedImages[selectedIndex].afX;
                        focusY = loadedImages[selectedIndex].afY;
                    }

                    float imageX2 = canvasCenter2.x - focusX * drawSize2.x;
                    float imageY2 = canvasCenter2.y - focusY * drawSize2.y;
                    ImVec2 imageTopLeft2(imageX2, imageY2);

                    ImGui::PushClipRect(canvasMin2, canvasMax2, true);
                    ImGui::SetCursorScreenPos(imageTopLeft2);
                    ImGui::Image((void*)g_currentImageSRV, drawSize2);

                    ImDrawList* pipDrawList = ImGui::GetWindowDrawList();

                    if (loadedImages[selectedIndex].hasAF) {
                        pipDrawList->AddLine(ImVec2(canvasCenter2.x - 15, canvasCenter2.y), ImVec2(canvasCenter2.x + 15, canvasCenter2.y), IM_COL32(50, 230, 50, 200), 2.0f);
                        pipDrawList->AddLine(ImVec2(canvasCenter2.x, canvasCenter2.y - 15), ImVec2(canvasCenter2.x, canvasCenter2.y + 15), IM_COL32(50, 230, 50, 200), 2.0f);
                    }
                    else {
                        ImGui::SetCursorScreenPos(ImVec2(canvasCenter2.x - 70, canvasCenter2.y - 10));
                        ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 0.6f), "AF 정보 없음 (Center)");
                    }
                    ImGui::PopClipRect();

                    // --------------------------------------------------------
                    //  PiP 크기 수동 조절 핸들 (좌측 상단 모서리)
                    // --------------------------------------------------------
                    ImVec2 gripPos = canvasMin2;
                    // 크기 조절 손잡이 디자인 (회색 삼각형과 빗금 2개)
                    pipDrawList->AddTriangleFilled(
                        ImVec2(gripPos.x, gripPos.y),
                        ImVec2(gripPos.x + 30.0f, gripPos.y),
                        ImVec2(gripPos.x, gripPos.y + 30.0f),
                        IM_COL32(80, 80, 80, 200)
                    );
                    pipDrawList->AddLine(ImVec2(gripPos.x + 12.0f, gripPos.y + 4.0f), ImVec2(gripPos.x + 4.0f, gripPos.y + 12.0f), IM_COL32(200, 200, 200, 255), 1.5f);
                    pipDrawList->AddLine(ImVec2(gripPos.x + 20.0f, gripPos.y + 4.0f), ImVec2(gripPos.x + 4.0f, gripPos.y + 20.0f), IM_COL32(200, 200, 200, 255), 1.5f);

                    ImGui::SetCursorScreenPos(gripPos);
                    ImGui::InvisibleButton("##PiPResize", ImVec2(30.0f, 30.0f));

                    // 마우스 오버 시 대각선 화살표 커서로 변경
                    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
                        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
                    }
                    // 드래그 시 크기 반영
                    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                        // 좌상단을 드래그하므로, 왼쪽(-x)/위(-y)로 마우스를 밀수록 창이 커짐
                        float dragDelta = -(ImGui::GetIO().MouseDelta.x + ImGui::GetIO().MouseDelta.y) * 0.5f;
                        pipBaseSize += dragDelta * 2.0f; // 부드럽게 반응하도록 속도 보정
                    }

                    // --------------------------------------------------------
                    //  돋보기 배율 조절 UI (그립과 안 겹치게 '우측 하단'으로 이동)
                    // --------------------------------------------------------
                    ImVec2 panelMin = ImVec2(canvasMax2.x - 160.0f, canvasMax2.y - 70.0f);
                    ImVec2 panelMax = ImVec2(canvasMax2.x - 10.0f, canvasMax2.y - 10.0f);

                    pipDrawList->AddRectFilled(panelMin, panelMax, IM_COL32(0, 0, 0, 160), 6.0f);
                    pipDrawList->AddRect(panelMin, panelMax, IM_COL32(80, 80, 80, 255), 6.0f);

                    ImGui::SetCursorScreenPos(ImVec2(panelMin.x + 12, panelMin.y + 8));
                    ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1.0f), "AF Focus");

                    ImGui::SetCursorScreenPos(ImVec2(panelMin.x + 12, panelMin.y + 32));
                    if (ImGui::Button(" - ##ZoomOut", ImVec2(30, 24))) {
                        focusMagnifyScale -= 0.25f;
                        if (focusMagnifyScale < 0.5f) focusMagnifyScale = 0.5f;
                    }

                    ImGui::SameLine();
                    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 2.0f);
                    ImGui::Text("%3.0f%%", focusMagnifyScale * 100.0f);

                    ImGui::SameLine();
                    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 2.0f);
                    if (ImGui::Button(" + ##ZoomIn", ImVec2(30, 24))) {
                        focusMagnifyScale += 0.25f;
                        if (focusMagnifyScale > 5.0f) focusMagnifyScale = 5.0f;
                    }

                    ImGui::EndChild(); // Viewer2_PiP 끝
                    ImGui::PopStyleVar();     // 테두리 굵기 원상복구
                    ImGui::PopStyleColor(2);  // 배경, 테두리 색상 원상복구
                } // showPiP 끝

                ImGui::EndChild(); // Viewer1_Main 끝
            }
            ImGui::EndChild();
            ImGui::Columns(1);
        }
        else {
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "폴더를 열어 사진을 불러오세요.");
        }

        ImGui::End();

        if (showExportPopup) {
            ImGui::OpenPopup("Export Progress");
            ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
            ImGui::SetNextWindowSize(ImVec2(600.0f, 440.0f), ImGuiCond_Appearing);

            // 뒷배경을 강하게 어둡게(Dim) 하고 팝업 자체는 거의 불투명하게 만듦
            ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, ImVec4(0.0f, 0.0f, 0.0f, 0.85f));
            ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.1f, 0.12f, 0.16f, 0.98f));

            if (ImGui::BeginPopupModal("Export Progress", nullptr, ImGuiWindowFlags_NoResize)) {
                const int progress = g_exportProgress.load();
                const int total = g_exportTotal.load();
                const float fraction = total > 0 ? static_cast<float>(progress) / static_cast<float>(total) : 0.0f;
                ImGui::ProgressBar(fraction, ImVec2(-1.0f, 0.0f));
                ImGui::Text("%d / %d", progress, total);

                std::vector<std::string> logs;
                {
                    std::lock_guard<std::mutex> lk(g_exportMutex);
                    logs = g_exportLogs;
                }
                ImGui::BeginChild("ExportLogs", ImVec2(0.0f, 300.0f), true, ImGuiWindowFlags_HorizontalScrollbar);
                for (const auto& log : logs) ImGui::TextUnformatted(log.c_str());
                ImGui::SetScrollHereY(1.0f);
                ImGui::EndChild();

                if (g_exportRunning.load()) {
                    if (ImGui::Button("취소 (Cancel)")) g_exportCancelFlag = true;
                }
                else if (ImGui::Button("닫기 (Close)")) {
                    showExportPopup = false;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
            //  팝업이 끝나면 반드시 스타일 복구
            ImGui::PopStyleColor(2);
        }

        // 보류 이미지 경고 팝업
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24.0f, 24.0f));

        //  뒷배경 Dim & 불투명도 증가
        ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, ImVec4(0.0f, 0.0f, 0.0f, 0.85f));
        ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.1f, 0.12f, 0.16f, 0.98f));

        if (ImGui::BeginPopupModal("보류 이미지 알림", &showHoldWarningPopup, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("보류(Hold) 상태인 사진이 %d장 있습니다.\nPick 상태인 사진만 내보내시겠습니까?", holdCount);
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            if (ImGui::Button("내보내기 (Export)")) {
                std::wstring destPath = SelectFolderDialogW();
                if (!destPath.empty()) {
                    {
                        std::lock_guard<std::mutex> lk(g_exportMutex);
                        g_exportLogs.clear();
                    }
                    g_exportCancelFlag = false;
                    StartExport(destPath, pendingExportPicks);
                    showExportPopup = true;
                    showHoldWarningPopup = false;
                    ImGui::CloseCurrentPopup();
                    ImGui::OpenPopup("Export Progress");
                }
            }

            ImGui::SameLine(0, 10.0f);
            if (ImGui::Button("미분류 이미지 이동")) {
                auto it = std::find_if(loadedImages.begin(), loadedImages.end(),
                    [](const ImageData& img) { return img.status == ImageData::Status::Hold; });
                if (it != loadedImages.end()) {
                    int holdIndex = std::distance(loadedImages.begin(), it);
                    SelectIndex(holdIndex);
                }
                showHoldWarningPopup = false;
                ImGui::CloseCurrentPopup();
            }

            ImGui::SameLine(0, 10.0f);
            if (ImGui::Button("취소 (Cancel)")) {
                showHoldWarningPopup = false;
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }
        //  스타일 및 패딩 복구
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar();

        // 후원(Support) 모달 팝업
        if (showSupportPopup) {
            ImGui::OpenPopup("개발자 후원하기 (Support)");
        }

        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(450.0f, 500.0f), ImGuiCond_Appearing);

        // 글래스모피즘 어두운 배경(Dim) 효과 적용
        ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, ImVec4(0.0f, 0.0f, 0.0f, 0.85f));
        ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.1f, 0.12f, 0.16f, 0.98f));

        if (ImGui::BeginPopupModal("개발자 후원하기 (Support)", &showSupportPopup, ImGuiWindowFlags_NoResize)) {
            float windowWidth = ImGui::GetWindowWidth();

            // 타이틀 중앙 정렬
            ImGui::Spacing();
            std::string title = "☕ FastPIC이 마음에 드셨나요?";
            float titleWidth = ImGui::CalcTextSize(title.c_str()).x;
            ImGui::SetCursorPosX((windowWidth - titleWidth) * 0.5f);
            ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.4f, 1.0f), "%s", title.c_str());
            ImGui::Spacing();

            std::string subtitle = "스마트폰 카메라로 QR 코드를 스캔하여\n개발자에게 커피 한 잔을 선물해 주세요!";
            // 멀티라인 텍스트 중앙 정렬을 위한 헬퍼 람다
            auto CenterText = [&](const char* text) {
                float w = ImGui::CalcTextSize(text).x;
                ImGui::SetCursorPosX((windowWidth - w) * 0.5f);
                ImGui::Text("%s", text);
                };
            CenterText("스마트폰 앱(토스, 카카오페이 등)으로 QR 코드를 스캔하여");
            CenterText("개발자에게 시원한 커피 한 잔을 선물해 주세요!");
            ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

            // QR 코드 이미지 렌더링 (중앙 정렬)
            float imgSize = 220.0f; // QR 코드 출력 크기 (픽셀)
            if (g_qrCodeSRV) {
                ImGui::SetCursorPosX((windowWidth - imgSize) * 0.5f);
                // 흰색 배경이 없으면 QR 인식이 잘 안될 수 있으므로, 하얀색 배경 패널을 먼저 그려줍니다.
                ImVec2 pos = ImGui::GetCursorScreenPos();
                ImGui::GetWindowDrawList()->AddRectFilled(pos, ImVec2(pos.x + imgSize, pos.y + imgSize), IM_COL32(255, 255, 255, 255));
                ImGui::Image((void*)g_qrCodeSRV, ImVec2(imgSize, imgSize));
            }
            else {
                ImGui::SetCursorPosX((windowWidth - 250.0f) * 0.5f);
                ImGui::TextColored(ImVec4(0.8f, 0.3f, 0.3f, 1.0f), "[ qr_code.png 파일을 찾을 수 없습니다 ]");
            }

            ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
            CenterText("📸 여러분의 후원이 더 나은 기능을 만드는 원동력이 됩니다.");
            ImGui::Spacing(); ImGui::Spacing();

            // 닫기 버튼 중앙 정렬
            float btnWidth = 120.0f;
            ImGui::SetCursorPosX((windowWidth - btnWidth) * 0.5f);
            if (ImGui::Button("닫기 (Close)", ImVec2(btnWidth, 0))) {
                showSupportPopup = false;
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }
        ImGui::PopStyleColor(2);


        // 도움말 모달 팝업
        if (showHelpPopup) {
            ImGui::OpenPopup("조작 방법 (Manual)");
        }
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(750.0f, 750.0f), ImGuiCond_Appearing);

        //  뒷배경 Dim & 불투명도 증가
        ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, ImVec4(0.0f, 0.0f, 0.0f, 0.85f));
        ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.1f, 0.12f, 0.16f, 0.98f));

        if (ImGui::BeginPopupModal("조작 방법 (Manual)", &showHelpPopup, ImGuiWindowFlags_AlwaysAutoResize)) {

            // 타이틀 강조 (중앙 정렬)
            {
                std::string titleText = "◀ FastPIC 조작 가이드 및 단축키 안내 ▶";
                float textWidth = ImGui::CalcTextSize(titleText.c_str()).x;
                float windowWidth = ImGui::GetContentRegionAvail().x;
                ImGui::SetCursorPosX((windowWidth - textWidth) * 0.5f);
                ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "%s", titleText.c_str());
            }
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            // 테이블이나 컬럼을 사용하여 좌우 정렬 맞추기
            auto RenderGuideRow = [](const char* key, const char* desc) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Bullet(); ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), key); // 키는 흰색 강조
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(desc); // 설명은 기본색
                };

            if (ImGui::BeginTable("##GuideTable", 2, ImGuiTableFlags_SizingFixedFit)) {
                ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed, 180.0f); // 단축키 영역 너비 고정
                ImGui::TableSetupColumn("Description", ImGuiTableColumnFlags_WidthStretch);

                // 1. 네비게이션
                ImGui::TableNextRow(); ImGui::TableNextColumn();
                ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.4f, 1.0f), "[ 사진 및 그룹 네비게이션 ]");
                ImGui::TableNextColumn(); // 빈칸 채우기
                RenderGuideRow("위 / 아래 방향키", "현재 그룹 내 사진 1장씩 이동 (순환)");
                RenderGuideRow("좌 / 우 방향키", "이전 / 다음 그룹(연사 묶음) 통째로 점프");

                // 구분선 및 공백
                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::Spacing(); ImGui::TableNextColumn();

                // 2. 사진 분류
                ImGui::TableNextRow(); ImGui::TableNextColumn();
                ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.4f, 1.0f), "[ 사진 분류 (Culling) ]");
                ImGui::TableNextColumn();
                RenderGuideRow("숫자 1번", "Pick (초록) - 선택 후 자동 다음 사진 이동");
                RenderGuideRow("숫자 2번", "Hold (노란) - 보류");
                RenderGuideRow("숫자 3번", "Reject (빨간) - 제외 (버리기)");

                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::Spacing(); ImGui::TableNextColumn();

                // 3. 메인 뷰어 조작
                ImGui::TableNextRow(); ImGui::TableNextColumn();
                ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.4f, 1.0f), "[ 메인 이미지 뷰어 조작 ]");
                ImGui::TableNextColumn();
                RenderGuideRow("마우스 휠", "사진 확대 / 축소 (Zoom)");
                RenderGuideRow("좌클릭 드래그", "확대된 사진 이동 (Panning)");
                RenderGuideRow("더블클릭 / 휠클릭", "화면 맞춤 (기본 배율/위치 복귀)");

                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::Spacing(); ImGui::TableNextColumn();

                // 4. PiP 돋보기 뷰어 조작
                ImGui::TableNextRow(); ImGui::TableNextColumn();
                ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.4f, 1.0f), "[ AF Focus 돋보기 뷰어 (PiP) ]");
                ImGui::TableNextColumn();
                RenderGuideRow("우측 하단 PiP 창", "카메라 AF 초점 위치 자동 확대 비교");
                RenderGuideRow("PiP 내부 [ - ] [ + ]", "배율 조절 (25% 단위, 50% ~ 500%)");
                RenderGuideRow("PiP 좌상단 모서리", "마우스 드래그로 창 크기 조절");

                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::Spacing(); ImGui::TableNextColumn();

                // 5. 내보내기
                ImGui::TableNextRow(); ImGui::TableNextColumn();
                ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.4f, 1.0f), "[ 일괄 내보내기 ]");
                ImGui::TableNextColumn();
                RenderGuideRow("E 키", "Pick(1번) 지정 사진들만 일괄 복사");

                ImGui::EndTable();
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();


            // 닫기 버튼 - 중앙 정렬
            float buttonWidth = 100.0f;
            float availWidth = ImGui::GetContentRegionAvail().x;
            ImGui::SetCursorPosX((availWidth - buttonWidth) * 0.5f + ImGui::GetCursorPosX());

            if (ImGui::Button("확인 (OK)", ImVec2(buttonWidth, 0.0f))) {
                showHelpPopup = false;
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }
        //  스타일 복구
        ImGui::PopStyleColor(2);

        // 렌더링 
        // 업로드 큐 처리: 디코더 스레드가 만든 RGBA 버퍼를 GPU 텍스처로 올립니다.
        // 현재 선택된 파일 경로를 전달하면, 업로드 후 자동으로 표시됩니다.
        std::string currentSelectedPath;
        if (selectedIndex >= 0 && selectedIndex < (int)loadedImages.size()) currentSelectedPath = loadedImages[selectedIndex].filePath;
        ProcessUploadQueueAndIntegrate(currentSelectedPath, loadedImages);

        ImGui::Render();

        //  글래스 느낌이 잘 투과되어 보이도록, 빈 배경(도화지) 색상을 세련된 어두운 푸른빛으로 변경
        const float clear_color_with_alpha[4] = { 0.05f, 0.08f, 0.12f, 1.00f };

        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color_with_alpha);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_pSwapChain->Present(1, 0); // VSync 켬
    }

    // 5. 정리 (Cleanup)
    // 디코더 스레드 종료: 작업 중이면 알리고 조인 (리소스 해제 전에 스레드 종료)
    g_decodeThreadRunning = false;
    g_decodeCv.notify_all();
    for (auto& t : g_decodeThreads) {
        if (t.joinable()) t.join();
    }

    // EXIF 메타데이터 스캐너 스레드 안전 종료
    g_exifTaskRunning = false;
    if (g_exifThread.joinable()) {
        g_exifThread.join();
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    if (g_qrCodeSRV) g_qrCodeSRV->Release();
    CleanupDeviceD3D();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
    CoUninitialize();

    return 0;
};