#ifndef FONTS_TEKO_H
#define FONTS_TEKO_H

#include "lvgl.h"

// ===================== Polices Teko converties pour LVGL =====================
//
// Reconverties depuis la police Google Fonts "Teko" (SIL Open Font
// License) via l'outil officiel lv_font_conv, PAS reutilisees depuis
// les .h Adafruit_GFX (GFXfont) du projet TFT -- format de donnees
// totalement different, incompatible avec lv_font_t.
//
// Poids/tailles -- chrono encore agrandi (72->84px), et espacement
// resserre : les chiffres ("1:23.456") n'ont pas de descendante, donc
// une bonne partie du line_height de la police (87px pour ce corps 84)
// reste vide en bas de la ligne -- normal (reserve pour g/y/p/q), pas
// un espacement ajoute par nous. Comme le placement est manuel (pas de
// flow automatique LVGL), on peut remonter la ligne suivante dans cet
// espace sans souci.
//   - Bold 84px  : chrono tour en cours / vitesse (le plus gros)
//   - Bold 38px  : "PRESS REC"
//   - Medium 34px: heure
//   - Medium 26px: labels/valeurs + texte le plus fin (circuit,
//     dernier/meilleur, REC, GPS, batterie, tours)
//
// Attention : la hauteur d'ecran reste 240px (identique au TFT) --
// grossir les polices ne donne pas plus de place verticale, seulement
// plus de largeur. A verifier une fois tous les elements du vrai ecran
// statut empiles ensemble (pas seulement en test isole comme ici).
//
// Plage couverte : ASCII imprimable 0x20-0x7E + Latin-1 Supplement
// 0xA0-0xFF (couvre tous les accents francais : e ` ^ " a c u etc.) --
// etendue suite au constat que les caracteres accentues ne
// s'affichaient pas (glyphe manquant, plage ASCII pure au depart).
// A regenerer avec --range different si besoin d'autres caracteres.
//
// IMPORTANT : l'ecran 1.91 est 536x240 (paysage large), diffKrent du
// TFT 320x240 -- ces tailles sont un point de depart, pas une verite
// figee. A retoucher a l'oeil une fois les ecrans construits, comme ca
// avait ete fait pour le TFT (cf. "Prochain chantier -- affichage
// lisible en roulant" du README bring-up).
//
// Regeneration (si besoin d'une autre taille/graisse/plage) :
//   npm install -g lv_font_conv
//   lv_font_conv --font Teko-Bold.ttf --no-compress --bpp 4 --size XX \
//     --format lvgl -o lv_font_teko_bold_XX.c --lv-include lvgl.h \
//     --range 0x20-0x7E,0xA0-0xFF
// (Teko-Bold/Medium/Regular.ttf generes depuis Teko[wght].ttf de
// github.com/google/fonts via `fonttools varLib.instancer`, poids
// 700/500/400.)
//
// IMPORTANT : --no-compress est obligatoire ici -- LV_USE_FONT_COMPRESSED
// est desactive dans lv_conf.h (comme dans la config Waveshare
// d'origine). Sans ce flag, lv_font_conv genere des glyphes compresses
// (bitmap_format=1) que LVGL ne peut pas decoder -> texte invisible,
// ecran noir, sans aucune erreur de compilation (piege rencontre au
// premier essai).

LV_FONT_DECLARE(lv_font_teko_bold_84);
LV_FONT_DECLARE(lv_font_teko_bold_56);
LV_FONT_DECLARE(lv_font_teko_bold_38);
LV_FONT_DECLARE(lv_font_teko_medium_34);
LV_FONT_DECLARE(lv_font_teko_medium_26);

#endif
