# Part parameter coverage (jlcparts)

jlcparts cache.sqlite3, parts fetched up to 2026-11-11, in stock. *Given*: the attribute is there; *usable*: it reads as a number in the model's unit (`ci/part_params.py`). Made by `ci/param_coverage.py`.

## mosfet: 19528 in stock, 38 basic / preferred

| parameter | id | given | usable | usable (basic / preferred) | unreadable, e.g. |
|---|---|---|---|---|---|
| Vds (V) | `vds` | 100% | 100% | 100% |  |
| Id (A) | `id` | 100% | 100% | 100% |  |
| Rds(on) (Ω) | `rds_on` | 99% | 99% | 100% |  |
| Vgs(th) (V) | `vgs_th` | 99% | 99% | 100% |  |
| Qg (C) | `qg` | 94% | 94% | 84% |  |
| Ciss (F) | `ciss` | 96% | 96% | 100% |  |
| Coss (F) | `coss` | 86% | 86% | 82% |  |
| Crss (F) | `crss` | 93% | 93% | 100% |  |

## ldo: 7254 in stock, 9 basic / preferred

| parameter | id | given | usable | usable (basic / preferred) | unreadable, e.g. |
|---|---|---|---|---|---|
| Vin max (V) | `vin_max` | 99% | 99% | 100% |  |
| Vout (V) | `vout` | 98% | 98% | 100% |  |
| Iout (A) | `iout` | 99% | 99% | 100% |  |
| Dropout (V) | `dropout` | 88% | 88% | 89% |  |
| Iq (A) | `iq` | 89% | 89% | 89% |  |
| PSRR (dB) | `psrr` | 76% | 76% | 67% |  |
| Noise (Vrms) | `noise` | 50% | 45% | 33% | `0.003%Vout`, `0.01%Vout` |

## opamp: 5341 in stock, 8 basic / preferred

| parameter | id | given | usable | usable (basic / preferred) | unreadable, e.g. |
|---|---|---|---|---|---|
| GBW (Hz) | `gbw` | 92% | 92% | 75% |  |
| Slew (V/us) | `slew` | 95% | 95% | 75% |  |
| Vos (V) | `vos` | 99% | 99% | 100% |  |
| Ib (A) | `ib` | 99% | 99% | 100% |  |
| CMRR (dB) | `cmrr` | 97% | 97% | 100% |  |
| Iout (A) | `iout` | 88% | 88% | 88% |  |
| Iq (A) | `iq` | 95% | 95% | 88% |  |
| en (V/√Hz) | `en` | 88% | 88% | 75% |  |
| Supply min (V) | `supply_min` | 82% | 82% | 75% |  |
| Supply max (V) | `supply_max` | 82% | 82% | 75% |  |
| RR in | `rr_in` | 66% | 66% | 25% |  |
| RR out | `rr_out` | 66% | 66% | 25% |  |

## inductor: 25564 in stock, 13 basic / preferred

| parameter | id | given | usable | usable (basic / preferred) | unreadable, e.g. |
|---|---|---|---|---|---|
| L (H) | `l` | 99% | 99% | 100% |  |
| DCR (Ω) | `dcr` | 93% | 93% | 100% |  |
| Irated (A) | `i_rated` | 93% | 93% | 100% |  |
| Isat (A) | `i_sat` | 58% | 58% | 0% |  |
| SRF (Hz) | `srf` | 23% | 23% | 100% |  |

## zener: 7274 in stock, 237 basic / preferred

| parameter | id | given | usable | usable (basic / preferred) | unreadable, e.g. |
|---|---|---|---|---|---|
| Vz (V) | `vz` | 97% | 97% | 100% |  |
| Zzt (Ω) | `zzt` | 96% | 96% | 98% |  |
| Pd (W) | `pd` | 100% | 100% | 100% |  |
| Ir (A) | `ir` | 100% | 100% | 100% |  |

## diode: Schottky: 5899 in stock, 159 basic / preferred

| parameter | id | given | usable | usable (basic / preferred) | unreadable, e.g. |
|---|---|---|---|---|---|
| Vr (V) | `vr` | 99% | 99% | 100% |  |
| If (A) | `if` | 99% | 99% | 100% |  |
| Vf (V) | `vf` | 99% | 99% | 100% |  |
| Ir (A) | `ir` | 94% | 94% | 99% |  |
| trr (s) | `trr` | 0% | 0% | 0% |  |

## diode: rectifier / fast recovery: 3886 in stock, 81 basic / preferred

| parameter | id | given | usable | usable (basic / preferred) | unreadable, e.g. |
|---|---|---|---|---|---|
| Vr (V) | `vr` | 99% | 99% | 100% |  |
| If (A) | `if` | 99% | 99% | 100% |  |
| Vf (V) | `vf` | 99% | 99% | 100% |  |
| Ir (A) | `ir` | 97% | 97% | 100% |  |
| trr (s) | `trr` | 61% | 61% | 67% |  |

## diode: switching: 1405 in stock, 26 basic / preferred

| parameter | id | given | usable | usable (basic / preferred) | unreadable, e.g. |
|---|---|---|---|---|---|
| Vr (V) | `vr` | 100% | 100% | 100% |  |
| If (A) | `if` | 99% | 99% | 100% |  |
| Vf (V) | `vf` | 100% | 100% | 100% |  |
| Ir (A) | `ir` | 99% | 99% | 100% |  |
| trr (s) | `trr` | 96% | 96% | 100% |  |

## MLCC: 18954 in stock, 133 basic / preferred

| parameter | id | given | usable | usable (basic / preferred) | unreadable, e.g. |
|---|---|---|---|---|---|
| C (F) | `c` | 98% | 98% | 100% |  |
| Rated V (V) | `v` | 98% | 98% | 100% |  |
| Tolerance | `tol` | 84% | 84% | 90% |  |
| Dielectric | `diel` | 95% | 95% | 100% |  |
| ESR (Ω) | `esr` | 0% | 0% | 0% |  |
| DC bias (F) | `bias` | 0% | 0% | 0% |  |

