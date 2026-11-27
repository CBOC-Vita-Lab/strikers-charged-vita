// Prints the Settings screen's model for the audit scripts (audit_settings_keys.py, check_settings_effective.py):
//   rows            one line per choice:  row|choice index|label|KEY=VALUE KEY=VALUE   (the default choice has no assignments)
//   combo N         the text with choice N of every row (the last choice when a row has fewer), written by the real Serialize
//   pick ROW=N ... those rows at those choices over the defaults (ROW is the title)
//   choice ROW N    the text with only choice N of the row ROW (by title)
//   clang++ -std=c++17 -I../src settings_dump.cpp ../src/np_settings.cpp ../src/vita_plan.cpp -o settings_dump
#include "np_settings.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace NpSettings;

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "rows";
    State s;
    Load(s, nullptr);
    std::string out;
    if (mode == "rows") {
        for (int o = 0; o < OptionCount(); ++o)
            for (int c = 0; c < ChoiceCount(o); ++c) {
                std::printf("%s|%d|%s|", GetOption(o).title, c, GetOption(o).choices[c].label);
                bool first = true;
                for (const Assign& a : GetOption(o).choices[c].set) {
                    if (!a.key) break;
                    std::printf("%s%s=%s", first ? "" : " ", a.key, a.value ? a.value : "");
                    first = false;
                }
                std::printf("\n");
            }
        return 0;
    }
    if (mode == "combo" && argc > 2) {
        const int n = std::atoi(argv[2]);
        for (int o = 0; o < OptionCount(); ++o) SetChoice(s, o, n < ChoiceCount(o) ? n : ChoiceCount(o) - 1);
    } else if (mode == "pick") {   // pick "ROW=N" ...: those rows at those choices over the defaults
        for (int a = 2; a < argc; ++a) {
            const std::string item = argv[a];
            const size_t eq = item.rfind('=');
            if (eq == std::string::npos) return 2;
            const int o = OptionIndex(item.substr(0, eq).c_str());
            if (o < 0) { std::fprintf(stderr, "no row %s\n", item.c_str()); return 2; }
            SetChoice(s, o, std::atoi(item.c_str() + eq + 1));
        }
    } else if (mode == "choice" && argc > 3) {
        for (int o = 0; o < OptionCount(); ++o)
            if (!std::strcmp(GetOption(o).title, argv[2])) SetChoice(s, o, std::atoi(argv[3]));
    } else {
        std::fprintf(stderr, "usage: settings_dump rows | combo N | pick ROW=N ... | choice ROW N\n");
        return 2;
    }
    if (!Serialize(s, out)) {
        std::fprintf(stderr, "does not fit the 96-assignment limit\n");
        return 1;
    }
    std::fputs(out.c_str(), stdout);
    return 0;
}
