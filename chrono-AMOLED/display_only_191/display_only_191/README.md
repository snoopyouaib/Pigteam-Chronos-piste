# display_only_191 -- 7 écrans navigables (données simulées)

Même principe que le `display_only` du projet TFT : écran + encodeur
EC11 + bouton BACK, **aucune** dépendance GPS/SD/batterie/WiFi réelle.
Toutes les données affichées viennent d'une couche de simulation en
RAM (cf. section "Simulation" de `src/main.cpp`), pour régler
l'affichage sur le banc.

## État actuel

Les **7 écrans** sont construits et fonctionnels : Statut, Circuit,
Connexion, Session (liste), Session (tours), Réglages, WiFi.
"New track (capture)" est replié dans le menu Circuit plutôt que
d'être une entrée de menu séparée (demande explicite, cf. discussion).

## Navigation (encodeur conservé, pas de menu liste séparé)

- **Statut** : PUSH avance la démo (recherche → détecté →
  enregistrement → détecté...), BACK va sur **Circuit**.
- **Anneau** Circuit → Connexion → Session → Réglages → Circuit... :
  la rotation de l'encodeur change d'écran (pas de liste de menu
  intermédiaire -- l'écran affiché EST le menu).
- **Circuit / Session / Réglages** : PUSH entre en mode sélection
  (l'encodeur change de rôle, parcourt la liste), re-PUSH valide.
  BACK en mode sélection annule et revient au mode anneau ; BACK en
  mode anneau retourne au Statut.
- **Session → Tours** : PUSH valide ouvre les tours de la session
  sélectionnée (défilement par rotation), BACK y revient à la liste
  des sessions (en mode sélection).
- **Réglages → WiFi** : PUSH valide ouvre WiFi, BACK en coupe direct
  au Statut (comportement repris du TFT).

Décision de navigation finale (tactile only) discutée mais **pas
retenue pour cette itération** -- l'encodeur + BACK a été conservé, cf.
"Prochaines étapes" du README principal pour l'historique de cette
discussion.

## Laissé de côté pour l'instant

- Message "CAPTURE NEW TRACK (simu)" temporaire (jaune, ~4s) après
  sélection de "Nouveau circuit" sur l'écran Circuit -- le flag
  `simCaptureArmed` est positionné mais rien ne l'affiche encore.
- Retour visuel si on tente d'entrer en sélection sur Session sans
  aucune session enregistrée (PUSH silencieusement ignoré).

## Polices

Réutilise `lib/fonts_teko/` du projet `firmware_1.91` (4 tailles Teko
calibrées à l'œil sur ce même board -- Bold 84/38, Medium 34/26).

## Mise en page de l'écran Statut (calibrée à l'œil sur le vrai board)

| Élément | Police | Taille | Position |
|---|---|---|---|
| GPS + circuit (fusionnés) | Medium | 26px | haut gauche |
| REC / % batterie (couleur seuil) | Medium | 26px | haut droite |
| Vitesse ou chrono (gros, selon mode) | Bold | 84px | centré (recherche/détecté) ou centré (REC) |
| Heure (recherche/détecté) | Bold | 38px | centré |
| PRESS REC clignotant (détecté) | Bold | 38px | centré, jaune |
| Dernier / Best (REC) | Medium | 34px | gauche |
| Tours: N (REC) | Medium | 34px | bas droite |

## Couleurs dynamiques

- **État** : gris (recherche) -> vert (PRESS REC) -> orange (REC,
  attente franchissement ligne) -> rouge (REC actif)
- **Batterie** : vert >50%, orange 20-50%, rouge ≤20% (triangle
  100%->0%->100% simulé sur ~40s pour voir les 3 couleurs se succéder)

## Prochaines étapes

- Message "CAPTURE NEW TRACK (simu)" temporaire + retour visuel liste
  de sessions vide en mode sélection (cf. "Laissé de côté" ci-dessus).
- Coloration fine du texte GPS séparément du circuit sur la ligne du
  haut de Statut (actuellement une seule couleur pour toute la ligne --
  un `lv_span` permettrait de nuancer).
- Rebrancher les vraies données (GPS, SD, batterie, WiFi) une fois la
  navigation validée -- ce projet reste 100% simulé par conception.
- Reconsidérer une navigation tout-tactile en complément/remplacement
  de l'encodeur pour les écrans hors pilotage (cf. discussion dans le
  README principal) -- pas retenu pour cette itération, l'encodeur a
  été conservé.
