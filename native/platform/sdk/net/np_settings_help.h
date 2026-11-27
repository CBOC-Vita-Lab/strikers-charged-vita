// set18 (owner, 23 Nov: "the description is a good idea if you can make it look native. Like it was in the game in the first place."): the one-line help of the VITA SETTINGS
// row under the cursor, drawn on the steel strip at the foot of the pause panel, in the panel's own bar lettering (the font and colour of the bars' "option" text,
// taken from the package at run time: no new art, no new font). The pause panel package (pausemenu_v3.fen) has no description text of its own (only the seven bars
// and the title band), so the text is drawn the way the port's online banner is (np_notice.cpp): through nlTextBox, the path TLTextInstance::Render takes,
// once per frame after the last front-end scene.
//
// SHVitaSettings sets the line (Show) whenever the cursor's bar, the page or a value changes and clears it (Show(nullptr)) while the panel slides, while a popup is up and
// when the scene goes. The text is UTF-8 in the game's language (NpNativeSettings::HelpLine already translated it).
#pragma once

class nlFont;
struct nlColour;
class GLView;
class nlMatrix4;

// scene: the FEScene of the panel (the line is drawn only after that scene); utf8 null or "": no line.
void NpSettingsHelpShow(const char* utf8, const nlFont* font, const nlColour* colour, const void* scene);
// FERender::RenderScene, after each scene: draws the line when `scene` is the panel's.
void NpSettingsHelpRender(const void* scene, GLView* view, const nlMatrix4& matView);
