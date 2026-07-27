# Alimentation 12V → 5V -- convertisseur, protections, interrupteur

Ce document récapitule le montage électrique retenu pour alimenter le
chrono OLED directement depuis la batterie moto (CBR125), en amont du
contact -- pensé pour ne rien oublier au moment du câblage définitif.

## Pourquoi en direct-batterie, avant le contact

Objectif : que le chrono continue de fonctionner même si le contact est
coupé ou si le moteur cale, plutôt que de subir une coupure brutale à
chaque arrêt (cf. test volontaire de coupure d'alimentation en cours de
session, concluant -- logs GPS restés exploitables sans corruption même
dans ce cas). Contrepartie assumée : le chrono consomme en permanence
tant que l'interrupteur dédié n'est pas coupé (cf. plus bas).

## Schéma de câblage

```
Batterie 12V moto (avant contact, borne +)
        |
        |  Fusible 2A -- AU PLUS PRÈS DE LA BATTERIE
        |  (protège le câble, pas le convertisseur)
        v
   [ FUSIBLE 2A ]
        |
        v
 [ INTERRUPTEUR ON/OFF ]  -- accessible/visible en paddock,
        |                    pas planqué sous un carénage
        |
        +──────────────────────────┐
        |                          |
        v                          v
  Entrée convertisseur        [ TVS 1.5KE18A ]
  (fil ROUGE = +)             en parallèle, au plus
        |                     près de l'entrée du
        |                     convertisseur --
        |                     cathode côté + (impératif,
        |                     bague repère → vers le +)
        |                          |
        +──────────────────────────┘
        |
        v
 [ CONVERTISSEUR DC-DC 12V/24V → 5V ]
   (buck, protections intégrées :
    surtension/surintensité/
    court-circuit/surchauffe)
        |
        |  Sortie 5V -- fil JAUNE = +, NOIR = -
        |  câble LE PLUS COURT POSSIBLE
        v
   Chrono PIGTEAM (ESP32-S3-Tiny N8R8)

Retour batterie (masse, borne -)
        |
        +── fil NOIR d'entrée du convertisseur
        +── fil NOIR de sortie du convertisseur (commun avec l'entrée)
```

## Composants retenus

| Composant | Référence / valeur | Rôle |
|---|---|---|
| Fusible | **2A** (mini/micro lame) | Protège le câble batterie→convertisseur, pas le convertisseur lui-même (déjà protégé en interne) |
| Interrupteur ON/OFF | calibre courant faible (le convertisseur ne tire pas des masses) | Coupe l'alimentation entre deux sorties -- indispensable vu la consommation à vide du convertisseur (cf. plus bas) |
| TVS (diode de protection) | **1.5KE18A** (18V, 1500W, through-hole DO-15) | Absorbe les transitoires de tension (load-dump, coupure d'un consommateur sur le réseau 12V) -- ne fait rien en fonctionnement normal, agit seulement sur un pic. **⚠️ Sens impératif : cathode côté +, cf. avertissement dédié ci-dessous** |
| Convertisseur DC-DC | 12/24V → 5V, **5A** (disponible) ou **3A** (en commande) | Alimentation régulée du chrono -- protections intégrées (surtension/surintensité/court-circuit/surchauffe) déjà présentes sur le module, la TVS reste néanmoins recommandée pour les transitoires rapides que ces protections ne couvrent pas toujours à temps |

## ⚠️ Sens de branchement de la TVS -- point critique

**Cathode côté +, l'autre patte côté - (masse).** Repérable sur le
boîtier par une bague/trait de couleur à une extrémité -- cette
extrémité va côté +.

**Pourquoi ce sens précis** : la TVS doit rester **bloquante** en
fonctionnement normal (12-14.4V), montée en polarisation inverse comme
un Zener de protection -- invisible électriquement tant que la tension
reste sous son seuil (18V), elle ne conduit que si un pic le dépasse.

**Si le sens est inversé** : elle se retrouve polarisée en direct par
rapport au 12V normal -- elle conduit en permanence dès ~0.7V, comme une
diode classique. Ça court-circuite quasiment la ligne d'alimentation en
continu dès la mise sous tension (pas un mode dégradé, un vrai problème
immédiat) : le fusible saute, ou la diode grille avant si elle lâche en
premier.

**À vérifier avant toute mise sous tension du montage.**

## Points de placement à ne pas rater

- **Fusible côté batterie**, pas côté convertisseur -- il protège tout le
  tronçon de câble, pas juste l'appareil en bout de ligne.
- **TVS au plus près de l'entrée du convertisseur** -- elle protège ce
  qui est juste après elle.
- **Convertisseur au plus près du chrono**, pas de la batterie -- une
  chute de tension pèse bien plus lourd en pourcentage sur du 5V que sur
  du 12V, et une ligne 5V longue traversant tout le câblage moto capte
  davantage de bruit électrique (sensible pour le GPS/ESP32). Le 12V
  brut, lui, encaisse une plus longue distance sans souci.
- **Câble de sortie 5V le plus court possible** -- idéalement quelques
  cm entre le convertisseur et le chrono.

## Checklist avant de refermer le montage

- [ ] Fusible 2A posé au plus près de la batterie
- [ ] Interrupteur accessible et bien visible en paddock (réflexe facile à prendre)
- [ ] TVS 1.5KE18A branchée en parallèle sur l'entrée du convertisseur -- **cathode côté +** (bague repère sur le boîtier), **vérifiée avant mise sous tension** (un sens inversé court-circuite la ligne dès l'allumage)
- [ ] Sens de branchement vérifié : entrée rouge (+) / noir (-), sortie jaune (+) / noir (-)
- [ ] Convertisseur positionné près du chrono, pas près de la batterie
- [ ] Câble de sortie 5V aussi court que possible
- [ ] Longueur du câble 12V (batterie → convertisseur) sans contrainte particulière -- moins sensible qu'en 5V

## À garder en tête à l'usage

- **Consommation à vide non nulle** : le convertisseur tire du courant
  en permanence dès qu'il est sous tension, même sans rien en sortie
  (10 mA pour la version 3A, 15 mA pour la version 5A). Sur la batterie
  d'une CBR125, un oubli de l'interrupteur sur ON peut décharger
  sensiblement la batterie en quelques semaines -- **penser à couper
  après chaque sortie**.
- Le fusible protège le câblage, pas le convertisseur (déjà protégé en
  interne) -- inutile de sur-dimensionner "pour être large", 2A suffit
  très confortablement pour la consommation réelle du chrono (quelques
  centaines de mA à 5V).
