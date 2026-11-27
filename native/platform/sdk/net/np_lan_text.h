// np_lan_text.h: the disc's online message texts that name Nintendo's service, the Wii or an SD card, replaced by their message id (1.3).
//
// The disc keeps every language's texts in a file of its own (Art/fe/<language>.loc) and every message has the same id (a hash of its name) in all of
// them. The port used to find the English texts by their words and replace them in place; a Spanish or French player then read the disc's own
// "Nintendo Wi-Fi Connection" texts. This table holds the ids (no disc text is copied here: the new texts come from the port's table, np_text_strings.tsv,
// keyed by their English) and the length in characters of the disc's own message in English, North American Spanish and North American French
// (the replacement has to fit the old text: the file's layout does not change; a text that does not fit is left as the disc has it).
// The second block (words3) covers the messages about the Wii's memory, an SD card, a Mii, the Wii Remote and the Nintendo service. Messages about the Mii
// screens themselves are not here: the port skips those screens (np_online_flow.cpp). Lengths are the North American files' as before.
// Header only, no game headers (tests/test_lan_text.cpp). It changes what is drawn, nothing a match or the online protocol reads.
#ifndef NP_LAN_TEXT_H
#define NP_LAN_TEXT_H

#include <stddef.h>

#include "platform/sdk/include/np/np_text.h"

