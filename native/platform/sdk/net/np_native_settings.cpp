// The engine-free model of the game's native settings screen (np_native_settings.h). Everything about the settings stays in NpSettings (platform/vita/src/np_settings.cpp):
// this file only names which options the player rows are (set18: every player setting, five pages), cuts their text, and saves through the same Serialize + stamp the pixel screen uses.
#include "platform/sdk/net/np_native_settings.h"

#include "platform/sdk/include/np/np_text.h"
#include "platform/vita/src/np_build_identity.h"
#include "platform/vita/src/np_settings.h"
#include "platform/vita/src/np_settings_guard.h"
#include "../../../include/np/live_settings.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

extern "C" int NpLiveSettingsSet(const char* key, const char* valueOrNull) __attribute__((weak));   // np_live_settings.cpp (a test build without it applies nothing)

namespace NpNativeSettings {
namespace {

constexpr int kFileLimit = 64 * 1024;   // the technical screen reads this much of the file

// One text in three languages (the port's own words for this screen; the option titles and choice labels come from np_text_strings.tsv through NpText::In).
struct Text3 {
    const char* en;
    const char* es;
    const char* fr;
};

const char* Pick(int language, const Text3& t) {
    const char* s = language == NpText::kSpanish ? t.es : language == NpText::kFrench ? t.fr : t.en;
    return s && *s ? s : t.en;
}

enum RowKind { kRowOption = 0, kRowAccount, kRowReset };
struct RowDef {
    const char* option;   // title of the option in np_settings.cpp kTable; null for the ONLINE ACCOUNT and RESET rows
    Text3 name;
    RowKind kind;
};

// set18 (owner, 23 Nov: "the settings menu is native in game ... Remove any settings that aren't needed or are just pure negative ... Simplify where able"): every player setting is
// here, five rows a page, the most used first (1.3, owner 15 Nov: the first page opens with the two unlock rows). MAIN, PICTURE, TOUCH, MATCH, ONLINE. Every choice of a row is
// reachable (the row table itself has no hidden choices any more). The pixel screen keeps only SAFE START (L + R at start-up) and ONLINE (the ONLINE ACCOUNT row here).
constexpr RowDef kRows[kPages][kMaxRowsPerPage] = {
    {{"UNLOCK ALL CHARACTERS", {"ALL CHARACTERS", "TODOS PERSONAJES", "TOUS PERSONNAGES"}, kRowOption},
     {"UNLOCK ALL STADIUMS", {"ALL STADIUMS", "TODOS ESTADIOS", "TOUS LES STADES"}, kRowOption},
     {"FRAME RATE", {"FRAME RATE", "FOTOGRAMAS", "IMAGES/S"}, kRowOption},
     {"CONTROLS", {"CONTROLS", "CONTROLES", "COMMANDES"}, kRowOption},
     {"LANGUAGE", {"LANGUAGE", "IDIOMA", "LANGUE"}, kRowOption}},
    {{"STADIUM CROWD", {"CROWD", "PÚBLICO", "PUBLIC"}, kRowOption},
     {"SHARPNESS", {"SHARPNESS", "NITIDEZ", "NETTETÉ"}, kRowOption},
     {"DEPTH OF FIELD", {"BLUR", "DESENFOQUE", "FLOU"}, kRowOption},
     {"HD CHARACTERS", {"HD CHARACTERS", "PERSONAJES HD", "PERSONNAGES HD"}, kRowOption},
     {nullptr, {nullptr, nullptr, nullptr}, kRowOption}},
    {{"REAR TOUCH", {"REAR TOUCH", "TÁCTIL", "TACTILE"}, kRowOption},
     {"MEGA SAVE AIM", {"MEGA SAVE", "PARADA", "ARRÊT"}, kRowOption},
     {"MEGA SAVE SPEED", {"SAVE SPEED", "VEL. PARADA", "VITESSE ARRÊT"}, kRowOption},
     {"TOUCH MEGA STRIKE", {"TOUCH STRIKE", "TRALLAZO TÁCTIL", "FRAPPE TACTILE"}, kRowOption},
     {"TOUCH SHAKEDOWN", {"TOUCH SHAKEDOWN", "SACUDIDA TÁCTIL", "SECOUSSE TACTILE"}, kRowOption}},
    {{"REDUCED CAMERA SHAKE", {"LESS SHAKE", "MENOS TEMBLOR", "MOINS SECOUSSES"}, kRowOption},
     {"MEGA STRIKES", {"MEGA STRIKES", "SUPERTRALLAZOS", "MÉGA FRAPPES"}, kRowOption},
     {"ORIGINAL GAME FIXES", {"GAME FIXES", "ARREGLOS", "CORRECTIFS"}, kRowOption},
     {"POINTER SPEED", {"POINTER SPEED", "VEL. PUNTERO", "VITESSE POINTEUR"}, kRowOption},
     {nullptr, {nullptr, nullptr, nullptr}, kRowOption}},
    {{"CONNECTION", {"CONNECTION", "CONEXIÓN", "CONNEXION"}, kRowOption},
     {nullptr, {"ONLINE ACCOUNT", "CUENTA EN LÍNEA", "COMPTE EN LIGNE"}, kRowAccount},
     {nullptr, {"RESET TO DEFAULTS", "RESTABLECER TODO", "RÉTABLIR PAR DÉFAUT"}, kRowReset},
     {nullptr, {nullptr, nullptr, nullptr}, kRowOption},
     {nullptr, {nullptr, nullptr, nullptr}, kRowOption}},
};
constexpr int kRowsOnPage[kPages] = {5, 4, 5, 4, 3};

// The one-line help of the two rows that are not options (an option's is its help line in np_settings.cpp, translated by np_text_strings.tsv).
const Text3 kAccountHelp = {"Your online name, problem reports, and deleting your online account.", "Tu nombre en línea, los informes de problemas y borrar tu cuenta en línea.",
                            "Votre nom en ligne, les rapports de problème et la suppression du compte."};
const Text3 kResetHelp = {"Puts every setting back to its default. Nothing is written until you save.", "Devuelve cada ajuste a su valor inicial. Nada se escribe hasta guardar.",
                          "Remet chaque réglage par défaut. Rien n'est écrit avant d'enregistrer."};

const Text3 kCustom = {"CUSTOM", "OTRO", "AUTRE"};   // (the table's PERSONALIZADO / PERSONNALISÉ do not fit a row)
// Choice labels too long for "NAME: VALUE" in 24 characters, in the row's words (keyed by the English label of the table).
const struct {
    const char* english;
    Text3 text;
} kShortValues[] = {{"DRAWING ONLY", {"DRAWING", "GRÁFICOS", "AFFICHAGE"}},
                    {"FRONT TOUCH", {"FRONT TOUCH", "TÁCTIL FRONTAL", "TACTILE AVANT"}}};

const Text3 kMore = {"MORE", "MÁS", "PLUS"};
const Text3 kDone = {"DONE", "LISTO", "TERMINÉ"};
const Text3 kLeaveMessage = {
    "Changes apply the next time the game starts. SAVE AND RESTART ends this match now. Circle: keep editing.",
    "Los cambios se aplican al volver a iniciar el juego. GUARDAR Y REINICIAR termina este partido ya. Círculo: seguir editando.",
    "Les changements s'appliquent au prochain lancement. ENREGISTRER ET RELANCER met fin à ce match maintenant. Cercle: continuer."};
const Text3 kLeaveNoteMatch = {"Some changes apply the next time the game starts.", "Algunos cambios se aplican al volver a abrir el juego.", "Certains changements s'appliquent au prochain lancement."};
const Text3 kLeaveNoteAgain = {"The game starts again to apply the changes.", "El juego vuelve a abrirse para aplicar los cambios.", "Le jeu se relance pour appliquer les changements."};
const Text3 kSaveFailed = {"Could not save the settings.", "No se pudieron guardar los ajustes.", "Impossible d'enregistrer les réglages."};

// 1.3 (ux13, U2): after SAVE in a running match, when a row that waits for the next start changed, ask "start the game again now?" (the match would be lost, so it is a
// question of its own, never part of the first popup). MSC_PAUSE_RESTART=1 turns it on; default off until the console test.
const Text3 kRestartAsk = {
    "Start the game again now to apply the changes? You leave this match: progress since your last save is lost. Your save file is not touched.",
    "¿Volver a abrir el juego ahora para aplicar los cambios? Sales de este partido: se pierde el avance desde tu último guardado. Tu archivo de guardado no se toca.",
    "Relancer le jeu maintenant pour appliquer les changements ? Vous quittez ce match : la progression depuis votre dernière sauvegarde est perdue. Votre sauvegarde n'est pas touchée."};
const Text3 kNotNow = {"NOT NOW", "AHORA NO", "PAS MAINTENANT"};
const Text3 kStartAgain = {"START AGAIN", "VOLVER A ABRIR", "RELANCER"};

NpSettings::State g_state;
bool g_open = false;
int g_language = NpText::kEnglish;

int Copy(const char* s, char* out, int cap) {
    if (!out || cap <= 0) return 0;
    int n = 0;
    while (s[n] && n < cap - 1) ++n;
    while (n > 0 && s[n] != 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;   // never cut inside a UTF-8 sequence
    std::memcpy(out, s, static_cast<size_t>(n));
    out[n] = 0;
    return n;
}

bool ValidRow(int page, int row) { return page >= 0 && page < kPages && row >= 0 && row < kRowsOnPage[page]; }

int OptionOf(int page, int row) {
    if (!ValidRow(page, row) || !kRows[page][row].option) return -1;
    return NpSettings::OptionIndex(kRows[page][row].option);
}

bool ReadFileText(std::string& text) {
    if (!NpNativeSettingsBackendRead) return false;
    std::string buffer(static_cast<size_t>(kFileLimit) + 1, '\0');
    int length = 0;
    if (!NpNativeSettingsBackendRead(&buffer[0], kFileLimit, &length) || length < 0 || length > kFileLimit) return false;
    buffer.resize(static_cast<size_t>(length));
    text = buffer;
    return true;
}

bool LoadFromBackend() {
    std::string text;
    if (!ReadFileText(text)) return false;
    NpSettings::Load(g_state, text.c_str());
    return true;
}

}  // namespace

bool Open(int language) {
    g_open = false;
    if (language < 0) language = NpText::CurrentLanguage();
    g_language = language >= NpText::kEnglish && language <= NpText::kFrench ? language : NpText::kEnglish;
    for (int p = 0; p < kPages; ++p)
        for (int r = 0; r < kRowsOnPage[p]; ++r)
            if (kRows[p][r].option && NpSettings::OptionIndex(kRows[p][r].option) < 0) return false;   // a row names an option that does not exist: not offered
    if (!LoadFromBackend()) return false;
    g_open = true;
    return true;
}

bool IsOpen() { return g_open; }
void Close() { g_open = false; }

bool Reload() {
    if (!g_open) return false;
    if (!LoadFromBackend()) {
        g_open = false;
        return false;
    }
    return true;
}

int Pages() { return kPages; }
int Rows(int page) { return page >= 0 && page < kPages ? kRowsOnPage[page] : 0; }
bool IsAllSettingsRow(int page, int row) { return ValidRow(page, row) && kRows[page][row].kind == kRowAccount; }
bool IsResetRow(int page, int row) { return ValidRow(page, row) && kRows[page][row].kind == kRowReset; }
int Option(int page, int row) { return OptionOf(page, row); }
int NextPage(int page) { return ((page + 1) % kPages + kPages) % kPages; }

int Name(int page, int row, char* out, int cap) {
    if (!ValidRow(page, row)) return Copy("", out, cap);
    return Copy(Pick(g_language, kRows[page][row].name), out, cap);
}

int Value(int page, int row, char* out, int cap) {
    const int o = OptionOf(page, row);
    if (!g_open || o < 0) return Copy("", out, cap);
    const int c = g_state.choice[o];
    char text[kMaxLabelBytes * 2];
    const char* english = c < 0 ? nullptr : NpSettings::GetOption(o).choices[c].label;
    const char* shown = c < 0 ? Pick(g_language, kCustom) : NpText::In(g_language, english);
    for (const auto& v : kShortValues)
        if (english && std::strcmp(english, v.english) == 0) shown = Pick(g_language, v.text);
    Copy(shown, text, sizeof text);
    char* paren = std::strstr(text, " (");
    if (paren) *paren = 0;
    return Copy(text, out, cap);
}

int Label(int page, int row, char* out, int cap) {
    if (!ValidRow(page, row)) return Copy("", out, cap);
    char name[kMaxLabelBytes], value[kMaxLabelBytes];
    Name(page, row, name, sizeof name);
    if (IsAllSettingsRow(page, row) || IsResetRow(page, row)) return Copy(name, out, cap);
    Value(page, row, value, sizeof value);
    char both[kMaxLabelBytes * 2 + 4];
    std::snprintf(both, sizeof both, "%s: %s", name, value);
    return Copy(both, out, cap);
}

int MoreLabel(int page, char* out, int cap) {
    char text[kMaxLabelBytes];
    std::snprintf(text, sizeof text, "%s (%d/%d)", Pick(g_language, kMore), page + 1, kPages);
    return Copy(text, out, cap);
}

int DoneLabel(char* out, int cap) { return Copy(Pick(g_language, kDone), out, cap); }

bool Dim(int page, int row) { return Why(page, row) != nullptr; }

const char* Why(int page, int row) {
    const int o = OptionOf(page, row);
    if (!g_open || o < 0) return nullptr;
    const char* why = NpSettings::WhyNoEffect(g_state, o);
    return why ? NpText::In(g_language, why) : nullptr;
}

PressResult Press(int page, int row, int dir) {
    if (!g_open || !ValidRow(page, row)) return kNothing;
    if (IsAllSettingsRow(page, row)) return kAllSettings;
    if (IsResetRow(page, row)) {   // every row back to its default; the leave popup saves it (Changed() is true from here until a save or a discard)
        NpSettings::ResetAll(g_state);
        return kChanged;
    }
    const int o = OptionOf(page, row);
    if (o < 0) return kNothing;
    if (NpSettings::WhyNoEffect(g_state, o)) return kNoEffect;
    NpSettings::Step(g_state, o, dir < 0 ? -1 : 1);
    return kChanged;
}

int Help(int page, int row, char* out, int cap) {
    if (!ValidRow(page, row)) return Copy("", out, cap);
    if (IsAllSettingsRow(page, row)) return Copy(Pick(g_language, kAccountHelp), out, cap);
    if (IsResetRow(page, row)) return Copy(Pick(g_language, kResetHelp), out, cap);
    const int o = OptionOf(page, row);
    if (o < 0) return Copy("", out, cap);
    return Copy(NpText::In(g_language, NpSettings::GetOption(o).help[0]), out, cap);
}

int HelpLine(int page, int bar, bool shown, char* out, int cap) {
    if (!g_open || !shown || bar < 0 || bar >= Rows(page)) return Copy("", out, cap);
    return Help(page, bar, out, cap);
}

bool Changed() { return g_open && (NpSettings::Changed(g_state) || g_state.reset); }   // (set18: RESET TO DEFAULTS is a change even when every row already stood at its default: the save drops the retired lines)
bool RestartNeeded() { return g_open && NpSettings::Changed(g_state) && NpSettings::RestartNeeded(g_state); }

int LeaveMessage(char* out, int cap) { return Copy(Pick(g_language, kLeaveMessage), out, cap); }

int LeaveButtonLabel(int button, char* out, int cap) {
    const char* english = button == kLeaveSaveNext ? "SAVE FOR NEXT START" : button == kLeaveSaveRestart ? "SAVE AND RESTART" : button == kLeaveDiscard ? "DISCARD" : "";
    return Copy(NpText::In(g_language, english), out, cap);
}

int SaveFailedMessage(char* out, int cap) { return Copy(Pick(g_language, kSaveFailed), out, cap); }

bool Save() {
    if (!g_open || !NpNativeSettingsBackendWrite) return false;
    std::string out;
    if (!NpSettings::Serialize(g_state, out)) return false;
    out = NpSettingsGuard::Stamped(out, NpBuildIdentity::Version(), NpBuildIdentity::BuildId());   // as the technical screen's save: the file says which build wrote it
    if (!NpNativeSettingsBackendWrite(out.c_str(), static_cast<int>(out.size()))) return false;
    if (NpLiveSettingsSet) NpSettings::ApplyLive(g_state, [](const char* key, const char* value) { NpLiveSettingsSet(key, value); });   // the running game takes the live rows now (before Load: it compares with the old file)
    NpSettings::Load(g_state, out.c_str());   // what is on disk is now what was loaded: Changed() is false
    return true;
}

bool NextStartChanged() { return g_open && NpSettings::Changed(g_state) && NpSettings::NextStartChanged(g_state); }

int LeaveCount(bool) { return 3; }

LeaveActionId LeaveAction(int index, bool matchRunning) {
    if (index == 1) return kActDiscard;
    if (index == 2) return kActKeep;
    return !matchRunning && NextStartChanged() ? kActSaveRestart : kActSave;
}

int LeaveLabel(int index, bool matchRunning, char* out, int cap) {
    if (index < 0 || index >= 3) return Copy("", out, cap);
    const LeaveActionId a = LeaveAction(index, matchRunning);
    const char* english = a == kActSave ? "SAVE" : a == kActSaveRestart ? "SAVE AND START AGAIN" : a == kActDiscard ? "DON'T SAVE" : "KEEP EDITING";
    return Copy(NpText::In(g_language, english), out, cap);
}

bool PauseRestartOffered(bool matchRunning) {
    const char* e = std::getenv("MSC_PAUSE_RESTART");
    return matchRunning && e && e[0] == '1' && NextStartChanged();
}

int RestartAskMessage(char* out, int cap) { return Copy(Pick(g_language, kRestartAsk), out, cap); }

int RestartAskLabel(int button, char* out, int cap) { return Copy(Pick(g_language, button == 1 ? kStartAgain : kNotNow), out, cap); }

int LeaveNote(bool matchRunning, char* out, int cap) {
    if (!NextStartChanged()) return Copy("", out, cap);
    return Copy(Pick(g_language, matchRunning ? kLeaveNoteMatch : kLeaveNoteAgain), out, cap);
}

LeaveOutcome Apply(LeaveActionId action) {
    switch (action) {
    case kActKeep: return kStay;
    case kActDiscard: Discard(); return kLeft;
    case kActSave: return Save() ? kLeft : kOutSaveFailed;
    case kActSaveRestart:
        Restart();                                   // saves first; returns only when the system refused (or on the host)
        return Changed() ? kOutSaveFailed : kLeft;      // still changed = the save itself failed
    }
    return kStay;
}

bool Restart() {
    if (!Save()) return false;
    return NpNativeSettingsBackendRestart && NpNativeSettingsBackendRestart();
}

void Discard() {
    if (!g_open) return;
    if (!LoadFromBackend()) g_open = false;
}

bool OpenAll() { return NpNativeSettingsBackendOpenAll && NpNativeSettingsBackendOpenAll(); }

}  // namespace NpNativeSettings
