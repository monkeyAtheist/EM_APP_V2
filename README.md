# QTsignalApp 7 — refonte

Outil pédagogique et utilitaire de développement : électrostatique, magnétostatique, ondes (FDTD 2D) et antennes.
Ce dépôt est la réécriture de QTsignalApp 6.14, menée selon l'audit du 15/11/2026.

**Statut :**
- **Phase 0 (socle) : terminée.**
- **Phase 1 (pilote Électrostatique en Qt Quick) : terminée.**
- **Phase 2 (magnétostatique, FDTD) : terminée.** Modules Magnétostatique (noyaux exacts) et Ondes (FDTD 2D
  en temps réel).

Les modules Électrostatique, Magnétostatique et Ondes sont utilisables. Antennes et outils RF restent dans
l'ancienne application d'ici leur phase.

## Structure

```
physics/                    qsig_physics : cœur de calcul C++20, sans aucune dépendance Qt
  include/qsig/constants.h  constantes physiques CODATA 2022 (seule définition du projet)
  include/qsig/vec3.h       le seul type vecteur 3D
  include/qsig/linalg.h     le seul solveur linéaire dense (LU à pivot partiel)
  include/qsig/electrostatics.h, magnetostatics.h   noyaux de champ (formes exactes, voir plus bas)
  include/qsig/sheet_integrals.h   intégrales exactes des plaques, disques, anneaux et pavés chargés
  include/qsig/special_functions.h intégrale elliptique complète générale de Bulirsch
  include/qsig/...          solveurs repris de la 6.14 : FDTD 2D, MoM fil mince et RWG, BEM 2D,
                            couplage de boucles, banc de validation, calculatrices, modèles RF
app/                        qsig_app : logique d'application (QtCore/QtGui, sans interface)
  scene_base                base commune des modules : liste de sources, annuler/rétablir, fichiers JSON
  electrostatic_scene, magnetostatic_scene   les modules statiques (sources, formules, dessin, champ)
  wave_simulation           scène d'ondes posée sur le solveur FDTD 2D (maillage, matériaux, images, mesures)
  wave_scene                module Ondes : objets, réglages, simulation sur un fil de calcul
  field_lines               lignes de champ régulièrement espacées
  field_map                 carte de champ calculée en arrière-plan (QtConcurrent)
  units                     saisie et affichage des grandeurs avec unités (« 2 nC », « 10 cm », virgule)
ui/                         QTsignalApp : interface Qt Quick (QML) et vue de champ (QQuickPaintedItem)
tests/                      qsig_tests (physique) et qsig_app_tests (couche application)
  fixtures/scenarios.h      scénarios rejoués à l'identique sur l'ancien et le nouveau code
  fixtures/legacy_baseline.inc   valeurs enregistrées depuis le code 6.14
scripts/                    build.cmd et run.cmd : compiler, tester et lancer sous Windows
tools/baseline/             enregistreur de la référence 6.14 (compile l'ancien code avec un faux Qt minimal)
tools/reference/            scripts Python indépendants de l'audit (Hallén, plaque, solénoïde)
cmake/                      options de compilation communes
```

## Compiler et tester

Sous Windows, avec Qt 6.11 et son compilateur MinGW :

```
scripts\build.cmd        configure, compile et lance tous les tests (journaux dans build\logs\)
scripts\run.cmd          lance l'application depuis le dossier de compilation
```

Ou directement, avec les préréglages de `CMakeUserPresets.json` (propres à ce PC) :

```
cmake --preset mingw-release
cmake --build --preset mingw-release
ctest --preset mingw-all
```

Sous Linux ou en intégration continue :

```
cmake --preset ci
cmake --build --preset ci
ctest --preset ci          # tout, y compris la parité avec la 6.14 (~15 s)
ctest --preset fast        # tests rapides seulement
```

Sans Qt (intégration continue, travail sur la physique seule), seuls le cœur et ses tests sont construits ;
`-DQSIG_BUILD_APP=OFF` force ce mode.

`cmake --preset sanitize` construit avec AddressSanitizer et UBSan (gcc ou clang).

## Règles du cœur physique

1. Aucun en-tête Qt dans `physics/`. La configuration CMake échoue si Qt y est lié.
2. Les constantes viennent uniquement de `qsig/constants.h`. Les anciens noms (`NumericalEM::Mu0`, `FDTD::epsilon0`…)
   ne sont plus que des alias.
3. Un seul type vecteur (`qsig::Vec3`) et un seul solveur dense (`qsig::linalg`).
4. Zéro avertissement avec `-Wall -Wextra -Wshadow -Wconversion -Wold-style-cast -Wdouble-promotion…`
   (`-Werror` en intégration continue).
5. Mise en forme par `clang-format` (`.clang-format`, 120 colonnes).
6. Tout changement de résultat physique s'accompagne d'un test qui le justifie.

## Les tests

| Groupe | Contenu |
| --- | --- |
| `constants`, `core`, `linalg` | Valeurs CODATA 2022, cohérence c²μ0ε0 = 1, alias, Vec3, sinCos rapide contre la bibliothèque, solveur dense, solveur complexe par blocs contre le solveur simple |
| `electrostatics` | Coulomb, sphères, coquille, ligne et plan infinis, théorème de Gauss pour toutes les sources finies, champ lointain, superposition, plaques et pavé exacts comparés à des quadratures indépendantes, saut de σ/ε0 à la traversée, potentiel relatif des sources infinies (−grad V = E) |
| `magnetostatics` | Fil infini, théorème d'Ampère, circulation de H entre deux points (I/4 pour un quart de tour, courant enlacé sur un contour fermé, n·I·l dans un solénoïde), boucle exacte (y compris à 1 µm du fil et axe incliné), Helmholtz, dipôle, solénoïde exact comparé à une intégration convergée et à la formule sur l'axe, div B = 0 et rot B = 0 hors des courants |
| `app` | Saisie des unités, annuler/rétablir (un glisser = une étape), enregistrement JSON, saisie par densité, carte de champ en arrière-plan, module Ondes (réglages annulables, fichiers, exemples, simulation sur son fil de calcul, cavité excitée sur la résonance du mode TM₃₂), module Antennes (éléments, réglages annulables, fichiers, calcul en arrière-plan, balayage et péremption, optimiseur en une étape annulable, banc, fils tracés au concepteur et raccordés, source posée sur un fil, transformations d'une sélection (déplacement, réseau, rotation, miroir, alignement, suppression, glisser groupé, bout de fil glissé, coupe), chacune en une étape annulable, groupes et contraintes (suivis à chaque modification, suspension, refus, renommage, enregistrement)), point B en statique (tension, écarts, circulation de H, profils), échelles de couleurs selon le fond clair ou sombre, demandes de carte plus rapides que les calculs (aucune famine, carte inutile abandonnée) ; surfaces du module Antennes (source au pied d'un fil, lecture des courants de surface et de la polarisation, réglage du maillage annulable et enregistré) |
| `view3d` | Caméra orbitale (axes directs, projection et son inverse, vues exactes alignées sur les plans XY / XZ / YZ des vues 2D, pas de 45° des flèches du cube, cadrage, pointage d'un plan, zoom sous le curseur) et formes des sources dans l'espace (sphère, pavé tourné, sources infinies, sens des courants conforme à la règle de la main droite) |
| `waves` | Simulation d'une scène d'ondes : maillage, décroissance en 1/√r d'une source ponctuelle et isotropie, vitesse c/n et coefficient de transmission 2/(1+n) d'une impulsion, énergie d'une cavité fermée, TEz, fentes et sources linéaires discrétisées, modifications en cours de simulation, zones dessinées (peinture, gomme, traits fins continus), point B : rapport d'amplitude, déphasage et retard d'une onde plane |
| `antennas` | Géométrie des éléments (fils, dipôles droits et en V, dipôle replié, boucles, cadre, hélice), champ lointain recalculé depuis les courants contre celui du solveur et contre le bilan d'énergie, faisceau des Yagi et du couple déphasé vers +x, balayages, garde de maillage, optimiseur Yagi (score amélioré, disposition conservée, arrêt), banc de validation (tout conforme au maillage par défaut, écart signalé à 40 segments par λ), fil posé entre deux points, deux fils bout à bout = le dipôle d'un seul tenant, rotations et miroirs du concepteur (chaque point et la source à leur image, pour les six types d'éléments), fil coupé = même antenne, branche raccordée en T (courant dans la branche, loi des nœuds), fil posé sur une source refusé, contraintes du concepteur (les huit sortes, chaîne, contradiction signalée, référence supprimée, copie sans contraintes) ; surfaces : monopôle sur son plan de masse (résonance, lobe relevé, |J| = I / 2πρ autour du pied, maillage, pied décollé refusé), dipôle devant un réflecteur, hélice en mode axial (≈ 140 Ω, faisceau dans l'axe, polarisation circulaire droite), contours, points d'appui, rotation et miroir des plaques ; plaques jointes (plaque en trois morceaux = d'un seul tenant, réflecteur en coin contre le dipôle et ses images) ; antenne patch dans l'air (≈ 50 Ω, faisceau à la verticale, résonance de la plaque, tenue au maillage) |
| `fdtd` | Limite de Courant, absorption CPML et réflexion < −80 dB contre une boîte de référence (TMz et TEz), conservation de l'énergie en cavité PEC, vitesse de propagation, matériaux et sources posés en bloc identiques à la pose cellule par cellule |
| `mom` | Capacité d'une ligne bifilaire, mutuelle inductance de boucles coaxiales, garde de convergence Pocklington ; réseau de fils : dipôle λ/2 et longueur de résonance comparés à Hallén, convergence avec le maillage, puissance rayonnée = puissance fournie, rayonnement du moment de courant d'un dipôle court, directivités 2,15 et 1,76 dBi, réciprocité |
| `surfaces` | Solveur fils + surfaces `qsig::mom` : intégrales de 1/R sur un triangle contre une subdivision fine et de part et d'autre de son plan ; maillages (rectangle et disque, sommet sous chaque fil) ; fils seuls : impédance du solveur filaire à 10⁻⁸ près (dipôle, Yagi) ; ruban de largeur w = fil de rayon w/4 ; monopôle sur plaque : convergence avec le maillage, bilan de puissance à 2·10⁻³, oscillation autour de la théorie des images ; jonction fil-surface (Hwu et Wilton) contre une jonction en T de surfaces seules ; plaque dans le plan de symétrie d'un dipôle : aucun courant ; sources et fils refusés, avec leur raison |
| `validation` | Banc de validation des antennes, passivité du cas patch |
| `parity` | Le nouveau cœur reproduit la 6.14 à 10⁻⁷ près sur 15 sources électrostatiques, 11 magnétostatiques, 2 cas FDTD (Mur, PEC ; les cas CPML sont exclus depuis la correction de la couche absorbante), 3 réseaux de fils, BEM, boucles, banc de validation (boucle, monopôle ; le dipôle est exclu depuis la correction du banc) et calculatrice. Les sources remplacées par des formes exactes (plaques, disque, anneau, pavé, boucle, solénoïde, Helmholtz) en sont exclues : leurs tests propres les comparent à des références indépendantes |

