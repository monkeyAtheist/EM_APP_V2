# QTsignalApp 7 — refonte

Outil pédagogique et utilitaire de développement : électrostatique, magnétostatique, ondes (FDTD 2D) et antennes.
Ce dépôt est la réécriture de QTsignalApp 6.14, menée selon l'audit du 15/11/2026.

**Statut : phase 0 (socle) terminée.** Le cœur de calcul est extrait sans Qt, unifié et couvert de tests.
L'interface arrive en phase 1 (pilote Électrostatique). L'ancienne application reste l'outil utilisable d'ici là.

## Structure

```
physics/                    qsig_physics : cœur de calcul C++20, sans aucune dépendance Qt
  include/qsig/constants.h  constantes physiques CODATA 2022 (seule définition du projet)
  include/qsig/vec3.h       le seul type vecteur 3D
  include/qsig/linalg.h     le seul solveur linéaire dense (LU à pivot partiel)
  include/qsig/electrostatics.h, magnetostatics.h   noyaux de champ portés de la 6.14, sans Qt
  include/qsig/...          solveurs repris de la 6.14 : FDTD 2D, MoM fil mince et RWG, BEM 2D,
                            couplage de boucles, banc de validation, calculatrices, modèles RF
tests/                      qsig_tests : tests de référence et de non-régression
  fixtures/scenarios.h      scénarios rejoués à l'identique sur l'ancien et le nouveau code
  fixtures/legacy_baseline.inc   valeurs enregistrées depuis le code 6.14
tools/baseline/             enregistreur de la référence 6.14 (compile l'ancien code avec un faux Qt minimal)
tools/reference/            scripts Python indépendants de l'audit (Hallén, plaque, solénoïde)
cmake/                      options de compilation communes
```

## Compiler et tester

Sous Windows, avec le compilateur MinGW de Qt 6.11 (préréglages de `CMakeUserPresets.json`, propres à ce PC) :

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
| `constants`, `core`, `linalg` | Valeurs CODATA 2022, cohérence c²μ0ε0 = 1, alias, Vec3, solveur dense |
| `electrostatics` | Coulomb, sphères, coquille, ligne et plan infinis, théorème de Gauss pour toutes les sources finies, champ lointain, superposition |
| `magnetostatics` | Fil infini, théorème d'Ampère, boucle comparée à la solution exacte par intégrales elliptiques, Helmholtz, dipôle, solénoïde |
| `fdtd` | Limite de Courant, absorption CPML, conservation de l'énergie en cavité PEC, vitesse de propagation |
| `mom` | Capacité d'une ligne bifilaire, mutuelle inductance de boucles coaxiales, dipôle comparé à Hallén, garde de convergence Pocklington |
| `validation` | Banc de validation des antennes, passivité du cas patch |
| `parity` | Le nouveau cœur reproduit la 6.14 à 10⁻⁷ près sur 15 sources électrostatiques, 11 magnétostatiques, 4 cas FDTD, 3 réseaux de fils, BEM, boucles, banc de validation (dipôle, boucle, monopôle) et calculatrice |

### Convention `[defect]`

Un test marqué `[defect]` affirme le **bon** comportement physique et échoue aujourd'hui, à cause d'un défaut mesuré par
l'audit. Le harnais le compte comme « défaut confirmé ». S'il se met à passer, il est signalé en échec
(« FIXED? ») pour qu'on retire l'étiquette : une correction ne passe jamais inaperçue.

`qsig_tests --verbose "[defect]"` affiche les écarts mesurés :

| Défaut | Mesure actuelle | Corrigé en |
| --- | --- | --- |
| Plaque rectangulaire chargée, champ sur l'axe | +10 % à 2 cm, +106 % à 1 cm, +578 % à 5 mm | phase 1 |
| Disque chargé, champ sur l'axe | −48 % à 2 cm, −86 % à 5 mm | phase 1 |
| Solénoïde, à 2 cm de l'enroulement | −17 % et +24 % selon z | phase 2 |
| Dipôle λ/2 (réseau de fils) | 82,4 + j23,5 Ω, contre R ≈ 88 Ω et X ≈ 50 ± 15 Ω (Hallén) | phase 3, à confirmer avec NEC-2 |
| Banc de validation | dipôle déclaré PASS avec 23,5 % d'écart d'impédance | phase 3 |

## Changements par rapport à la 6.14

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

## Prochaine étape : phase 1, pilote Électrostatique

Coque d'interface (Qt Quick recommandé), modèle de scène en QtCore avec annulation et sauvegarde, formes exactes
pour la plaque, le disque et le pavé, carte de champ calculée sur GPU en temps réel. Porte de sortie : carte fluide
pendant le glisser d'une charge et erreur inférieure à 1 % près d'une plaque.
