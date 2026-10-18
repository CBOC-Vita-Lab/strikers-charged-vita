// Command-buffer capture: see vgx_capture.h.
#include "vgx_capture.h"

#include <map>
#include <type_traits>

namespace vgx::capture {

namespace {
constexpr char kMagic[8] = {'V', 'G', 'X', 'C', 'A', 'P', '2', 0};

// ---- writing
struct Out {
    std::FILE* f = nullptr;
    bool ok = true;
    void raw(const void* p, size_t n) { if (n && std::fwrite(p, 1, n, f) != n) ok = false; }
    void u32(uint32_t v) { raw(&v, 4); }
    void u64(uint64_t v) { raw(&v, 8); }
    template <typename T, typename A> void pod(const std::vector<T, A>& v) {
        static_assert(std::is_trivially_copyable<T>::value, "not a plain record");
        u32(static_cast<uint32_t>(v.size()));
        u32(static_cast<uint32_t>(sizeof(T)));
        raw(v.data(), v.size() * sizeof(T));
    }
    void str(const std::string& s) { u32(static_cast<uint32_t>(s.size())); raw(s.data(), s.size()); }
};
void WriteShader(Out& o, const ShaderSource& s) {
    o.u32(s.fragment); o.u32(s.id); o.u64(s.keyHash); o.u32(s.texMask); o.u32(s.attrMask);
    o.u32(s.usesPalette); o.u32(s.usesLights); o.u32(s.numTexGens);
    o.u32(s.nMv); o.u32(s.nTm); o.u32(s.nPtm); o.u32(s.nPal); o.u32(s.nLt); o.u32(s.nTev);
    o.str(s.text);
}
void WriteUpload(Out& o, const TexUpload& t) {
    o.u32(t.texId); o.u32(t.width); o.u32(t.height); o.u32(t.format); o.u32(t.authored); o.u32(t.genMips);
    o.u32(static_cast<uint32_t>(t.levels.size()));
    for (const auto& l : t.levels) o.pod(l);
}
void WriteBufUpload(Out& o, const BufUpload& b) { o.u32(b.bufId); o.pod(b.vtx); o.pod(b.idx); }
void WriteBuf(Out& o, const CmdBuf& b) {
    o.u64(b.frame);
    o.pod(b.cmds); o.pod(b.draws); o.pod(b.copyTex); o.pod(b.copyDisp); o.pod(b.targets); o.pod(b.passes);
    o.pod(b.vtx); o.pod(b.idx); o.pod(b.uni); o.pod(b.vtxChunks); o.pod(b.variants);
    o.u32(static_cast<uint32_t>(b.uploads.size()));
    for (const TexUpload& t : b.uploads) WriteUpload(o, t);
    o.u32(static_cast<uint32_t>(b.bufUploads.size()));
    for (const BufUpload& u : b.bufUploads) WriteBufUpload(o, u);
    o.u32(static_cast<uint32_t>(b.shaders.size()));
    for (const ShaderSource* s : b.shaders) WriteShader(o, *s);
}

// ---- what is alive (tracked from the first frame on while capture is enabled)
struct Live {
    std::map<uint32_t, TexUpload> tex;
    std::map<uint32_t, BufUpload> buf;
    std::vector<const ShaderSource*> shaders;
    SetTargetCmd target;
    bool haveTarget = false;
} g_live;

struct Window { uint64_t first, last; };
std::vector<Window> g_windows;
std::string g_dir;
bool g_init = false, g_on = false;
// The open window: its frames go to a temporary file while the textures and static buffers
// they refer to are noted; at its end the file is written as preamble (what was alive when
// the window began, of that only what the frames refer to) + frames.
Out g_out;
uint64_t g_openFirst = 0, g_openLast = 0;
std::string g_tmpPath, g_finalPath;
Live* g_snap = nullptr; // what was alive when the window began
std::vector<uint8_t> g_texRef, g_bufRef;

void Init() {
    if (g_init) return;
    g_init = true;
    const char* d = std::getenv("MSC_VGX_CAPTURE");
    const char* l = std::getenv("MSC_VGX_CAPTURE_LIST");
    if (!d || !*d || !l || !*l) return;
    g_dir = d;
    for (const char* p = l; *p;) {
        char* e = nullptr;
        const unsigned long long a = std::strtoull(p, &e, 10);
        if (e == p) break;
        unsigned long long b = a;
        if (*e == '-') { p = e + 1; b = std::strtoull(p, &e, 10); }
        g_windows.push_back(Window{a, b});
        if (*e != ',') break;
        p = e + 1;
    }
    g_on = !g_windows.empty();
    if (g_on) std::fprintf(stderr, "[vgx] capture: %u windows into %s\n", static_cast<unsigned>(g_windows.size()), g_dir.c_str());
}

void Track(const CmdBuf& b) {
    for (const Cmd& c : b.cmds) {
        switch (c.type) {
        case CMD_TEX_UPLOAD: g_live.tex[b.uploads[c.arg].texId] = b.uploads[c.arg]; break;
        case CMD_TEX_FREE: g_live.tex.erase(c.arg); break;
        case CMD_BUF_UPLOAD: g_live.buf[b.bufUploads[c.arg].bufId] = b.bufUploads[c.arg]; break;
        case CMD_BUF_FREE: g_live.buf.erase(c.arg); break;
        case CMD_SHADER: g_live.shaders.push_back(b.shaders[c.arg]); break;
        case CMD_SET_TARGET: g_live.target = b.targets[c.arg]; g_live.haveTarget = true; break;
        default: break;
        }
    }
}

inline void Ref(std::vector<uint8_t>& v, uint32_t id) {
    if (v.size() <= id) v.resize(static_cast<size_t>(id) + 256, 0);
    v[id] = 1;
}
void NoteRefs(const CmdBuf& b) {
    for (const DrawRec& d : b.draws) {
        if (d.vbuf) Ref(g_bufRef, d.vbuf);
        for (int m = 0; m < 8; ++m)
            if ((d.texMask & (1u << m)) && d.tex[m].texId) Ref(g_texRef, d.tex[m].texId);
    }
    for (const CopyTexCmd& c : b.copyTex) {
        if (c.srcTex) Ref(g_texRef, c.srcTex);
    }
}
void WritePreamble(Out& o, const Live& live, uint64_t frame, uint32_t& nTex, uint32_t& nBuf) {
    CmdBuf p;
    p.frame = frame;
    if (live.haveTarget) {
        p.targets.push_back(live.target);
        p.cmds.push_back(Cmd{CMD_SET_TARGET, 0});
    }
    for (const ShaderSource* s : live.shaders) {
        p.shaders.push_back(s);
        p.cmds.push_back(Cmd{CMD_SHADER, static_cast<uint32_t>(p.shaders.size() - 1)});
    }
    for (const auto& kv : live.tex) {
        if (!(kv.first < g_texRef.size() && g_texRef[kv.first])) continue;
        p.uploads.push_back(kv.second);
        p.cmds.push_back(Cmd{CMD_TEX_UPLOAD, static_cast<uint32_t>(p.uploads.size() - 1)});
    }
    for (const auto& kv : live.buf) {
        if (!(kv.first < g_bufRef.size() && g_bufRef[kv.first])) continue;
        p.bufUploads.push_back(kv.second);
        p.cmds.push_back(Cmd{CMD_BUF_UPLOAD, static_cast<uint32_t>(p.bufUploads.size() - 1)});
    }
    nTex = static_cast<uint32_t>(p.uploads.size());
    nBuf = static_cast<uint32_t>(p.bufUploads.size());
    WriteBuf(o, p);
}
void CloseWindow() {
    std::fclose(g_out.f);
    const bool framesOk = g_out.ok;
    g_out = Out{};
    Out fin;
    fin.f = std::fopen(g_finalPath.c_str(), "wb");
    std::FILE* tmp = std::fopen(g_tmpPath.c_str(), "rb");
    uint32_t nTex = 0, nBuf = 0;
    if (fin.f && tmp && g_snap) {
        fin.raw(kMagic, 8);
        WritePreamble(fin, *g_snap, g_openFirst, nTex, nBuf);
        static char block[1 << 16];
        for (size_t n; (n = std::fread(block, 1, sizeof block, tmp)) > 0;) fin.raw(block, n);
    } else {
        fin.ok = false;
    }
    if (tmp) std::fclose(tmp);
    if (fin.f) std::fclose(fin.f);
    std::remove(g_tmpPath.c_str());
    std::fprintf(stderr, "[vgx] capture: window %llu-%llu -> %s (%u textures and %u static buffers from before the window)%s\n",
                 static_cast<unsigned long long>(g_openFirst), static_cast<unsigned long long>(g_openLast), g_finalPath.c_str(), nTex, nBuf,
                 (fin.ok && framesOk) ? "" : "  WRITE ERROR");
    delete g_snap;
    g_snap = nullptr;
}
} // namespace

bool Enabled() {
    Init();
    return g_on;
}

void OnFrame(const CmdBuf& buf) {
    Init();
    if (!g_on) return;
    if (!g_out.f) {
        for (const Window& w : g_windows) {
            if (buf.frame != w.first) continue;
            char path[600];
            std::snprintf(path, sizeof path, "%s/cap_%06llu_%06llu.vgxcap", g_dir.c_str(), static_cast<unsigned long long>(w.first),
                          static_cast<unsigned long long>(w.last));
            g_finalPath = path;
            g_tmpPath = g_finalPath + ".tmp";
            g_out.f = std::fopen(g_tmpPath.c_str(), "wb");
            g_out.ok = g_out.f != nullptr;
            if (!g_out.f) { std::fprintf(stderr, "[vgx] capture: cannot write %s\n", g_tmpPath.c_str()); break; }
            g_openFirst = w.first;
            g_openLast = w.last;
            g_snap = new Live(g_live);
            g_texRef.clear();
            g_bufRef.clear();
            break;
        }
    }
    if (g_out.f) {
        WriteBuf(g_out, buf);
        NoteRefs(buf);
        if (buf.frame >= g_openLast) CloseWindow();
    }
    Track(buf);
}

// ---- reading
namespace {
struct In {
    std::FILE* f = nullptr;
    bool ok = true;
    void raw(void* p, size_t n) { if (n && std::fread(p, 1, n, f) != n) ok = false; }
    uint32_t u32() { uint32_t v = 0; raw(&v, 4); return v; }
    uint64_t u64() { uint64_t v = 0; raw(&v, 8); return v; }
    template <typename T, typename A> void pod(std::vector<T, A>& v) {
        const uint32_t n = u32(), sz = u32();
        if (!ok || sz != sizeof(T)) { ok = false; return; }
        v.resize(n);
        raw(v.data(), static_cast<size_t>(n) * sizeof(T));
    }
    void str(std::string& s) { const uint32_t n = u32(); if (!ok) return; s.resize(n); raw(s.data(), n); }
};
void ReadShader(In& i, ShaderSource& s) {
    s.fragment = i.u32() != 0; s.id = static_cast<uint16_t>(i.u32()); s.keyHash = i.u64(); s.texMask = static_cast<uint8_t>(i.u32());
    s.attrMask = i.u32(); s.usesPalette = i.u32() != 0; s.usesLights = i.u32() != 0; s.numTexGens = static_cast<uint8_t>(i.u32());
    s.nMv = static_cast<uint8_t>(i.u32()); s.nTm = static_cast<uint8_t>(i.u32()); s.nPtm = static_cast<uint8_t>(i.u32());
    s.nPal = static_cast<uint8_t>(i.u32()); s.nLt = static_cast<uint8_t>(i.u32()); s.nTev = static_cast<uint8_t>(i.u32());
    i.str(s.text);
}
bool ReadBuf(In& i, CmdBuf& b, std::deque<ShaderSource>& store) {
    b.frame = i.u64();
    if (!i.ok) return false; // (end of file)
    i.pod(b.cmds); i.pod(b.draws); i.pod(b.copyTex); i.pod(b.copyDisp); i.pod(b.targets); i.pod(b.passes);
    i.pod(b.vtx); i.pod(b.idx); i.pod(b.uni); i.pod(b.vtxChunks); i.pod(b.variants);
    uint32_t n = i.u32();
    for (uint32_t k = 0; k < n && i.ok; ++k) {
        TexUpload t;
        t.texId = i.u32(); t.width = static_cast<uint16_t>(i.u32()); t.height = static_cast<uint16_t>(i.u32());
        t.format = static_cast<uint8_t>(i.u32()); t.authored = static_cast<uint8_t>(i.u32()); t.genMips = i.u32() != 0;
        const uint32_t nl = i.u32();
        t.levels.resize(i.ok ? nl : 0);
        for (auto& l : t.levels) i.pod(l);
        b.uploads.push_back(std::move(t));
    }
    n = i.u32();
    for (uint32_t k = 0; k < n && i.ok; ++k) {
        BufUpload u;
        u.bufId = i.u32(); i.pod(u.vtx); i.pod(u.idx);
        b.bufUploads.push_back(std::move(u));
    }
    n = i.u32();
    for (uint32_t k = 0; k < n && i.ok; ++k) {
        store.emplace_back();
        ReadShader(i, store.back());
        b.shaders.push_back(&store.back());
    }
    return i.ok;
}
} // namespace

bool Load(const char* path, File& out) {
    In in;
    in.f = std::fopen(path, "rb");
    if (!in.f) return false;
    char magic[8] = {};
    in.raw(magic, 8);
    if (!in.ok || std::memcmp(magic, kMagic, 8) != 0) { std::fclose(in.f); return false; }
    for (;;) {
        CmdBuf b;
        if (!ReadBuf(in, b, out.shaderStore)) break;
        out.frames.push_back(std::move(b));
    }
    std::fclose(in.f);
    return out.frames.size() >= 2;
}

} // namespace vgx::capture