### Convention `[defect]`

Un test marqué `[defect]` affirme le **bon** comportement physique et échoue aujourd'hui, à cause d'un défaut mesuré par
l'audit. Le harnais le compte comme « défaut confirmé ». S'il se met à passer, il est signalé en échec
(« FIXED? ») pour qu'on retire l'étiquette : une correction ne passe jamais inaperçue.

`qsig_tests --verbose "[defect]"` affiche les écarts mesurés :

| Défaut | Mesure dans la 6.14 | État |
| --- | --- | --- |
| Plaque rectangulaire chargée, champ sur l'axe | +10 % à 2 cm, +106 % à 1 cm, +578 % à 5 mm | **corrigé (phase 1)** : formule exacte, écart < 10⁻¹² |
| Disque chargé, champ sur l'axe | −48 % à 2 cm, −86 % à 5 mm | **corrigé (phase 1)** : intégrales de contour exactes, écart < 10⁻⁹ |
| Solénoïde, à 2 cm de l'enroulement | −17 % et +24 % selon z | **corrigé (phase 2)** : nappe de courant exacte, écart < 10⁻⁹ |
| Couche absorbante CPML (FDTD) | réflexion −38 dB (1,3 %) : profil magnétique pris sur les nœuds électriques | **corrigé (phase 2)** : profils E et H chacun à leur position de Yee, −95 dB mesuré contre une boîte de référence |
| Dipôle λ/2 (réseau de fils) | 82,4 + j23,5 Ω, contre R ≈ 88 Ω et X ≈ 50 ± 15 Ω (Hallén) | **corrigé (phase 3)** : la formulation « pulse » sous-estime la réactance quel que soit le maillage ; la formulation de Galerkin à fonctions triangulaires (par défaut) donne 87,2 + j47,2 Ω et converge (résonance à 0,4756 λ et 72,0 Ω contre 0,4727 λ et 72,1 Ω pour Hallén) |
| Fils minces (a < λ/1000), fonctions triangulaires | dipôle λ/2 à a = λ/10 000 : 15,7 − j1306 Ω, résultat faux sans message | **corrigé (phase 3)** : le noyau ∇∇G demandait un échantillonnage bien plus fin que le rayon ; forme à potentiels mixtes (dérivées reportées sur les fonctions triangulaires, partie 1/R intégrée exactement) : 80,3 + j45,3 Ω, stable avec le maillage, et 3 à 7 fois plus rapide |
| Fils + surfaces (solveur hybride fils + RWG) | monopôle λ/4 sur une plaque de λ : 65 + j35 Ω à 72 triangles, 56 + j9 Ω à 512, sans converger ; puissance rayonnée de 0,78 à 0,92 fois la puissance fournie ; 42 s pour 431 inconnues, 387 s pour 759 | **remplacé (phase 3b)** par `qsig::mom` (fils, surfaces et jonction fil-surface en potentiels mixtes) : 45,3 + j18,9 Ω à 128 triangles, 44,5 + j18,9 Ω à 512 ; bilan de puissance 1,0000 ; 47 ms pour 191 inconnues, 0,3 s pour 751 |
| Banc de validation | dipôle déclaré PASS avec 23,5 % d'écart d'impédance | **corrigé (phase 3)** : référence Hallén pour le même fil, chemin de production à fonctions triangulaires, PASS sous 3 % sur R : 1,1 % mesuré |

## Changements par rapport à la 6.14

### Conception par formes — lot 5, première partie : réflecteurs paraboliques, arc, spirale

Quatre formes de plus (demande de Jerry, 18/11) :

