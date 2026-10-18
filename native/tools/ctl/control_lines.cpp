// tools/ctl/control_lines.cpp (W5 CTL): the controls table (platform/sdk/input/np_control_table.h) as a list, and as a harness script that
// tries every line's input in its context in the real game and checks that the engine did the action.
//
//   g++ -std=gnu++11 -I platform/sdk/input tools/ctl/control_lines.cpp platform/sdk/input/np_controls.cpp -o control_lines
//   control_lines list   <modern|classic> [rear=0|1] [touch=0|1]   the lines as the controls page shows them
//   control_lines script <modern|classic> [rear=0|1] [touch=0|1] [skill=N] [only=mega]
//                                                                    the script (MSC_ITEM_TEST=1 for the item lines; run the game with
//                                                                    the same MSC_CONTROLS / MSC_REAR_TOUCH / touch switches)
// The script: the menu lines in the main menu first, then a 10-minute friendly; every check is tried (each in its context, up to 4 times)
// until all are decided ($open == 0). Results: "[script] CHECK RESULT <name> PASS|FAIL ..." lines and "[script] checks: ..." at exit.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "np_control_table.h"

using namespace NpControlTable;

namespace {

const char* CtxName(Ctx c) {
    switch (c) {
    case kAttack: return "with the ball";
    case kDefence: return "they have the ball";
    case kCaptain: return "captain with the ball";
    case kMeter: return "own Mega Strike meter";
    case kSave: return "Mega Save";
    case kShake: return "their Mega Strike meter";
    case kPlay: return "open play";
    case kPaused: return "pause menu";
    case kMenu: return "main menu";
    case kSubMenu: return "options menu";
    }
    return "?";
}
const char* CtxCond(Ctx c) {
    switch (c) {
    case kAttack: return "chave == 1 && cplayer != 4 && cplayer != 0 && cmode == 0 && paused == 0";
    case kDefence: return "cball == 1 && cmode == 0 && paused == 0";
    case kCaptain: return "chave == 1 && cplayer == 0 && cmode == 0 && paused == 0";
    case kMeter: return "cmode == 2";
    case kSave: return "cmode == 1";
    case kShake: return "cmode == 3";
    case kPlay: return "gs == 5 && cmode == 0 && paused == 0";
    case kPaused: return "paused == 1";
    case kMenu: return "scene == 1 && nbuttons >= 8";
    case kSubMenu: return "scene == 27 && nbuttons >= 8";
    }
    return "f < 0";
}
// how many failed tries decide a check: the captain's wind-up is often cut short by a tackle, which is no verdict on the button
int MaxFails(Ctx c) { return c == kCaptain ? 12 : 4; }
int Timeout(Ctx c) {
    switch (c) {
    case kMenu: case kSubMenu: return 150;
    case kPlay: case kPaused: return 90;
    case kCaptain: return 60;
    default: return 45;
    }
}
std::vector<std::string> Split(const char* s) {
    std::vector<std::string> out;
    std::string cur;
    for (const char* p = s; p && *p; ++p) {
        if (*p == ';') {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += *p;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}
struct Item {
    std::string name;
    const Line* line;
    const Test* test;
    std::string cellText;
};

std::vector<Item> Items(const Config& cfg) {
    std::vector<Item> out;
    char buf[256];
    for (int i = 0; i < LineCount(); ++i) {
        const Line& l = LineAt(i);
        if (!Shown(l, cfg)) continue;
        for (int t = 0; t < 6 && l.tests[t].drive; ++t) {
            const Test& x = l.tests[t];
            if (!(x.layouts & LayoutBit(cfg.layout))) continue;
            Item it;
            char nm[32];
            snprintf(nm, sizeof nm, "L%dT%d", i, t);
            it.name = nm;
            it.line = &l;
            it.test = &x;
            if (IsSentence(l)) it.cellText = Sentence(l, cfg, buf, sizeof buf);
            else it.cellText = (x.ctx == kDefence) ? l.withoutBall : l.withBall;
            out.push_back(it);
        }
    }
    return out;
}

void Block(const Item& it, const Config& cfg, bool active) {
    const Test& x = *it.test;
    std::string drive = x.drive;
    std::string extra;
    if (!drive.empty() && drive[0] == '?') {
        const size_t semi = drive.find(';');
        extra = " && " + drive.substr(1, semi - 1);
        drive = semi == std::string::npos ? "" : drive.substr(semi + 1);
    }
    std::string counter = x.counter, op = "up";
    if (counter[0] == '=') { op = "same"; counter = counter.substr(1); }
    else if (counter[0] == '-') { op = "down"; counter = counter.substr(1); }
    else if (counter[0] == '~') { op = "changed"; counter = counter.substr(1); }
    // a line whose switch is off must do nothing: its input is tried and the counter must stay
    if (!active) op = "same";
    printf("if $%s == 0 && $%sf < %d && %s%s\n", it.name.c_str(), it.name.c_str(), MaxFails(x.ctx), CtxCond(x.ctx), extra.c_str());
    printf("log TRY %s\n", it.name.c_str());
    for (const std::string& s : Split(x.setup)) printf("%s\n", s.c_str());
    printf("expect %s %s\n", counter.c_str(), op.c_str());
    for (const std::string& s : Split(drive.c_str())) printf("%s\n", s.c_str());
    printf("check %s %d\n", it.name.c_str(), Timeout(x.ctx));
    printf("endif\n");
    (void)cfg;
}

int g_skill = 2;          // the CPU's skill (skill=N): 2 brings the opponent's Mega Strikes (save, shakedown); 0 lets the captain wind up
bool g_megaOnly = false;  // only=mega: only the own Mega Strike's checks (the captain's wind-up, the meter)

void Script(const Config& cfg) {
    std::vector<Item> items = Items(cfg);
    if (g_megaOnly) {
        std::vector<Item> keep;
        for (const Item& it : items)
            if (it.test->ctx == kCaptain || it.test->ctx == kMeter) keep.push_back(it);
        items = keep;
    }
    const char* layout = cfg.layout == 1 ? "modern" : "classic";
    printf("# W5 CTL generated by tools/ctl/control_lines.cpp (tools/ctl/gen_lines.sh) from platform/sdk/input/np_control_table.h: every line of the controls page\n");
    printf("# (%s, rear touch %s, touch minigames %s) pressed in its context; the engine's action is read from the [ctl] counters.\n", layout,
           cfg.rear ? "on" : "off", cfg.touchMeter ? "on" : "off");
    printf("# Run with MSC_CONTROLS=%s MSC_REAR_TOUCH=%d%s MSC_ITEM_TEST=1.\n", layout, cfg.rear ? 1 : 0, cfg.touchMeter ? "" : " MSC_TOUCH_MINIGAMES=0");
    for (const Item& it : items) {
        const Test& x = *it.test;
        const bool active = NeedsMet(x.need, cfg) && NeedsMet(it.line->need, cfg);
        printf("checkdef %s %d %s | %s | %s: %s -> %s%s\n", it.name.c_str(), MaxFails(x.ctx), layout, it.line->input, CtxName(x.ctx), it.cellText.c_str(),
               x.counter, active ? "" : " (switched off: must do nothing)");
    }
    // the front end
    printf("vmash cross 20 scene == 0\nlog STATE title screen\nvmash cross 30 scene != 0\nwait scene == 1\nwait scene == 10 || nbuttons >= 8\n"
           "if scene == 10\nwait nbuttons >= 2\nidle 10\nvmash cross 30 scene == 1\nwait nbuttons >= 8\nendif\nwait onctx == 0 1800\n"
           "log STATE main menu\nidle 30\n");
    printf("repeat 6\n");
    for (const Item& it : items)
        if (it.test->ctx == kMenu || it.test->ctx == kSubMenu) {
            Block(it, cfg, NeedsMet(it.test->need, cfg) && NeedsMet(it.line->need, cfg));
            printf("try wait nbuttons >= 8 900\nidle 30\n");
        }
    // back to the main menu (the back button of the layout)
    printf("if scene == 27\nvpress %s 3 10\ntry wait scene == 1 600\ntry wait nbuttons >= 8 900\nidle 30\nendif\n", cfg.layout == 1 ? "circle" : "square");
    printf("if scene == 1\nidle 10\nendif\nendrepeat\nlog STATE menus done\n");
    // the match
    printf("launch mode=friendly stadium=0 home=0 away=7 hsk=0,1,2 ask=3,4,5 human=0 skill=%d time=600\n", g_skill);
    printf("wait ingame == 1 4000\nlog STATE match object\nwait gs == 5 6000\nlog STATE open play\nstopwhen $open == 0\n");
    printf("repeat 4000\n");
    // the minigames first (they come and go), then the pause, the captain, attack and defence
    static const Ctx order[] = {kSave, kShake, kMeter, kCaptain, kMeter, kPlay, kPaused, kAttack, kDefence};
    for (Ctx c : order)
        for (const Item& it : items)
            if (it.test->ctx == c) Block(it, cfg, NeedsMet(it.test->need, cfg) && NeedsMet(it.line->need, cfg));
    // keep the match going between the checks: survive the minigames, resume, get the ball to the captain, chase the ball
    const char* shoot = cfg.layout == 1 ? "circle" : "square";
    printf("if cmode == 1\ntry vmash cross 8 cmode != 1 600\nendif\n");
    printf("if cmode == 2\ntry vmash %s 10 cmode != 2 300\nendif\n", shoot);
    printf("if cmode == 3\ntry vmash select 6 cmode != 3 600\nendif\n");
    printf("if paused == 1\nvpress start 3 30\nendif\n");
    printf("if chave == 1 && cplayer == 4\nvpress cross 3 20\nendif\n");
    printf("if chave == 1 && cplayer != 4 && cplayer != 0\nvchase 10 none goal\nvpress cross 3 15\nendif\n");
    printf("if chave == 1 && cplayer == 0\nvchase 20 none goal\nvchase 6 %s goal\nendif\n", shoot);
    printf("if chave == 0 && cplayer != 0 && cball != 1\nvhold 18 cross\nendif\n");
    printf("if chave == 0\nvchase 12 none ball\nendif\n");
    printf("idle 2\nendrepeat\nlog STATE script end\nexit\n");
}

void List(const Config& cfg) {
    char buf[256];
    printf("%s, rear touch %s, touch minigames %s\n", cfg.layout == 1 ? "MODERN" : "CLASSIC", cfg.rear ? "on" : "off", cfg.touchMeter ? "on" : "off");
    for (int i = 0; i < LineCount(); ++i) {
        const Line& l = LineAt(i);
        if (!Shown(l, cfg)) continue;
        const bool on = Active(l, cfg);
        if (IsSentence(l)) printf("  %-20s %s\n", l.input, Sentence(l, cfg, buf, sizeof buf));
        else printf("  %-20s %-36s %s\n", l.input, on ? l.withBall : "(REAR TOUCH OFF)", on ? l.withoutBall : "");
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: control_lines list|script modern|classic [rear=0|1] [touch=0|1]\n");
        return 2;
    }
    Config cfg;
    cfg.layout = !strcmp(argv[2], "classic") ? 0 : 1;
    cfg.rear = cfg.layout == 1;
    cfg.touchSave = cfg.touchMeter = cfg.touchShake = cfg.touchPointer = true;
    for (int i = 3; i < argc; ++i) {
        if (!strncmp(argv[i], "rear=", 5)) cfg.rear = argv[i][5] == '1';
        if (!strncmp(argv[i], "touch=", 6)) cfg.touchSave = cfg.touchMeter = cfg.touchShake = argv[i][6] == '1';
        if (!strncmp(argv[i], "skill=", 6)) g_skill = atoi(argv[i] + 6);
        if (!strcmp(argv[i], "only=mega")) g_megaOnly = true;
    }
    if (!strcmp(argv[1], "list")) List(cfg);
    else Script(cfg);
    return 0;
}