namespace NpLanText {

struct IdPatch {
    unsigned long id;    // the message id (the lookup table's hash)
    const char* key;     // the English text of the replacement: its Spanish and French come from np_text_strings.tsv
    unsigned short discLength[3];   // the disc's text in English, Spanish, French (characters; tests check that the replacements fit)
    bool online;         // part of the online screens' wording (MSC_ONLINE_TEXT=0 leaves these as the disc has them)
    bool translateOnly;  // English is left as the disc has it (np_prompts.cpp rewrites it for the player's layout): only Spanish and French are replaced
};

static const IdPatch kPatches[] = {
    {0x1455335c, "MULTIPLAYER", {25, 26, 24}, false, false},
    {0x4256773a, "PLAY A FRIEND ONLINE, NEARBY OR ON THE SAME Wi-Fi!", {80, 91, 82}, false, false},
    {0x85a2ed2d, "ONLINE PLAY DATA WILL NOW BE CREATED ON THIS PS VITA.", {123, 159, 143}, true, false},
    {0xcab013a6, "ERROR {0}{p}UNABLE TO CONNECT TO THE INTERNET. CHECK THE Wi-Fi CONNECTION OF THIS PS VITA.", {96, 120, 109}, true, false},
    {0x5a4a0fac, "ERROR {0}{p}THE ONLINE SERVICE IS BUSY OR DOWN.{p}PLEASE TRY AGAIN LATER.", {170, 225, 216}, true, false},
    {0x72028ed0, "ERROR {0}{p}UNABLE TO CONNECT TO THE ONLINE SERVICE.{p}PLEASE TRY AGAIN LATER.", {142, 170, 169}, true, false},
    {0xec4c1d0d, "ERROR {0}{p}THIS VERSION CAN NO LONGER USE THE ONLINE SERVICE.{p}PLEASE UPDATE THE GAME.", {184, 234, 214}, true, false},
    {0x62e4d59c, "ERROR CONNECTING TO THE ONLINE SERVICE.", {46, 39, 70}, true, false},
    {0xb1a1af1a, "ERROR {0}{p}COMMUNICATION ERROR. PLEASE TRY AGAIN.", {90, 105, 126}, true, false},
    {0x3e7f99c3, "ONLINE CENTRAL", {20, 23, 20}, true, false},
    {0x193d1103, "CONNECTING TO ONLINE PLAY", {26, 31, 29}, true, false},
    {0xf208fe14, "CONNECTED TO ONLINE PLAY", {37, 40, 17}, true, false},
    {0x5d68a9f5, "ONLINE DOMINATION", {23, 30, 14}, true, false},
    {0x63c0d209, "ONLINE INVITATION", {23, 21, 23}, true, false},
    // (1.3, words3) the other messages that named the Wii's memory, an SD card, a Mii, the Wii Remote or the Nintendo service:
    {0x05808a3c, "AN ERROR OCCURRED WHILE ACCESSING THE SAVE DATA. NO FURTHER SAVING OR LOADING IS POSSIBLE.", {98, 98, 124}, false, false},
    {0x4ba23f6c, "ERROR {0}{p}THERE IS NOT ENOUGH FREE SPACE ON THIS PS VITA.", {69, 76, 87}, false, false},
    {0x4be8db2c, "MARIO STRIKERS CHARGED WILL AUTOMATICALLY SAVE YOUR DATA ON THIS PS VITA.", {78, 90, 107}, false, false},
    {0x97a7fd0a, "ERROR {0}{p}THE SAVE DATA IS DAMAGED AND CANNOT BE USED.", {112, 137, 128}, false, false},
    {0xc6d9d6a1, "THE SAVE DATA IS DAMAGED AND CANNOT BE USED.", {96, 125, 109}, false, false},
    {0x9d2bbe6c, "THERE IS NOT ENOUGH FREE SPACE ON THIS PS VITA. FREE UP SPACE AND TRY AGAIN.", {174, 186, 186}, false, false},
    {0xff660c68, "THERE IS NOT ENOUGH FREE SPACE ON THIS PS VITA. FREE UP SPACE AND TRY AGAIN.", {174, 197, 186}, false, false},
    {0xd4b7841e, "THERE IS NOT ENOUGH FREE SPACE ON THIS PS VITA. FREE UP SPACE AND TRY AGAIN.", {134, 163, 178}, false, false},
    {0x338075fe, "TO PLAY ONLINE, A SAVE FILE MUST FIRST BE CREATED ON THIS PS VITA.", {98, 106, 124}, true, false},
    {0x1417b6cd, "FAILURE TO ACCESS THE GAME FILES. THE ONLINE CONNECTION WAS CLOSED.", {72, 76, 76}, true, false},
    {0x4e9fbc22, "ERROR {0} CONNECTING TO THE ONLINE SERVICE.", {50, 43, 89}, true, false},
    {0x53161f8f, "ERROR {0}{p}THERE IS NO RESPONSE. EITHER THE PLAYER IS NOT ONLINE, OR YOU HAVE BEEN REMOVED FROM THEIR FRIEND LIST.", {156, 160, 134}, true, false},
    {0xe1bb7f6c, "{0} HAS INVITED YOU TO AN ONLINE MATCH. WOULD YOU LIKE TO:", {68, 56, 56}, true, false},
    {0xca0352fa, "THAT WAS A GREAT WIN! YOU ARE NOW READY TO TAKE ON THE WORLD OF MARIO STRIKERS CHARGED! PUT YOUR SKILLS TO THE TEST IN EVERY MODE, ONLINE TOO!", {197, 239, 239}, true, false},
    {0x32aa36ca, "ONLINE CUP", {14, 38, 14}, true, false},
    {0xbcf4ee94, "ONLINE DOMINATION", {19, 34, 19}, true, false},
    {0x14308e7c, "YOU CANNOT GO ONLINE WITH THIS NAME. TRY AGAIN WITH A DIFFERENT NAME.", {145, 132, 152}, true, false},
    {0xe6bfe2d3, "QUIT THE GAME", {22, 21, 21}, false, false},
    {0xa4c48b96, "ACTIVATE CONTROLLERS", {20, 21, 26}, false, true},
    {0xe37f5215, "ACTIVE CONTROLLERS", {18, 23, 26}, false, true},
    {0xc473b18b, "PRESS THE HIT BUTTON!", {24, 37, 21}, false, true},
    {0x879a1cb3, "PLEASE CHECK THE CONTROLLER.", {61, 59, 64}, false, true},
    {0xb39ac9a8, "PLEASE CHECK THAT EVERY PLAYER HAS A CONTROLLER.", {73, 80, 71}, false, true},
    {0x55c0f1a1, "SWITCH POWER-UPS WITH ITS BUTTON.", {34, 41, 48}, false, true},
    {0x1a42cf29, "POWER-UPS ARE EARNED BY WINDING UP AND RELEASING A SHOT WHEN A BLUE ENERGY RING SURROUNDS YOUR PLAYER, OR WHEN A PLAYER WITHOUT THE BALL IS TACKLED. THE POWER-UP APPEARS IN THE HUD. LAUNCH IT WITH THE POWER-UP BUTTON. EARN SOME AND WIN THE GAME!", {375, 305, 416}, false, true},
    {0xa6a75c80, "WHEN A MEGA STRIKE IS LAUNCHED ON YOUR TEAM, YOU CAN STOP THE BALLS AND WIN THE GAME! TAKE CONTROL OF THE GOALIE'S HANDS. WHEN THE HANDS ARE OVER A BALL, PRESS THE ACTION BUTTON TO DESTROY IT! BALLS CAN COME FROM ANYWHERE, SO BE READY!", {299, 282, 323}, false, true},
    {0xe6d29a31, "TRY A DIFFERENT KIND OF SKILLSHOT WITH BOO! FIND THE RIGHT DISTANCE TO SCORE! IF YOU LOSE THE BALL, WIN IT BACK ON DEFENSE: SLIDE TACKLE, OR THROW A BIG HIT WITH THE HIT BUTTON.", {298, 357, 311}, false, true},
    {0x03cf0e52, "TO PUT YOUR CONTROLLER ON A TEAM, PRESS THE ACTION BUTTON ON THE TEAM BOX YOU WANT. TO CHANGE TEAMS, PICK IT UP AGAIN WITH THE SAME BUTTON.", {294, 169, 238}, false, true},
};
static const size_t kPatchCount = sizeof kPatches / sizeof kPatches[0];

// (mpbeta27 / int34) The one-time beta popup is enough: main menu MULTIPLAYER and hub ONLINE CENTRAL stay clean without a BETA suffix.
inline const char* BetaKeyFor(unsigned long) { return nullptr; }

// The port's text language (NpText::kEnglish, kSpanish, kFrench) of a disc language file (nlLocalization::nlLanguage: 1 French, 3 Spanish,
// 7 North American French, 8 North American Spanish); every other file gets the English replacement.
inline int TextLanguage(int discLanguage) {
    switch (discLanguage) {
    case 1:
    case 7: return NpText::kFrench;
    case 3:
    case 8: return NpText::kSpanish;
    default: return NpText::kEnglish;
    }
}

// Replaces, in place, the messages of kPatches found in a language file by their ids. `Lookup` has `hash` and `StringOffset` (nlLocalization::StringLookup);
// `strings` points at the file's UTF-16 text in the machine's own byte order, `offsets` are in characters. A text that is not shorter than or equal to the
// old one (the old length is measured to its end mark) is left alone. `onlineText` false keeps the `online` messages as the disc has them.
// Returns how many messages were replaced.
template <class Lookup>
int PatchById(const Lookup* lookup, unsigned long count, unsigned short* strings, int discLanguage, bool onlineText, bool beta = false) {
    if (!lookup || !strings) return 0;
    const int language = TextLanguage(discLanguage);
    int replaced = 0;
    for (unsigned long i = 0; i < count; ++i) {
        for (size_t p = 0; p < kPatchCount; ++p) {
            const IdPatch& patch = kPatches[p];
            if ((unsigned long)lookup[i].hash != patch.id || (patch.online && !onlineText)) continue;
            if (patch.translateOnly && language == NpText::kEnglish) break;
            unsigned short* s = strings + lookup[i].StringOffset;
            size_t old = 0;
            while (s[old] && old < 1024) ++old;
            if (s[old] || old == 0) break;   // no end mark within 1024 characters, or an empty message: not the text this table was made for
            unsigned short wide[512];
            const char* betaKey = beta ? BetaKeyFor(patch.id) : nullptr;
            const char* text = NpText::In(language, betaKey ? betaKey : patch.key);
            const int len = NpText::ToUtf16(text, wide, 511);
            if (len <= 0 || size_t(len) > old) break;
            for (size_t k = 0; k < old; ++k) s[k] = k < size_t(len) ? wide[k] : 0;
            ++replaced;
            break;
        }
    }
    return replaced;
}

}  // namespace NpLanText

#endif  // NP_LAN_TEXT_H
