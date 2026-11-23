# Roadmap — Cámara (CAMSS SDM660) — lavender

> Estado al **2026-11-23**. Agente cámara: serie de parches **`1000`–`1100`**
> (los parches arrancan en `1001`; el `1000` se deja de frontera con el rango
> `0900`–`1000` de userland), driver `drivers/camera/lavender_cam.c`, boot-args
> `lvcam=N`, `lvcamdelay=S`, `lvcamsel=0/1/2`, rama `cam-1001` (worktree
> `work/wt-cam`). Este archivo es el tablero de avance:
> ~~tachado = verificado en el teléfono~~, `⬜ PENDIENTE` = falta.
> Convención: un archivo por función clave (`WIFI-ROADMAP.md`, `AUDIO-ROADMAP.md`, …).

## Objetivo final

Cámara usable en iOS 16.7.16: sensor → CSIPHY → CSID → (IPIF) → VFE → frame
en memoria, expuesto al userland por un nodo de dispositivo y un cliente,
empezando por la trasera principal (`camera@0`).

**Punto de partida:** no había trabajo previo de cámara en el árbol
(`WHAT-WORKS.md`: «Camera stack … not implemented»). La DTB del teléfono trae el
CAMSS completo de SDM660 y tres sensores; el DTBO del dispositivo es un stub
(487 bytes, una entrada de id 0), así que la DTB base es la única verdad.

## El hardware (de `resources/lavender_dtb.bin`)

| Bloque | MMIO | Notas |
|---|---|---|
| MMCC (todos los relojes de cámara) | `0xc8c0000` | GDSC `camss_top` `+0x34a0`, `camss_vfe0/1` `+0x3664/0x3674`; `bimc_smmu` `+0xe020` **sin escribir** |
| CSIPHY0/1/2 (v3.5) | `0xc824000` + `0x1000·n` | mux de reloj en `0xca00120` + `4·n` |
| CCI | `0xca0c000` | el I2C de los sensores |
| CSID0..3 (v5.0) | `0xca30000` + `0x400·n` | dentro del dominio de energía del VFE |
| IPIF (v3.0) | `0xca31000` | mux CSI PIX en `0xca00020` |
| VFE0/1 (vfe48) | `0xca10000` / `0xca14000` | únicos que escriben frames a memoria |
| MMSS SMMU | `0xcd00000` | por aquí escribe el VFE |
| Sensores | `camera@0/1/2` | trasera principal (CSIPHY0/CSID0, CCI master 0, MCLK0 GPIO 32, RESET0 GPIO 48); segunda trasera (1/1, master 1, MCLK3 GPIO 35, RESET3 GPIO 52, VAF GPIO 50); frontal (2/2, master 1, MCLK1 GPIO 33, RESET1 GPIO 47, STBY1 GPIO 45). Pines CCI: GPIO 36–39. MCLK 24 MHz |

La DTB nombra los módulos que MIUI sondea por slot (OV02A10 y S5K5E8, variantes
ofilm y sunny); el que trae **esta** unidad se decide leyendo su chip ID en F1.

## Panorama

| Fase | Alcance | Estado |
|---|---|---|
| F0 | Sonda de solo lectura (`lvcam=1`, parche 1001): dominios, ramas de reloj, MMPLL10 y RCG de MMCC, pines de cámara en TLMM, identidad del SMMU | ⬜ PENDIENTE |
| F0b | Encendido de `camss_top` y las ramas AHB, muxes de reloj CSI e interfaces de registro del CSIPHY, apagado de lo encendido (`lvcam=2`, parche 1001) | ⬜ PENDIENTE |
| F1 | CCI: reloj, reset, timings del DTB y transacciones; rails, MCLK y secuencia de encendido del sensor; **chip ID** (`~1010`) | ⬜ PENDIENTE |
| F2 | Patrón de test del CSID → VFE (RDI) → frame en memoria, sin sensor (`~1020`) | ⬜ PENDIENTE |
| F3 | Stream real del sensor → CSIPHY → frame RAW (`~1030`) | ⬜ PENDIENTE |
| F4 | IPIF + VFE en el camino, formato útil (`~1040`) | ⬜ PENDIENTE |
| F5 | iOS: nodo de dispositivo + cliente que entrega el frame (`~1050`) | ⬜ PENDIENTE |
| F6+ | EEPROM/calibración, actuator (VAF), flash, los otros dos sensores (`1060+`) | ⬜ PENDIENTE |

