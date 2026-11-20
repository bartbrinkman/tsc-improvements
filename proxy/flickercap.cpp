#include "flickercap.h"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "log.h"

namespace tscfix::flickercap {
namespace {

int g_key = 0;
volatile LONG g_trigger = 0;
int g_frames = 60;                 // pictures in the window
std::vector<IDirect3DSurface9*> g_ring;
unsigned g_next = 0, g_stored = 0;  // ring position, pictures stored so far (up to g_frames)
int g_after = -1;                   // frames still to record after the key (-1: not armed)
int g_sets = 0;
RECT g_crop{};  // empty: the whole picture
bool g_down = false;

void Release() {
    for (IDirect3DSurface9* s : g_ring)
        if (s) s->Release();
    g_ring.clear();
    g_next = g_stored = 0;
    g_after = -1;
}

// The pictures go from the graphics card to memory at once (a few ms each) and a helper thread
// writes them as BMP files, so the game does not stop while the files are written.
struct Picture {
    UINT width, height;
    std::vector<uint8_t> pixels;  // 32 bits per pixel, top row first
};
DWORD WINAPI WriteFiles(void* arg) {
    auto* job = static_cast<std::pair<std::string, std::vector<Picture>>*>(arg);
    int written = 0;
    for (size_t i = 0; i < job->second.size(); ++i) {
        const Picture& p = job->second[i];
        char path[MAX_PATH];
        sprintf_s(path, "%s/f%03zu.bmp", job->first.c_str(), i);
        FILE* f = nullptr;
        if (fopen_s(&f, path, "wb") != 0 || !f) continue;
        BITMAPFILEHEADER fh{};
        BITMAPINFOHEADER ih{};
        fh.bfType = 0x4D42;
        fh.bfOffBits = sizeof fh + sizeof ih;
        fh.bfSize = fh.bfOffBits + static_cast<DWORD>(p.pixels.size());
        ih.biSize = sizeof ih, ih.biWidth = static_cast<LONG>(p.width), ih.biHeight = -static_cast<LONG>(p.height);
        ih.biPlanes = 1, ih.biBitCount = 32, ih.biCompression = BI_RGB;
        fwrite(&fh, sizeof fh, 1, f), fwrite(&ih, sizeof ih, 1, f), fwrite(p.pixels.data(), 1, p.pixels.size(), f);
        fclose(f);
        ++written;
    }
    Log("flicker capture: %d pictures written (oldest first) in %s", written, job->first.c_str());
    delete job;
    return 0;
}

void Write(IDirect3DDevice9* device) {
    char dir[MAX_PATH], path[MAX_PATH];
    GetTempPathA(MAX_PATH, dir);
    strcat_s(dir, "tscfix-flicker");
    CreateDirectoryA(dir, nullptr);
    SYSTEMTIME t;
    GetLocalTime(&t);
    sprintf_s(path, "%s/%02d%02d%02d-set%d", dir, t.wHour, t.wMinute, t.wSecond, g_sets++);
    CreateDirectoryA(path, nullptr);
    auto* job = new std::pair<std::string, std::vector<Picture>>(path, {});
    unsigned n = g_stored, size = static_cast<unsigned>(g_ring.size()), first = (g_next + size - n) % size;
    IDirect3DSurface9* memory = nullptr;
    for (unsigned i = 0; i < n; ++i) {
        IDirect3DSurface9* s = g_ring[(first + i) % size];
        if (!s) continue;
        D3DSURFACE_DESC desc{};
        s->GetDesc(&desc);
        if (!memory && FAILED(device->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &memory, nullptr))) break;
        D3DLOCKED_RECT lr{};
        if (FAILED(device->GetRenderTargetData(s, memory)) || FAILED(memory->LockRect(&lr, nullptr, D3DLOCK_READONLY))) continue;
        Picture p{desc.Width, desc.Height, {}};
        p.pixels.resize(static_cast<size_t>(desc.Width) * desc.Height * 4);
        for (UINT y = 0; y < desc.Height; ++y)
            memcpy(&p.pixels[static_cast<size_t>(y) * desc.Width * 4], static_cast<const uint8_t*>(lr.pBits) + static_cast<size_t>(y) * lr.Pitch, desc.Width * 4);
        memory->UnlockRect();
        job->second.push_back(std::move(p));
    }
    if (memory) memory->Release();
    Log("flicker capture: set %d, %zu pictures taken; writing in the background to %s", g_sets - 1, job->second.size(), path);
    if (HANDLE h = CreateThread(nullptr, 0, &WriteFiles, job, 0, nullptr)) CloseHandle(h);
    else delete job;
}

}  // namespace

void Configure(int key, int frames) {
    g_key = key;
    if (frames >= 8 && frames <= 900) g_frames = frames;
}
void SetCrop(int x, int y, int width, int height) {
    if (width > 0 && height > 0) g_crop = {x, y, x + width, y + height};
}
bool Enabled() { return g_key > 0; }
void Trigger() { InterlockedExchange(&g_trigger, 1); }
void OnReset() { Release(); }

void OnPresent(IDirect3DDevice9* device) {
    if (g_key <= 0) return;
    IDirect3DSurface9* back = nullptr;
    if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) || !back) return;
    if (g_ring.empty()) {
        D3DSURFACE_DESC desc{};
        back->GetDesc(&desc);
        if (g_crop.right > static_cast<LONG>(desc.Width) || g_crop.bottom > static_cast<LONG>(desc.Height)) g_crop = {};
        UINT w = g_crop.right ? static_cast<UINT>(g_crop.right - g_crop.left) : desc.Width;
        UINT h = g_crop.right ? static_cast<UINT>(g_crop.bottom - g_crop.top) : desc.Height;
        g_ring.assign(static_cast<size_t>(g_frames), nullptr);
        for (auto& s : g_ring)
            if (FAILED(device->CreateRenderTarget(w, h, desc.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &s, nullptr))) s = nullptr;
        Log("flicker capture: on, key code %d, window of %d pictures of %ux%u", g_key, g_frames, w, h);
    }
    if (IDirect3DSurface9* slot = g_ring[g_next]) {
        device->StretchRect(back, g_crop.right ? &g_crop : nullptr, slot, nullptr, D3DTEXF_NONE);
        g_next = (g_next + 1) % static_cast<unsigned>(g_ring.size());
        if (g_stored < g_ring.size()) ++g_stored;
    }
    back->Release();
    bool down = (GetAsyncKeyState(g_key) & 0x8000) != 0;
    bool triggered = InterlockedExchange(&g_trigger, 0) != 0;
    // (tools) A named event "tscfix-capture" set by a script works as a press, without key input.
    static HANDLE event = CreateEventA(nullptr, FALSE, FALSE, "Local\\tscfix-capture");
    if (event && WaitForSingleObject(event, 0) == WAIT_OBJECT_0) triggered = true;
    if (((down && !g_down) || triggered) && g_after < 0) g_after = g_frames / 3;  // a third of the window comes after the key
    g_down = down;
    if (g_after > 0) --g_after;
    else if (g_after == 0) {
        Write(device);
        g_after = -1;
    }
}

}  // namespace tscfix::flickercap
