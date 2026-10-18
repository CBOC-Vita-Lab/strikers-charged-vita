#include <GLES2/gl2.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>
#include "../src/vgx_backend_gl.cpp"

namespace vgx { bool LogEnabled() { return false; } }

static void Check(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}

int main(int argc, char** argv) {
    Check(argc == 2, "test mode required");
    const std::string mode = argv[1];
    const bool off = mode == "off", bad = mode == "bad", missing = mode == "missing", late = mode == "late";
    if (off) unsetenv("MSC_VGX_VALIDATE"); else setenv("MSC_VGX_VALIDATE", "1", 1);
    FILE* capture = std::tmpfile();
    Check(capture != nullptr, "stderr capture available");
    int saved = dup(STDERR_FILENO);
    Check(saved >= 0 && dup2(fileno(capture), STDERR_FILENO) >= 0, "stderr redirected");
    vgx::backend::Init(640, 528);
    vgx::ShaderSource vs, fs;
    vs.id = fs.id = 1; vs.nMv = 6; vs.keyHash = 123;
    fs.fragment = true; fs.nTev = 0; fs.keyHash = 456;
    vs.text = "void main() {}"; fs.text = "void main() {}";
    vgx::CmdBuf before;
    before.frame = 0;
    before.shaders = {&vs, &fs};
    before.cmds = {{vgx::CMD_SHADER, 0}, {vgx::CMD_SHADER, 1}, {vgx::CMD_BUF_UPLOAD, 0}};
    if (missing) before.cmds.pop_back();
    vgx::BufUpload upload; upload.bufId = 3; upload.vtx.resize(48); upload.idx = {0, 1, static_cast<uint16_t>((bad || late) ? 3 : 2)};
    upload.vtx[0] = 1;
    before.bufUploads.push_back(upload);
    vgx::backend::Execute(before);
    before.bufUploads[0].vtx[0] = 0xff;
    vgx::CmdBuf selected;
    selected.frame = 1;
    selected.vtx.assign(80, 0); selected.vtxChunks = {0, 32}; selected.idx = {0, 1, 2}; selected.uni.assign(40, 0.0f);
    selected.vtx[0] = 0xff;
    vgx::DrawRec draw;
    draw.vsId = draw.fsId = 1; draw.vtxCount = 3; draw.idxCount = 3;
    draw.ublock[vgx::UB_PROJ] = 0; draw.ublock[vgx::UB_MV] = 16;
    draw.vbuf = 3; selected.draws.push_back(draw);
    draw.vbuf = 0; draw.vchunk = 1; selected.draws.push_back(draw);
    for (unsigned i = 0; i < 100; ++i) selected.cmds.push_back({vgx::CMD_DRAW, late ? (i >= 16 ? 0u : 1u) : (i ? 1u : 0u)});
    vgx::backend::Execute(selected);
    Check(vgx::backend::GetStats().drawCallsThisFrame == (missing ? 99u : 100u), "validation does not skip or add actual draws");
    selected.frame = 2;
    vgx::backend::Execute(selected);
    std::fflush(stderr);
    Check(dup2(saved, STDERR_FILENO) >= 0, "stderr restored");
    close(saved);
    std::rewind(capture);
    std::string log;
    char block[4096];
    while (size_t count = std::fread(block, 1, sizeof block, capture)) log.append(block, count);
    std::fclose(capture);
    if (off) {
        Check(log.find("[validate]") == std::string::npos, "default backend has no validation output");
    } else {
        Check(log.find("[validate] frame=1 summary draws=100") != std::string::npos, "selected frame reaches actual backend validator");
        Check(log.find(late ? "bad_draws=84" : (bad ? "bad_draws=1 " : "bad_draws=0")) != std::string::npos, "static and streamed draw results summarized");
        Check(log.find("[validate] frame=0") == std::string::npos && log.find("[validate] frame=2") == std::string::npos, "unselected frames stay quiet");
        size_t lines = 0, at = 0;
        while ((at = log.find("[validate] frame=1 draw=", at)) != std::string::npos) { ++lines; ++at; }
        Check(lines == (late ? 24u : 16u), "per-frame detail and later anomaly logs are bounded");
        if (missing) Check(log.find("unchecked_draws=1") != std::string::npos, "missing static shadow is unverified, not a proved bad index");
        if (bad) Check(log.find("bad_vertex_refs=1") != std::string::npos, "bad static indices observed at backend boundary");
        else if (!missing && !late) {
            size_t staticAt = log.find("source=static:3");
            Check(staticAt != std::string::npos, "static source identified");
            const std::string staticLine = log.substr(staticAt, log.find('\n', staticAt) - staticAt);
            Check(staticLine.find("vhash=57a3e9bec8b15e24") != std::string::npos, "static upload shadow, not its mutated source or stream, is inspected");
        }
        Check(log.find("vhash=a09d945a1cd8d6e5") != std::string::npos, "stream chunk excludes unrelated bytes and has stable zero-vertex hash");
    }
    std::printf("PASS: real backend validation integration (%s), bounded logs and selected-frame gating\n", argv[1]);
}