## Cómo se prueba

- Todo shim de este agente lleva `sweetlog=1 sweetusb=2 lvcam=N` (más
  `lvcamdelay=S` o `lvcamsel=N` cuando haga falta). Se flashea en `recovery` y
  se arranca con Power+VolUp.
- Log en vivo, desde una PowerShell elevada, siempre a la misma ruta (el script
  la trunca en cada corrida):

  ```powershell
  powershell -ExecutionPolicy Bypass -File F:\port_ios\repositorio\tools\usbnet-windows.ps1 -Log F:\port_ios\work\wlan\live.txt
  ```

  Copia de cada boot: `F:\port_ios\work\cam\cam-10NN-lvcamN-<fecha>.txt`.
- El shim es el nuestro cuando la línea `boot-args = …` trae `lvcam=` y aparecen
  líneas `SWEET CAM:`. La sonda arranca 25 s después del boot (`lvcamdelay`) y
  anuncia cada acceso 2 s antes de hacerlo; termina con
  `SWEET CAM: probe done`.
- Si el archivo se congela, relanzar el script antes de diagnosticar (relee el
  anillo del kernel desde el principio); leer siempre la última corrida.

## F0 — sonda de solo lectura — ⬜ PENDIENTE

`lvcam=1` no escribe nada. Responde: ¿el bootloader dejó `camss_top` (o los
GDSC del VFE) encendidos? ¿qué ramas de reloj están habilitadas y está lockeado
MMPLL10 (la fuente de los 24 MHz de MCLK)? ¿en qué estado quedaron los pines de
cámara? ¿está arriba el reloj AHB del SMMU? Solo si el dominio **y** su reloj
AHB ya están encendidos lee los registros del CAMSS.

`lvcam=2` añade, si el dominio está apagado: limpiar `SW_COLLAPSE` de
`camss_top`, esperar `PWR_ON`, habilitar `camss_top_ahb`, `camss_ahb`,
`ispif_ahb` y `csiphy_ahb2crif`, leer los muxes de reloj CSI y los controles
del CSIPHY, y volver a dejar todo como estaba. Si el dominio no encende, pone el
bit de colapso de vuelta y no toca los bloques.

## F1 y siguientes

Detalle de registros, orden de escritura y referencias (`msm_cci.c` del kernel
de Xiaomi, mainline `camss-*`): el brief del agente
(`work/briefs/camera-sdm660-brief.md`) y el handoff
(`research/cam-refs/HANDOFF-CAM-F0F1.md`).

## Coordinación entre agentes

Cada agente trabaja su función por su cuenta; lo compartido es el repo.

- Rangos ya asignados: WLAN/modem `0700–0799`, GPU `0300+`, audio `0500+`,
  userland/crash `0900–1000`, **cámara `1000–1100`** con boot-args `lvcam*`.
- El agente cámara trabaja en `work/wt-cam` (rama `cam-1001` desde
  `origin/main`) y nunca en el checkout `repositorio/`, que puede estar en la
  rama de otro agente con parches ajenos sin trackear.
- Commits con paths explícitos; nunca `git add -A`. No se pushea a `main`
  (cancela la CI de los demás): la rama propia se compila con
  `gh workflow run xnu.yml --ref cam-1001`.

## Trampas conocidas (no repetir)