- Parabole (de révolution) : rayon de l'ouverture, distance focale ; le sommet est sa position, l'axe la direction
  de son ouverture, son foyer est marqué d'une croix dans la vue. Cylindre parabolique : largeur de l'ouverture,
  longueur le long de la ligne focale, distance focale. Surfaces ouvertes parfaitement conductrices ; un fil posé
  dessus y est relié. Maillage : la parabole par `meshRevolution` (profil coupé à pas égal le long de la courbe),
  le cylindre par `qsig::mom::meshSweep` (nouveau : un profil balayé le long d'une direction).
- Arc de fil : rayon, angle couvert ; passif, une source s'y place d'un clic et s'y glisse comme sur un fil droit
  (`feedSlides`, `curveFraction`). Spirale d'Archimède à deux bras : rayons intérieur et extérieur, nombre de tours ;
  alimentée au centre entre ses deux bras.
- Ruban, onglet Créer : « Courbe ▾ » (arc, hélice, spirale) remplace le bouton Hélice ; « Parabole ▾ » (parabole,
  cylindre parabolique) rejoint Plaque et Disque. Exemples : parabole avec un dipôle au foyer, cylindre parabolique
  avec un dipôle sur la ligne focale, spirale.
- Correction au passage : une copie miroir d'un cône le retournait (sa pointe partait du mauvais côté) ; un cône et
  les réflecteurs prennent maintenant l'image de leur axe.

Vérifications (tests du cœur) :

| Cas | Résultat |
|---|---|
| Parabole de 2 λ d'ouverture, f/D = 0,4, dipôle au foyer | 12,21 dBi (12,23 dBi maillée plus fin) ; optique physique, calculée à part en Python : 12,03 dBi ; rapport avant/arrière 18,5 dB (17,2 dB en optique physique, qui ignore les bords) |
| Aire du maillage de cette parabole | à 2 % de l'aire exacte du paraboloïde |
| Parabole de focale 200 λ contre disque, sous un dipôle à λ/4 | 88,67 + j18,38 Ω contre 88,72 + j18,40 Ω |
| Cylindre parabolique de focale 200 λ contre plaque | 87,35 + j17,80 Ω contre 87,38 + j17,79 Ω |
| Cylindre parabolique 2 × 1 λ, f = 0,5 λ, dipôle sur la ligne focale | 9,43 dBi, 62,4 − j35,8 Ω, identique maillé plus fin |
| Arc de 5 λ de rayon, 0,47 λ de long, contre dipôle | 69,13 − j10,28 Ω contre 69,16 − j10,25 Ω |
| Spirale à deux bras, 4 tours de 0,02 à 0,25 λ | 426 Ω, 3,9 dBi, rapport axial 0,2 dB le long de l'axe ; identique à 60 et 100 segments par λ |

Limites : la spirale n'est pas comparée à un cas publié, et ses bras s'arrêtent sans charge, si bien que son
rapport axial varie avec la fréquence (de 0,2 à 4 dB entre 270 et 420 MHz pour l'exemple à 300 MHz). Une parabole
se maille vite en beaucoup de triangles : au-delà de 2 à 3 λ d'ouverture, la limite d'inconnues est atteinte.

### Plan de construction orientable et pivot des rotations (demande de Jerry, 18/11)

- Plan de construction (ruban, Construire › Plan) : en plus de l'orientation (XY, XZ, YZ) et de la position, trois
  angles Rx, Ry, Rz, comme dans la 6.14 : rotations autour des axes x, puis y, puis z du monde, autour du point
  où le plan coupe la normale de son orientation (z = position pour XY). La grille, le tracé des fils, la pose
  des éléments, le glisser des éléments, la copie miroir dans le plan, la duplication et la rotation « dans le
  plan » suivent le plan tourné ; l'accrochage se fait sur la grille du plan lui-même. « Vue de face » tourne la
  vue pour voir le plan de face (`OrbitCamera::faceTowards`, testé) ; « Remettre à plat » annule la rotation.
  Une vue exacte (cube) garde le plan qu'elle regarde, sauf si c'est le plan tourné lui-même.
- Pivot des rotations : les poignées de rotation tournaient toujours autour du centre de la sélection, quel que
  soit le choix du menu Pivot (d'où « tourner autour du pivot ou de l'origine ne marche pas »). Le choix est
  maintenant tenu par la vue (`AntennaView3D::rotationPivot`) et vaut pour les poignées, les menus et le panneau ;
  les poignées sont dessinées autour du point choisi, relié à la sélection par un trait pointillé.
- Menu Pivot : « Placer un pivot (cliquer dans la vue) », outil Pivot (touche P, outil 6 de la vue) : un clic sur
  le plan ou sur un bout de fil place le pivot, les rotations tournent alors autour de lui, et l'outil Sélection
  revient. Le menu montre les coordonnées du pivot placé.

### Conception par formes, milieux et sources — lot 4, première partie : solides diélectriques

Un solide (pavé, cylindre, cône, sphère) est fait d'un milieu : le conducteur parfait, ou un diélectrique de
permittivité relative εr et de pertes tan δ. Le champ traverse un diélectrique, plus lentement que l'air : il
dévie l'onde, la concentre, et en absorbe une part s'il a des pertes.

- Solveur `qsig::mom` : équivalence de surface (formulation de Poggio, Miller, Chang, Harrington, Wu et Tsai).
  La peau d'un corps diélectrique porte un courant électrique J = n × H et un courant magnétique M = −n × E,
  tous deux en fonctions de Rao-Wilton-Glisson ; ils rayonnent dans l'air avec les fils et les conducteurs,
  et leurs opposés dans le milieu du corps. Continuité de E et de H tangentiels à travers la peau :
  lignes E, (Z1 + Z2) J − (K1 + K2) M ; lignes H, (K1 + K2) J + (Z1/η1² + Z2/η2²) M. Z est l'opérateur du champ
  électrique déjà là, dans chaque milieu (nombre d'onde complexe avec des pertes) ; K est l'opérateur de
  rotationnel, nouveau (`triCurl`), dont la partie singulière en 1/R³ est intégrée exactement.
- Champ lointain : les courants magnétiques rayonnent aussi (`Radiator::addMagneticMoment`).
- Modèle : `AntennaElement::medium`, `permittivity`, `lossTangent`, `elementIsDielectric`. Une peau
  diélectrique est maillée plus fin (la longueur d'onde y est plus courte) et compte double : limite portée à
  3 000 inconnues quand la scène contient un diélectrique. Exemple « Dipôle près d'une sphère diélectrique ».
- Scène, vue, interface : champ « Milieu » d'un solide (conducteur parfait / diélectrique), εr, tan δ ; un
  solide diélectrique est rempli de la couleur d'accent ; la branche « Milieux » de l'arbre liste l'air, le
  conducteur parfait et chaque diélectrique utilisé.

Vérifications (tests du cœur) :

| Cas | Résultat |
|---|---|
| Opérateur de rotationnel d'un triangle contre intégration brute | accord à 1e-6 près, dans l'air et dans un milieu à pertes |
| Sphère εr = 4, k a = 1, dans une onde plane, contre la série de Mie | puissance diffusée et rétrodiffusée à −3 % avec 8 triangles par longueur d'onde dans le diélectrique, −1 % avec 12 |
| Même sphère, k a = 1,5 | puissance diffusée à −0,5 % |
| Corps de permittivité 1,0001 près d'un dipôle | impédance du dipôle inchangée (0,01 Ω pour une sphère, 0,08 Ω pour un pavé) |
| Dipôle à 0,3 λ d'une sphère εr = 4 de rayon 0,16 λ | 65,05 − j5,46 Ω (65,10 − j5,38 Ω maillé plus fin), 4,0 dBi contre 2,1 seul, bilan de puissance 1,0000 |
| Dipôle près d'un pavé de 0,3 λ, εr = 4 | 67,34 − j2,75 Ω (67,31 − j2,74 Ω maillé plus fin) ; avec tan δ = 0,1, 94 % de la puissance fournie est rayonnée |

Ce qui n'est pas encore calculé, et refusé avec un message qui nomme les éléments : un conducteur posé sur un
diélectrique ou qui le traverse (le patch sur son substrat, la sonde dans le substrat : seconde partie du
lot), deux diélectriques qui se touchent. Aucun fil ne s'attache à un diélectrique. Temps de calcul : environ
3 s pour 1 300 inconnues, 13 s pour 3 000, sur ma machine.

### Ruban du concepteur en deux onglets (demande de Jerry, 18/11)

Avec les groupes de création par nature et les solides, l'onglet Conception ne tenait plus dans la largeur de
la fenêtre : il fallait faire défiler le ruban. Ses outils sont triés en deux onglets, qui montrent tous deux
la vue de conception :

- « Créer » : Sélection · Créer un fil · Créer une surface · Créer un solide · Alimenter · Construire ·
  Inspecter (17 boutons) ;
- « Modifier » : Sélection · Modifier · Assembler · Construire · Inspecter (16 boutons).

Sélection, Construire (plan, pas, pivot) et Inspecter (mesurer) sont sur les deux. Les trois outils de vue
(Vues, Cadrer, Affichage) quittent le ruban : ils sont à droite de la rangée d'onglets, en petits boutons
(`compact` de `RibbonButton`), accessibles depuis les deux onglets. Un outil pris au clavier ou par un menu
ouvre l'onglet qui le porte (`followTool`). L'onglet de conception ouvert est retenu d'une session à l'autre.
Dans `AntennaViewport.qml` : `designTab` (0 Créer, 1 Modifier), `designing`, et `tab` vaut maintenant 0 ou 1
pour la conception, 2 Simulation, 3 Optimiser, 4 Validation. Le défilement du ruban reste en secours pour
une fenêtre étroite.

### Conception par formes, milieux et sources — lot 3 : solides conducteurs

Quatre types d'éléments de plus : pavé, cylindre, cône, sphère, parfaitement conducteurs. Un solide est une
peau fermée ; il ne s'alimente pas lui-même, un fil dont un bout est posé sur sa peau y est relié.

- Modèle (`antenna_model.*`) : types `Box`, `Cylinder`, `Cone`, `Sphere`, champ `height` ; `elementIsSolid`
  (un solide est aussi une « surface » pour tout ce qui se dit des surfaces) ; `AntennaShape::faces` (la peau
  en polygones, pour la remplir et la saisir par l'intérieur) ; `projectOnSurface` et `surfaceAt` étendus à la
  peau des solides. Deux exemples : « Monopôle sur un boîtier », « Monopôle sur une sphère ».
- Solveur : un pavé est fait de ses six faces jointes (maillage conforme du lot précédent) ; cylindre, cône et
  sphère sont des surfaces de révolution (`qsig::mom::meshRevolution`, nouveau : anneaux cousus par des
  triangles, peau fermée), dont le sommet le plus proche vient sous le pied d'un fil.
- Conducteurs superposés dans un même plan (pavé posé sur un plan de masse, plaques qui se chevauchent) : ils
  sont découpés pareil là où ils se recouvrent et l'un des deux y perd ses cellules, pour que le conducteur n'y
  soit qu'une fois. Les bouts d'une ligne de contact sont maintenant ceux des plaques elles-mêmes (une plaque
  qui traverse un pavé donnait un triangle dégénéré au coin).
- Scène, vue, interface : champs (largeur, longueur, hauteur, rayon, axe), boutons du ruban actifs, branche
  « Solides » de l'arbre, remplissage et sélection par les faces, filtre « Surfaces et solides ».

Vérifications (tests du cœur, 17 groupes) :

| Cas | Résultat |
|---|---|
| Pavé 0,4 × 0,4 × 0,2 λ, monopôle 0,24 λ au milieu du dessus | 27,27 − j4,30 Ω, identique à six plaques jointes ; 27,06 − j4,40 Ω à 16 triangles par λ |
| Fil posé hors du centre, ou sur un flanc du pavé | bilan de puissance 1,000 ; écart entre 10 et 16 par λ inférieur à 0,6 Ω |
| Tube fermé de rayon 0,005 λ contre fil de même rayon (brin parasite d'un dipôle) | 44,70 + j22,55 Ω contre 44,75 + j22,59 Ω |
| Monopôle sur une sphère de rayon 0,2 λ | 51,0 − j9,1 Ω à 10 par λ, 49,0 − j11,0 Ω à 16 (facettes : la sphère maillée est plus petite) |
| Monopôle sur un cylindre (bout, paroi), sur la base d'un cône | bilan 1,000 ; écart entre densités inférieur à 0,8 Ω (1,3 Ω sur la paroi) |
| Deux plaques qui se chevauchent de 0,2 λ contre la plaque de leur réunion | 61,86 − j1,13 Ω des deux côtés, mêmes 200 triangles |
| Pavé posé sur un plan de masse de 1 λ, monopôle dessus | 11,81 − j13,72 Ω, que la plaque ou le pavé soit en premier |

Limites, à connaître :

- La sphère et le cône ne sont validés que par le bilan de puissance et la convergence du maillage, pas contre
  un cas publié. Le tube contre le fil s'écarte quand le rayon grandit (1,3 Ω à 0,01 λ, 3 Ω à 0,02 λ) : c'est le
  modèle de fil mince qui décroche, pas le solide.
- Équation du champ électrique sur une peau fermée : elle perd en précision près des fréquences de résonance de
  la cavité d'un solide de plus d'une demi-longueur d'onde.
- Les solides courbes ne sont pas reliés aux plaques ni aux solides qu'ils touchent ; seul un fil s'y attache.
- 2 000 inconnues au plus.

### Conception par formes, milieux et sources — lot 2 : ruban et arbre par nature

Lot d'interface (QML, plus une fonction de la vue) : la création et la scène sont rangées par nature, comme
le plan le prévoit. Aucun changement du calcul.

- Ruban, onglet Conception : le groupe « Créer » (Fil, Élément ▾) devient trois groupes. « Créer un fil » :
  Fil (outil de tracé), Dipôle ▾ (droit ou en V, replié), Boucle ▾ (circulaire, cadre), Hélice. « Créer une
  surface » : Plaque, Disque. « Créer un solide » : Pavé, Cylindre, Cône, Sphère, grisés (`soon` de
  `RibbonButton`) tant que les solides n'existent pas ; leur bulle le dit. Icônes dessinées dans
  `RibbonIcon.qml` (dipole, loop, helix, plate, discflat, box, cylinder, cone, sphere).
- Panneau Scène des Antennes : un arbre à la place des onglets Tous / Sources / Fils / Surfaces / Groupes.
  Branches repliables avec leur compte : Fils, Surfaces, Solides (vide), Sources, Milieux (Air, Conducteur
  parfait : seuls milieux aujourd'hui), Groupes. Un clic sur un groupe sélectionne tous ses éléments
  (`AntennaView3D::selectGroup`) ; ses éléments sont listés sous lui. Les autres modules gardent leur liste.
- Menu contextuel « Créer » de la vue : fils, puis surfaces.

Préparation du lot 3 (solides), dans le cœur, testée ici :

- Maillages conformes des plaques jointes repris (`solveWithSurfaces`). Avant, chaque plaque recevait tous
  les sommets de ses voisines posés sur elle : dès qu'un fil se posait sur une face d'un pavé, chaque face
  finissait découpée aussi fin que toutes ses voisines réunies. Maintenant les plaques n'échangent que leurs
  lignes de grille imposées (pied d'un fil, bout d'une ligne de contact, côtés) : là où une telle ligne
  rencontre une autre plaque, celle-ci y passe aussi, de proche en proche autour d'un pavé ; entre ces lignes
  chaque plaque se découpe régulièrement, de la même façon des deux côtés d'un contact.
- Mesure : pavé fermé de 0,4 × 0,4 × 0,2 λ en six plaques, monopôle de 0,24 λ au milieu du dessus, à
  16 triangles par λ : 792 inconnues au lieu de 1 872, 27,06 − j4,40 Ω (27,05 − j4,57 Ω avant, 27,27 − j4,30 Ω
  à 10 par λ), bilan de puissance 1,0000. Plaque en trois morceaux contre plaque entière : 61,84 − j1,13 Ω
  contre 61,86 − j1,13 Ω, inchangé. Nouveau cas de test « a closed box of six joined plates ».

### Conception par formes, milieux et sources — lot 1 : les sources en entités

Plan validé par Jerry (formes : fils, surfaces, solides ; milieux qu'on leur attribue ; sources posées sur
des fils). Premier lot : une source est une entité posée sur un fil, plus une propriété « alimenté » de
l'élément.

- Modèle (`qsig/app/antenna_model.h`) : `AntennaFeed` (place le long du fil, tension, phase) et les fonctions
  `feedCount`, `feedAt`, `setFeedAt`, `addFeed`, `removeFeed`, `canAddFeed`. Un fil droit porte autant de
  sources qu'on veut (chacune a son sommet dans le maillage et son résultat, `AntennaFeedResult::feedIndex`) ;
  un élément tout fait (dipôle, dipôle replié, boucle, cadre, hélice) arrive avec la sienne, à la place que lui
  donne son type. Une surface n'en porte pas. Deux sources au même endroit d'un fil : refusé avec un message.
  Un fil coupé garde ses sources, chacune sur la partie qui la porte.
- Tests du cœur : un fil d'une longueur d'onde à deux sources (au quart et aux trois quarts) donne les mêmes
  impédances et la même directivité que deux fils demi-onde bout à bout alimentés chacun en son milieu (à
  1e-6 près) ; réciprocité entre les deux sources d'un même fil (courants égaux à 1e-9 près) ; ajout, retrait,
  coupe.
- Scène : `feedList` (une entrée par source : « Source n », son fil, sa place, sa tension), `placeFeed` (sur
  un fil déjà alimenté, une source de plus ; sur une source déjà là, rien), `addFeedTo`, `moveFeed` (glisser,
  un seul pas d'annulation), `deleteFeed`, `setFeedValue`. Fichiers : les sources supplémentaires d'un fil sont
  écrites dans `sources` ; les fichiers existants se relisent tels quels.
- Concepteur : une source se sélectionne d'un clic sur son point rouge (elle est cerclée au survol), seule,
  jamais avec des éléments. Sur un fil tracé, on la glisse le long du fil (les quarts et le milieu l'attirent,
  le bout posé sur une surface aussi). Suppr ou l'outil Supprimer la retirent sans son fil. Sur un élément tout
  fait, un clic sur la source la sélectionne et un glisser déplace l'élément, comme partout ailleurs sur lui.
  Le filtre de sélection « Sources » ne prend que les sources.
- Panneaux : l'onglet « Sources » du panneau Scène liste les sources (plus les éléments alimentés) ; le
  panneau Propriétés montre la source sélectionnée (son fil, sa position, sa tension, sa phase) ; celui d'un
  élément ne montre plus que sa géométrie, avec la liste de ses sources et « Placer une source ».

### Phase 3b, première étape (surfaces conductrices)

- Solveur fils + surfaces réécrit : `qsig::mom` (`physics/include/qsig/moment_method.h`). Le solveur hybride
  de la 6.14 a été mesuré avant d'être repris, puis écarté (voir le tableau des défauts) : sur un monopôle
  quart d'onde au centre d'une plaque d'une longueur d'onde, son impédance dérivait avec le maillage, il
  rayonnait de 78 à 92 % de la puissance fournie, et il lui fallait 42 s pour 431 inconnues.
  - Une seule formulation pour tout : équation intégrale du champ électrique en potentiels mixtes, testée par
    les fonctions de base (Galerkin). Fils : fonctions triangulaires et noyau réduit, par les intégrales déjà
    validées du solveur filaire. Surfaces : fonctions de Rao-Wilton-Glisson sur des triangles, la partie 1/R
    intégrée exactement (formules de Wilton et al., 1984).
  - Un fil dont le bout touche une surface y est relié : son courant se répartit autour du sommet de maillage
    où il arrive par la fonction de jonction de Hwu et Wilton (densité en 1/ρ). Le maillage d'une plaque passe
    par les points où les fils arrivent ; sur un disque, le sommet le plus proche vient sous le fil.
  - Sources : en un point d'un fil ; au pied d'un fil posé sur une surface, entre le fil et la surface
    (monopôle) ; en travers d'une arête d'un maillage.
  - Vérifié (groupe de tests `surfaces`) : fils seuls, même impédance que le solveur filaire à 10⁻⁸ près ;
    ruban de largeur w contre fil de rayon w/4 (68,8 − j11,8 Ω contre 69,2 − j10,2 Ω) ; monopôle sur plaque,
    45,3 + j18,9 Ω puis 44,7 + j18,9 Ω quand le maillage passe de λ/8 à λ/12, puissance rayonnée égale à la
    puissance fournie à 10⁻⁴ près ; quand la plaque grandit, l'impédance oscille autour de la moitié de celle
    du dipôle de longueur double (théorie des images : 42,7 + j22,5 Ω) ; jonction fil-surface contre une
    jonction en T de surfaces seules, 22,96 + j5,2 Ω contre 22,92 + j6,3 Ω à λ/40, puis 22,87 + j5,51 Ω contre
    22,95 + j5,53 Ω à λ/80 ; une plaque posée dans le plan de symétrie d'un dipôle ne porte aucun courant
    (10⁻¹⁴ A/m).
  - Un modèle en grille de fils de la même plaque, résolu par la partie filaire, tend vers le même résultat à
    mesure que la grille se resserre (de 40,5 + j17,4 Ω à λ/8 à 42,6 + j19,8 Ω à λ/40, contre 44,6 + j20,5 Ω ;
    directivité de 3,07 à 3,56 dBi, contre 3,79 dBi) : la convergence d'une grille est lente, d'où le choix de
    vraies surfaces.
  - Vitesse : 47 ms pour 191 inconnues, 0,13 s pour 423, 0,3 s pour 751, 1,4 s pour 1695 (limite de l'application :
    2000). Au-delà de quelques centaines d'inconnues, la résolution du système domine : elle se fait par blocs
    de colonnes et sur plusieurs cœurs (`linalg::solveComplexBlocked`). Mesuré à 1700 inconnues sur la machine
    de développement : 2,7 s par l'élimination simple, environ 0,75 s par blocs.
- Module Antennes : deux nouveaux éléments, la plaque conductrice (rectangle) et le disque conducteur, placés et
  orientés comme les autres (leur axe est leur normale). Ils ne portent pas de source.
  - Un fil dont un bout est posé sur une surface y est relié, sans autre geste. Dans le concepteur, un point
    tracé à moins de 8 pixels d'une surface se pose dessus (« posé sur la surface »).
  - Source au pied : la position de la source d'un fil ou d'une hélice va jusqu'à 0 et 1, le bout lui-même ;
    l'outil Source cliqué près d'un bout posé sur une surface l'y place. Une source au bout libre d'un fil est
    refusée avec son explication.
  - Hélice en mode axial : la même hélice, alimentée au pied (position 0) sur un plan de masse.
  - Vue 3D : triangles des surfaces colorés par |J| (courant de jonction compris : autour du pied d'un monopôle,
    |J| = I / 2πρ, vérifié par un test), contour simple pendant un glisser. Les surfaces se sélectionnent par
    leur contour, pour que glisser sur une grande plaque tourne toujours la vue.
  - Résultats : groupe « Surfaces » (|J| maximal, nombre de triangles) et polarisation du lobe principal quand
    elle n'est pas linéaire (rapport axial, sens droit ou gauche).
  - Réglage « Maillage des surfaces : triangles par longueur d'onde » (10 par défaut, de 4 à 40), annulable et
    enregistré avec la scène.
  - Exemples : monopôle λ/4 sur plan de masse (0,24 λ sur une plaque de λ : 39,5 + j1,2 Ω, TOS 1,27, 3,9 dBi à 45°
    au-dessus de l'horizon) ; dipôle à 0,2 λ d'un réflecteur plan (61,9 − j1,1 Ω, 8,5 dBi, avant/arrière 16,8 dB) ;
    hélice de Kraus en mode axial sur un disque (C = λ, 13°, 5 tours : 142 − j64 Ω, 10,2 dBi dans l'axe,
    polarisation circulaire droite, rapport axial 0,4 dB).
  - Une scène sans surface passe par le solveur filaire, comme avant : résultats inchangés au bit près.
- Vue 3D du module Antennes, à la demande de Jerry (reprises de la 6.14) :
  - fenêtre flottante « Outils de la sélection » (transformer, groupes, contraintes) : elle se déplace par sa
    barre de titre, se replie sur cette barre (« – » ou double clic) et se ferme (« × ») ; les boutons Déplacer
    et Contraintes de l'en-tête d'outils l'ouvrent. Position, état et repères affichés sont mémorisés d'une
    session à l'autre (`ui/qml/FloatingPanel.qml`). La fenêtre « Affichage » des premières versions est passée
    dans l'en-tête d'outils (voir plus bas) ;
  - menu du clic droit. Sur un élément (sélectionné au passage, avec son groupe) : dupliquer, grouper, dégrouper,
    tourner de ±45° dans le plan de construction ou autour de x, y, z, copie miroir (x, y ou z inversé, ou plan
    de construction), aligner, ajouter une contrainte, placer la source ici, placer le pivot de rotation ici,
    supprimer. Dans le vide : créer un élément de n'importe quel type à l'endroit du clic (sur le plan de
    construction), mesurer, placer ou retirer le pivot, tout sélectionner, cadrer la vue ;
  - pivot de rotation placé dans la scène (losange orange), troisième choix de « Axe passant par » ; les
    copies miroir du menu passent par lui, ou par l'origine quand il n'y en a pas ;
  - flèches x, y, z sur la sélection (outil Sélection) : en glisser une déplace la sélection le long de cet
    axe, sur un dixième de la grille (Maj : libre), en une seule étape annulable ;
  - axes de la scène tracés depuis l'origine (+x, +y, +z) ; ces axes, les flèches de la sélection et le petit
    repère du coin se masquent séparément (en-tête d'outils, Vue → Affichage, « Repères ») ;
  - la mesure donne aussi l'azimut et l'élévation du segment mesuré ;
  - différences avec la 6.14 : le menu « Créer » propose les types d'éléments qui existent ici (pas encore
    d'arc, de spirale, de méandre, de cylindre, de cône, de parabole ni de substrat) ; pas de confirmation à la
    suppression, puisqu'elle s'annule ; les flèches déplacent, l'anneau de rotation de la 6.14 n'est pas repris.
- En-tête d'outils (« ruban ») du module Antennes, à la demande de Jerry, sur le modèle des logiciels de
  conception. Stratégie et choix : document de projet `claude/strategie-ruban-outils.md`.
  - Un seul en-tête sur toute la largeur, au-dessus de la scène, de la vue et des propriétés. Il remplace les
    boutons de pages centrés, la bande d'outils de la vue 3D (qui n'était pas alignée sur eux), la fenêtre
    « Affichage » et les réglages posés en haut de chaque page.
  - Quatre onglets : Conception (la vue 3D où l'on dessine l'antenne), Simulation (Rayonnement 3D, Diagrammes,
    Balayage, Smith : la dernière page vue revient avec l'onglet), Optimiser, Validation. Sous les onglets, les outils de l'onglet par groupes nommés, chaque
    bouton avec son icône et son libellé :
    Sélection (Pointeur) · Créer (Fil, Élément ▾) · Alimenter (Source) · Modifier (Déplacer, Couper, Dupliquer,
    Tourner ▾, Miroir ▾, Aligner ▾, Supprimer) · Assembler (Grouper, Dégrouper, Contraintes) · Construire
    (Plan ▾, Pivot ▾) · Inspecter (Mesurer) · Vue (Vues ▾, Cadrer, Affichage ▾).
    Simulation : choix de la page, puis ses réglages (échelle du rayon et dynamique du diagramme 3D ; second
    plan et dynamique des coupes ; lancement, bande et grandeur du balayage). Optimiser et Validation : le bouton qui lance le calcul (leurs réglages restent dans
    la page).
  - « Supprimer » retire la sélection ; sans sélection, il devient l'outil Supprimer (touche X). « Élément »
    pose l'élément choisi au centre de la vue, sur le plan de construction. Les raccourcis V, F, S, M, X, C
    sont inchangés.
  - Conception et rendu séparés, à la demande de Jerry : le diagramme de rayonnement n'est plus dessiné dans la
    vue de conception (ses milliers de facettes ralentissaient chaque geste) ; il a sa vue, « Rayonnement 3D »,
    dans l'onglet Simulation. C'est une vue de rendu (`AntennaView3D.readOnly`) : on y tourne, on y zoome, rien
    ne s'y sélectionne ni ne s'y déplace. Rayon en dB sur 40 dB par défaut (30 à 40 dB détachent les lobes ; à
    60 dB tout se tasse en boule), ou proportionnel au champ ou à la puissance. Pendant qu'on tourne la vue, une
    facette sur quatre est dessinée, sans lissage ; le maillage complet revient au relâchement.
  - Poignées de rotation et pas réglables, à la demande de Jerry (reprise de la 6.14) : sur la sélection, à côté
    des trois flèches, trois quarts de cercle aux couleurs des axes ; en glisser un fait tourner la sélection
    autour de cet axe, par pas (15° par défaut), avec l'angle affiché et la sélection tournée tracée en
    pointillés pendant le geste ; au relâchement, une seule rotation annulable autour du centre de la
    sélection. Le bouton « Pas » du groupe Construire règle le pas de rotation et le pas de déplacement
    (0 : un dixième de la grille, comme avant) ; Maj libère les deux. Les deux pas sont mémorisés.
  - Sélection, à la demande de Jerry : une plaque ou un disque se prend aussi par l'intérieur (plus seulement
    par le bord) ; un fil à portée du pointeur passe devant, et de deux surfaces sous le pointeur c'est la plus
    proche de l'œil qui est prise. Bouton « Tout ▾ » du groupe Sélection : filtre de ce que le clic prend (et de
    ce que prend Ctrl + A) : tout, les fils, les surfaces, ou les sources (lot 1 des sources en entités).
  - Panneau Scène des Antennes en table par nature : Tous, Sources (éléments alimentés), Fils, Surfaces,
    Groupes ; le groupe d'un élément est écrit sur sa ligne. Les diélectriques y auront leur onglet quand ils
    existeront. Le module Antennes s'ouvre sur une scène vide (plus de Yagi chargée d'office ; les
    exemples restent dans le panneau Scène).
  - Dans une fenêtre plus étroite que les groupes, l'en-tête défile (molette, glisser, ou barre sous les groupes).
  - Code : `ui/qml/RibbonGroup.qml`, `RibbonButton.qml`, `RibbonIcon.qml` (icônes tracées au trait dans la
    couleur du thème, sans fichier d'image) ; l'en-tête est décrit dans `AntennaViewport.qml` et posé dans
    l'emplacement `moduleRibbon` de `Main.qml`. Chaque commande est déclarée une fois (`Action`) et sert à
    l'en-tête comme aux menus du clic droit.
  - Survol du diagramme 3D, comme dans la 6.14 : le point du diagramme sous le pointeur est marqué, avec θ, φ,
    le gain en dBi, le niveau relatif au maximum et le champ normalisé ; la direction du maximum est repérée
    en permanence.
  - Le même en-tête pour les modules de champ (Électrostatique, Magnétostatique, et les Ondes qui partagent
    leur vue `FieldViewport.qml`) : il remplace la barre flottante posée sur la carte. Onglets « Coupe 2D » et
    « Vue 3D » (ou « Simulation » pour les Ondes) ; groupes Simulation (Lancer, Pas à pas, Réinitialiser,
    Vitesse), Plan de coupe, Grandeur, Afficher (Couleurs, Lignes de champ, Iso, Flèches ou Vecteurs, Grille,
    en boutons à bascule) et Vue.
  - Ondes : un second onglet, « Dessin », porte les outils de l'ancienne palette flottante (Sélection, Pinceau,
    Gomme, Rectangle, Disque, Trait, Point B) et les options de l'outil en cours (matériau, εr, largeur). Un
    outil pris par son raccourci ouvre cet onglet ; revenir sur « Simulation » rend la main au pointeur.
  - Lecture des valeurs au survol, comme dans la 6.14 : sur les diagrammes polaires (angle, gain, niveau
    relatif, avec un rayon en pointillés), sur le balayage (fréquence et valeur de chaque courbe au point le
    plus proche, trait vertical) et sur l'abaque de Smith (fréquence, impédance, |S11|, TOS). Le mécanisme est
    commun aux trois (`AntennaPlotBase::setPointer`, `drawReadout`).
- Plaques reliées entre elles (phase 3b, deuxième étape) : deux plaques qui se touchent le long d'une ligne
  parallèle à un côté de chacune (coin, T, croix, boîte, plaques côte à côte) sont jointes, le courant passe de
  l'une à l'autre. Leurs maillages sont rendus conformes avant le calcul : les deux bouts de la ligne de contact
  entrent dans les deux grilles, puis chaque sommet d'un maillage posé sur une autre plaque y est ajouté, jusqu'à
  ce que plus rien ne manque (`contactLine` et `solveWithSurfaces` dans `antenna_model.cpp`).
  - Vérifié : un réflecteur d'une longueur d'onde en trois morceaux aux découpes différentes (0,45 et 0,55 λ
    sous 1 λ) donne 61,84 − j1,13 Ω, contre 61,86 − j1,13 Ω d'un seul tenant, même directivité, même rapport
    avant/arrière ; un morceau seul donne 66 − j25 Ω.
  - Exemple « Dipôle dans un réflecteur en coin (90°) » : dipôle à 0,5 λ de l'arête, panneaux de 1 λ :
    127,4 − j2,8 Ω, 10,6 dBi, avant/arrière 22,5 dB. Référence : le coin à côtés infinis est le dipôle et ses
    trois images (courants + − + −), soit la même impédance et quatre fois la directivité des quatre dipôles
    (11,8 dBi). L'impédance des panneaux finis rejoint celle des images à 5 Ω près pour 1 λ, à 1 Ω près pour
    1,5 λ ; leur directivité va de 10,6 dBi (1 λ) à 13,4 dBi (côtés de 2 λ, hauteur de 1,5 λ).
- Antenne patch dans l'air (phase 3b, deuxième étape) : exemple « Antenne patch dans l'air (sonde) », une plaque
  de 0,46 × 0,5 λ à 0,03 λ au-dessus d'un plan de masse de 1 λ, alimentée par une sonde entre les deux à 0,12 λ
  du centre : 50,0 − j1,2 Ω (TOS 1,02), 9,8 dBi à la verticale du patch, avant/arrière 18,5 dB.
  - L'écart entre les deux plaques est trois fois plus petit que la maille : le solveur tient. De 10 à 20
    triangles par longueur d'onde, l'impédance bouge de quelques ohms (50,0 − j1,2 ; 46,7 + j1,1 ; 44,6 + j1,7 Ω),
    la résonance ne se déplace pas, le bilan de puissance reste à 1,0005.
  - Résonance demi-onde de la plaque : R maximal (117 Ω) pour une longueur de 0,445 λ, où il reste la réactance
    de la sonde, +50 Ω (formule usuelle d'une sonde de cette taille : +59 Ω). La formule de Hammerstad pour
    l'allongement par les champs de bord donne 0,458 λ : 3 % d'écart, de l'ordre de la précision de ces formules.
- Limites : surfaces planes (rectangle, disque) dans l'air, parfaitement conductrices ; des plaques qui se
  croisent en biais (ligne de contact oblique par rapport à leurs côtés) et les disques ne sont pas reliés aux
  autres surfaces ; un fil se relie à une surface par ses bouts seulement ; près du bord d'un disque (moins d'une
  maille), un fil est refusé. Substrats diélectriques et antenne patch : étape suivante.

### Phase 3 (antennes filaires)

- Module Antennes : fil droit (passif ou alimenté en un point), dipôle droit ou en V, dipôle replié, boucle
  circulaire, cadre rectangulaire, hélice en mode normal ; position, orientation (azimut, élévation, rotation),
  rayon du fil, tension et phase de la source. Chaque modification est recalculée en arrière-plan.
- Résultats dans l'inspecteur : impédance d'entrée R + jX, |S11| et TOS sur Z₀ réglable, courant et puissance de
  chaque source ; gain, rapport avant/arrière, direction du lobe principal, puissance rayonnée.
- Vues : 3D (fils colorés selon |I|, sources, diagramme de rayonnement 3D, rotation à la souris), diagrammes
  polaires (plan horizontal, plan vertical du lobe principal et plan perpendiculaire, ouverture à −3 dB),
  balayage en fréquence (|S11|, TOS, R et X, gain et avant/arrière ; clic : analyser à cette fréquence ; calculé
  en parallèle, signalé périmé quand la scène change), abaque de Smith.
- Exemples : dipôles λ/2 et en V, dipôle replié, cadre et boucle d'une longueur d'onde, petite boucle, hélice,
  Yagi 3 et 6 éléments, couple en rayonnement longitudinal (courants en quadrature), réseau transversal de
  4 dipôles.
- Optimiseur Yagi-Uda : longueurs et espacements, objectif gain, avant/arrière ou compromis, adaptation à Z₀ ;
  simplexe de Nelder-Mead borné, chaque essai est une résolution complète ; le résultat est une seule étape
  annulable. L'adaptation coûte 2 points de score par dB manquant sous |S11| = −10 dB, avec une marge de
  0,5 dB pendant la recherche sur maillage réduit : le résultat reste adapté au maillage de la scène.
  Yagi 6 éléments, objectif gain, 200 essais : 10,96 → 12,2 dBi et 13 → 29 dB d'avant/arrière, |S11| = −9,9 dB.
- Banc de validation (onglet Validation) : 14 contrôles contre des références indépendantes (Hallén, dipôle de
  Hertz, petite boucle, inductance d'une spire, bilan de puissance, réciprocité, convergence du maillage), avec
  pour chacun la source de la référence et le seuil. 14 conformes au maillage par défaut.
- Méthode des moments : forme à potentiels mixtes pour les fonctions triangulaires (voir les défauts) et
  intégrales de chaque paire de segments calculées une seule fois pour les quatre combinaisons de fonctions.
- Vitesse sous Windows : les tests riches en sinus et cosinus y tournaient 4 à 7 fois plus lentement que sous
  Linux (bibliothèque mathématique de MinGW), la FDTD aussi vite. `qsig::sinCos` (réduction de Cody-Waite et
  noyaux de fdlibm, écart < 5·10⁻¹⁶ avec la bibliothèque) sert dans le noyau MoM et le champ lointain ; le
  diagramme 3D est calculé sur plusieurs cœurs, avec 2 points de Gauss par segment court.

### Phase 3 (vues 3D, points de mesure A et B, concepteur d'antennes)

- Navigation 3D commune (`qsig/app/orbit_camera.h`, testée sans Qt, et `ui/orbit_navigator`) : glisser pour
  tourner, Maj / bouton du milieu / bouton droit pour faire glisser la vue, molette pour zoomer sous le curseur,
  double clic pour la vue initiale. Cube de vues en haut à droite : un clic sur une face donne la vue exacte de
  ce côté (Dessus = plan XY, Face = plan XZ, Droite = plan YZ). Comme dans la 6.14, quatre flèches autour du
  cube font tourner l'observateur par pas de 45° (gauche, droite, haut, bas) et le bouton « Cadrer » remet toute
  la scène dans la vue sans changer de côté. Les pas tombent sur la grille de 45°, qui contient les vues de
  face, de côté, d'arrière et de dessus : une vue libre rejoint d'abord son angle de grille le plus proche.
- Vue 3D des champs statiques (bouton 2D / 3D des modules Électrostatique et Magnétostatique) : sources en
  volume (sphères, pavés, plaques, cylindres, boucles, solénoïde en hélice, sens des courants), plan de coupe de
  la vue 2D avec sa carte, ses équipotentielles et ses lignes de champ, réseau de 6 × 6 × 6 vecteurs du champ
  (longueur et couleur en échelle logarithmique), points de mesure. Ce qui est derrière le plan de coupe est
  dessiné avant lui. Une source se glisse parallèlement au plan de coupe ; un clic pose le point A, un clic
  droit le point B.
- Couleurs des cartes selon le thème (`FieldMapComputer::mapColour`) : le bas de l'échelle de |E| ou |B| et le
  zéro de V prennent la teinte du fond, blanc en thème clair, noir en thème sombre. La carte se fond dans la
  vue au lieu d'y poser une feuille de l'autre teinte ; les lignes, flèches et grilles tracées dessus sont
  sombres sur fond clair et blanches sur fond sombre. Case « Couleurs » (2D et 3D) : sans elle, il reste les
  lignes de champ, les équipotentielles et les flèches sur le fond nu de la vue. Le module Ondes garde ses
  images sombres dans les deux thèmes.
- Carte pendant qu'on glisse une source (`FieldMapComputer`) : une demande faite pendant un calcul attend la
  fin de celui-ci, dont la carte est affichée, au lieu de l'annuler. Avant, chaque mouvement de souris annulait
  le calcul en cours : dès que la souris allait plus vite que les calculs (lignes de champ, couleurs et
  équipotentielles masquées, donc affichage léger), plus rien ne s'affichait avant l'arrêt du geste. Un calcul
  n'est abandonné que s'il ne sert plus (autre plan, autre grandeur, autre thème, autre scène) ou si la
  nouvelle demande est bien plus légère (grille grossière d'un geste qui commence pendant une carte fine).
- Point B en statique (interrupteur de l'inspecteur, clic droit dans la vue) :
  - électrostatique : champ en B, tension U_AB = V_A − V_B, distance, champ moyen U_AB / d, rapport des champs,
    profils de V et de |E| de A à B ;
  - magnétostatique : champ en B, écart relatif de |B| (uniformité), angle entre les deux champs, circulation
    ∫H·dl le long de AB (théorème d'Ampère), profil de |B|.
- Diagramme de rayonnement 3D : rayon au choix (champ, puissance, dB), faces opaques ombrées, tracées de
  l'arrière vers l'avant, pôles fermés ; les couleurs donnent toujours le gain en dB.
- Concepteur d'antennes, première étape (vue 3D du module Antennes) :
  - vues planes par le cube ou les boutons Dessus / Face / Droite ; plan de construction XY / XZ / YZ réglable
    en position, matérialisé par la grille ;
  - outils : Sélection (glisser un élément le déplace parallèlement au plan, pas d'un dixième de grille, Maj :
    libre), Fil (glisser d'un bout à l'autre), Source (clic sur un élément ; sur un fil tracé, à l'endroit du
    clic), Mesurer, Supprimer, Couper (un fil tracé, à l'endroit du clic) ; raccourcis V F S M X C, Échap ;
    Alt + glisser tourne la vue quel que soit l'outil.
- Concepteur d'antennes, deuxième étape :
  - raccords : le solveur relie les fils qui se touchent, bout à bout, bout sur le corps d'un autre fil
    (jonction en T) ou croisement, sans qu'il faille couper le fil touché. Un fil tracé ou un bout glissé
    s'accroche donc au bout d'un fil (10 pixels) ou à son corps (8 pixels). Il ne s'accroche plus aux
    sources : le solveur refuse une source placée sur un nœud à plus de deux fils ;
  - sélection de plusieurs éléments (Ctrl + clic, Ctrl + A), glissés ensemble en une étape annulable ;
  - poignées carrées aux bouts du fil tracé sélectionné : en glisser une redimensionne et réoriente le fil,
    l'autre bout reste en place, la source garde sa position relative ;
  - panneau Transformer (sous le cube), une étape annulable par action : déplacement de Δx, Δy, Δz ou réseau
    de N copies espacées de Δ ; rotation autour de X, Y, Z ou de la normale au plan de construction, par le
    centre de la sélection ou l'origine, sur place ou en copie ; miroir dans le plan de construction, sur
    place ou en copie ; alignement des centres sur l'élément principal ; suppression de la sélection ;
  - `rotateElement` et `mirrorElement` (sans Qt) recalculent azimut, élévation et roulis : chaque point du fil
    et la source vont à leur image. Exception : le miroir d'une hélice garde son sens d'enroulement (le modèle
    n'a que des hélices à droite) ;
- Concepteur d'antennes, troisième étape : groupes et contraintes (panneau sous le cube, onglets Transformer,
  Groupe, Contraintes) :
  - groupes nommés : « Grouper » réunit la sélection ; un clic sur un élément d'un groupe sélectionne tout le
    groupe, qui se glisse, tourne et se copie d'un bloc ; le nom se modifie, « Dégrouper » dissout le groupe ;
  - contraintes : l'élément qui porte la contrainte est celui qui bouge, tourne ou change de longueur pour la
    respecter ; l'élément de référence n'est pas touché. Huit sortes : position fixée (axes au choix), longueur
    imposée, même longueur, parallèle, perpendiculaire, angle entre les axes, points confondus (centre ou bouts),
    distance entre deux points. Pour en poser une : cliquer la référence, Ctrl + clic sur l'élément à
    contraindre, choisir la sorte, « Ajouter » ;
  - elles sont appliquées de nouveau à chaque modification de la scène (`SourceListScene::settleState`,
    `applyConstraints`, sans Qt) : un fil accroché au bout d'un autre le suit quand on déplace, tourne ou
    redimensionne celui-ci. Une contrainte se suspend (case) ou se retire ; deux contraintes qui se
    contredisent restent listées et celle qui cède est signalée « non respectée » ;
  - chaque élément reçoit un identifiant qui le suit dans la liste ; identifiants, groupes et contraintes sont
    enregistrés dans le fichier. Une copie n'hérite ni du groupe ni des contraintes de son original ; quand
    la référence est supprimée, les contraintes qui en dépendaient le sont aussi ;
  - différence avec la 6.14 : ses contraintes déplaçaient les deux éléments à parts égales, sauf verrou ; ici
    le sens est explicite (celui qui suit, celui qui sert de référence), ce qui rend le résultat prévisible.
    Les contraintes de la 6.14 qui portent sur des surfaces viendront avec la phase 3b.

### Phase 2 (ondes FDTD 2D)

- Module Ondes : simulation FDTD 2D en temps réel, calculée en arrière-plan et seulement quand le module est à
  l'écran. Lancer, pause (Espace), pas à pas, réinitialiser, vitesse de 1 à 64 pas par image.
- Objets : source ponctuelle, source linéaire (front plan), blocs et disques diélectriques (εr, μr, σ),
  bloc et cylindre conducteurs, écran à fentes. Signal sinusoïdal, impulsion gaussienne ou ondelette de Ricker,
  injection douce ou forcée. Les objets se déplacent pendant la simulation.
- Réglages (annulables, enregistrés avec la scène) : taille de la zone, cellules par longueur d'onde,
  polarisation TMz ou TEz, bord absorbant CPML, Mur ou conducteur. Le maillage suit la plus haute fréquence
  significative des sources, dans une limite de 480 000 cellules ; la couche absorbante est ajoutée autour de
  la zone.
- Vues : champ instantané (échelle noire en zéro) et amplitude (√2 × valeur efficace glissante). Point de
  mesure avec la trace temporelle du champ.
- Exemples : source ponctuelle, interférences, fentes d'Young, dioptre incliné, lentille boule, guide à plaques,
  impulsion diffractée, cavité résonante sur son mode TM₃₂.
- Dessin dans le plan (palette d'outils, raccourcis V, P, G, R, D, T, B, Échap) : pinceau et gomme pour des zones
  diélectriques ou conductrices à main levée (clic droit : gommer), rectangle, disque et trait tracés à la souris.
  Les zones dessinées sont des objets comme les autres : annuler, déplacer d'un bloc, enregistrer.
- Point de mesure B : valeurs en B, A − B, rapport des amplitudes en dB, déphasage et retard de B sur A à la
  fréquence de la source sinusoïdale ; traces de A et B superposées (ou A − B).
- Couche absorbante CPML corrigée : −95 dB de réflexion au lieu de −38 dB (voir les défauts plus haut).
- Amplitude et phases moyennées en deux étages : ondulation < 1 % sur la carte d'amplitude.
- Solveur FDTD : coefficients précalculés et convolution CPML limitée aux couches absorbantes, environ deux fois
  plus rapide, résultats identiques au bit près.

### Phase 2 (magnétostatique)

- Module Magnétostatique : fils infini et fini, spires circulaire, rectangulaire et triangulaire, solénoïde,
  bobines de Helmholtz, dipôle, conducteurs cylindriques plein et creux (densité de courant J), nappe de courant.
  Carte |B| ou B⊥ (composante vers l'observateur), point de mesure B et H, exemples choisissant leur plan de coupe.
- Dessin des courants : ⊙ et ⊗ là où un courant traverse le plan de coupe, flèches de sens sur les spires et les
  fils.
- Lignes de champ régulièrement espacées dans les deux modules (Jobard et Lefer), fermées autour des courants,
  avec le sens du champ.
- Les deux modules partagent la même base (scène, annuler/rétablir, fichiers, vue, panneaux) : un module
  ne décrit que ses sources, ses formules, son dessin et son champ.
- Panneaux Scène et Propriétés repliables (boutons de la barre, F9 et F10, onglets au bord de la vue) ; leur
  état, leur largeur et le thème sont mémorisés.

- Boucle circulaire et paire de Helmholtz : formule exacte par intégrales elliptiques complètes (avant : polygones
  de 220 à 320 segments). Le module complémentaire est calculé exactement, d'où la précision jusqu'au fil.
- Solénoïde : nappe de courant exacte (Derby et Olbert, 2010) au-delà de 24 spires ; spire par spire, avec des
  boucles exactes, jusqu'à 24 spires (avant : 28 « spires virtuelles » de 88 segments, quel que soit N).
- Coût par point : 0,18 µs pour une boucle, 0,28 µs pour un solénoïde (contre plusieurs dizaines de µs).

### Phase 1 (pilote Électrostatique)

- Interface Qt Quick : barre des modules, scène, vue de champ (carte |E| ou V, équipotentielles, flèches, grille,
  coupe XY/XZ/YZ à position réglable), propriétés, point de mesure, thème clair ou sombre.
- Glisser une source se cale sur la grille (Maj : libre) ; une source hors du plan de coupe est dessinée en
  pointillés avec sa distance.
- Saisie avec unités et préfixes (« 2 nC », « −3,5 µC/m² », « 10 cm »), virgule française, contrôle de la dimension.
- Charge donnée par une densité (ρ, σ ou λ) : Q = densité × volume, surface ou longueur de la forme ; au choix,
  la densité ou la charge totale reste constante quand les dimensions changent.
- Annuler/rétablir sur toute modification ; un glisser est une seule étape. Scènes enregistrées en JSON
  (`.qsig-es.json`).
- Plaques rectangulaire et triangulaire, disque, anneau et pavé : formules exactes (avant : nuages de charges
  ponctuelles).
- Fil, cylindres et plan infinis : potentiel relatif (V = 0 à 1 m de l'axe, ou sur le plan) au lieu d'un
  potentiel indéfini qui masquait toute la carte V.
- Carte de champ calculée en arrière-plan, plus grossière pendant un glisser ; échelle de couleur adaptée à la vue,
  avec un avertissement quand la variation est faible.

### Phase 0 (socle)

- Constantes CODATA 2022 partout (avant : trois valeurs de μ0 et trois de ε0). Effet sur les résultats : environ 10⁻⁹.
- L'onglet MoM Pocklington refuse désormais un résultat non convergé : il recalcule à 2N+1 segments et rejette
  au-delà de 5 % d'écart. Les cas absurdes de l'audit (j605 687 Ω) sont rejetés.
- FDTD : plus de copie complète du champ à chaque pas, sauf avec la condition de Mur qui en a besoin.
- Virgule flottante reproductible (`-ffp-contract=off`) : sans cette option, GCC sur ARM64 fusionne les
  multiplications-additions (FMA). Cette seule différence d'arrondi faisait basculer le cas « patch » du banc de
  validation vers une impédance non physique (−0,43 − j8,3 Ω au lieu de 9,51 − j42,4 Ω). Le chemin de calcul du patch
  prend donc une décision discrète fragile : à reprendre en phase 3. Un test vérifie désormais que cette impédance
  reste passive (Re Z > 0).
- Solveur dense unique : solutions identiques au bit près, vitesse inchangée. Le gain de vitesse viendra
  d'un passage à Eigen ou LAPACK derrière la même interface.
- `em_engineering_model` : `QString` remplacé par `std::string`.
- Mise en forme : plus aucune ligne de plus de 123 caractères (contre 3 795 au maximum).

## Mettre à jour la référence 6.14

Uniquement si `tests/fixtures/scenarios.h` change :

```
cmake --preset release -DQSIG_LEGACY_DIR="../QTsignalApp"
cmake --build --preset release --target legacy_baseline
build/release/tools/baseline/legacy_baseline > tests/fixtures/legacy_baseline.inc
```

## Prochaines étapes

1. Conception par formes, milieux et sources, suite du plan validé : seconde partie du lot 4, le métal posé
   sur un diélectrique (patch sur substrat fini, sonde à travers le substrat), contre des patchs publiés ;
   suite du lot 5 : polygone, ellipse, charges, conducteurs réels.
   Restent aussi : cas de surfaces au banc de validation du module (réflecteur en coin contre ses images),
   contraintes du concepteur portant sur des surfaces, sources nommées par l'utilisateur.
2. Lecture au survol pour les courbes des autres modules (profils, traces temporelles), si elle manque à l'usage.
3. Phase 4, Outils (une fois les antennes validées) : reprise des outils de la 6.14 dans un module « Outils » —
   calculatrice scientifique avec variables, unités et constantes ; onde / fréquence ; dimensionnement
   d'antennes ; rail de Laplace ; notes ; équations et références ; onde plane et doublet de Hertz ; bilan de
   liaison ; inductances, capacités, bobines couplées et transformateurs ; antennes inductives ; lignes de
   transmission ; concepteur de filtres ; chaîne RF et adaptation avec abaque de Smith ; BEM électrostatique 2D.