1. **La CI descarta un parche rechazado con un warning y sigue.** Aplica cada
   parche con `git apply` estricto (sin fuzz ni 3-way); si el nuestro no
   aplica, el kernel se compila sin el driver y parece un bug del teléfono.
   Tras cada corrida: `grep "Skipping patch"` en el log (solo deben aparecer
   los 4 parches de blacktop) y buscar `Applying patch: …/10NN-…`.
2. **`bsd/conf/files` no está en el repo** (solo recibe hunks) y la serie
   mezcla dos formatos de diff (algunos parches arrancan en `--- a/…` sin
   `diff --git`). Los hunks se generan contra el árbol real: base
   `xnu-8796.141.3` + la serie de `main` aplicada parche por parche
   (`work/make_1001.py`). Y ojo: `git apply` ejecutado desde un subdirectorio
   del repo **ignora en silencio** todo lo que esté fuera de él.
3. **El contexto de un hunk solo debe contener líneas de parches de número
   menor.** `lavender_cam.c` va después de `sweet_cpufreq.c` en `conf/files`
   (líneas de 0094) y el enganche de `santoni_sdhci.c` después de
   `santoni_fb_test()`, no pegado a los insertos de audio/WLAN/GPU.
4. **Un chequeo de sintaxis vacío engaña.** El driver entero está detrás de
   `#if defined(ARM64_BOARD_CONFIG_LAVENDER)`: sin `-D` el compilador solo
   ve el stub. `work/cam/syntax-check.sh` (clang aarch64 del Docker del
   proyecto, `-Wall -Wextra -Werror -Wshadow`, formatos de `printf`
   verificados) lo define y fue probado con errores inyectados.
5. **CCI, CSID y VFE tienen sus propios relojes, y el CSID vive en el dominio
   de energía del VFE** (mainline enciende el VFE antes de leer el CSID). Sus
   registros no se leen hasta levantar eso; con los relojes o el dominio
   apagados el acceso puede colgar el bus.
6. **El GDSC `bimc_smmu` de MMCC (`0xe020`) no se escribe**: mainline lo
   tiene comentado por colgar el subsistema multimedia. Sus ramas
   (`0xe004`/`0xe008`) son `BRANCH_VOTED`: solo se leen.
7. **MMCC está en `0xc8c0000`** (no en `0xc8a0000`) y ahí están todos los
   relojes de cámara; GCC no tiene ninguno.
8. **Valores verificados en fuente** (aunque otros SoC usen otros): GDSCR
   `PWR_ON` bit 31 / `SW_COLLAPSE` bit 0; CBCR `CLOCK_ENABLE` bit 0 /
   `CLK_OFF` bit 31 (y en modo de clock-gating por hardware Linux no mira
   `CLK_OFF`); RCG2 `CMD` bit 0 `UPDATE`, bit 1 `ROOT_EN`, `CFG` en `+4` con
   `SRC_SEL` en bits 10:8; `STATUS` de una alfa-PLL DEFAULT en `+0x24`; TLMM
   de SDM660: mux bits 4:2, pull 1:0, fuerza 8:6, OE bit 9, IO en `+4`
   (in bit 0, out bit 1); en el SMMU `sCR0` es `+0x0` e `IDR0/IDR1` `+0x20/0x24`.
9. **Los rails de los sensores son LDOs del PM660/PM660L por SPMI**: reutilizar
   el camino que verificó el driver de audio, solo con los voltajes de la DTB.
10. **La dirección y el valor del chip ID no están en la DTB**: MIUI los trae
    en los `.bin` de camx (`/vendor/lib64/camera/com.qti.sensormodule.*`). F1
    los sondea o usa los registros estándar de cada sensor.
11. **Memoria Device**: `ml_io_map_unmappable` y copias byte a byte (los
    `STNP` de `memcpy`/`memset` no son seguros), como en el resto de drivers.
12. **El log USB arranca a los ~20–30 s** y el anillo se relee al relanzar el
    script; un archivo congelado no es un cuelgue del kernel.
