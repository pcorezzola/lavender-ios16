# iOS 16 en el Redmi Note 7 (lavender)

Port de investigación, no comercial, de **XNU 8796** (Darwin 22 / iOS 16) y del userland de **iPhone X (D22AP) iOS 16.7.16** al **Redmi Note 7** (`lavender`, Qualcomm SDM660 / Snapdragon 660).

![Pantalla de inicio de iOS 16 en el Redmi Note 7](docs/images/home-screen.jpg)

> **Estado: arranca hasta el lock screen y la pantalla de inicio de iOS 16.7.16 en ~2–3 minutos.** Es un bring-up experimental, no un teléfono de uso diario. En la pantalla de inicio se dibujan los íconos, el dock y las notificaciones, Ajustes abre y aparece la pantalla inicial de Siri; funcionan el touch (taps y swipes), los botones físicos, la red USB (CDC-NCM: ping, SSH y log del kernel en vivo desde Windows) y las lecturas rápidas de eMMC (ADMA2). No funcionan la GPU, Wi-Fi, Bluetooth, audio, cámara ni un gauge de batería real: el render lo hace la CPU por software. En este repositorio no se distribuyen binarios de Apple.

El port se apoya en [`aLpHa-Git-69/ios-lavender`](https://github.com/aLpHa-Git-69/ios-lavender) (rama `lavender`), derivado de [`dipa-studio/ios-sweet`](https://github.com/dipa-studio/ios-sweet). Acá está la bitácora del bring-up de lavender, los parches que hicieron confiable la escritura eMMC y el estado real del dispositivo.

## Avances

| Pantalla de inicio | Ajustes | Configuración de Siri |
| --- | --- | --- |
| <img src="docs/images/home-screen.jpg" width="230" alt="Pantalla de inicio de iOS 16 en el Redmi Note 7"> | <img src="docs/images/settings.jpg" width="230" alt="Ajustes en el Redmi Note 7"> | <img src="docs/images/siri-setup.jpg" width="230" alt="Pantalla de configuración de Siri en el Redmi Note 7"> |
| La grilla de SpringBoard: íconos, dock, badge de notificación y el botón de la Biblioteca de apps. Ahora se dibuja el fondo Bokeh, el dock y los widgets son translúcidos; el widget de Clima muestra su marcador de carga y el de Calendario sigue como un recuadro blanco. | Se dibuja y se recorre toda la lista de Ajustes. | Aparece la pantalla inicial de Siri. Sin micrófono ni parlante, Siri no puede responder. |

<img src="docs/images/boot-self-tests.jpg" width="230" alt="Consola de arranque con los self-tests de Skylight en el Redmi Note 7">

Debajo del logo de Apple y la barra de progreso, el window server **Skylight** del userland corre sus self-tests de blit por software: `sk_copy64_8` a `sk_copy64_64x`, `sk_copy32_*`, `sk_zero_16` a `sk_zero_128` y una prueba de `memcmp` imprimen `PASSED`. Es el camino por software que dibuja cada cuadro en este port.

## Qué anda

| Área | Estado | Notas |
| --- | --- | --- |
| Arranque XNU | **Anda** | `xnu-8796`, kernel `LAVENDER` Mach-O arm64 |
| CPU | **Anda** | 4× Cortex-A73 Gold con páginas de 16 KiB; los A53 Silver quedan aparcados |
| GICv3 / timers | **Anda** | GIC y timers virtual/físico |
| eMMC SDCC v5 | **Anda** | Enumeración, raíz HFSX, journal replay, lecturas y escrituras; el anillo de log se vuelca |
| Raíz HFSX en `userdata` | **Anda** | `rd=emmcroot0`, read-write; sin dual-boot con Android |
| Framebuffer / AppleCLCD | **Anda** | 1080×2340, stride 4320, composición por software con IOSurface |
| `launchd` / daemons | **Anda** | `launchd`, `backboardd`, `SpringBoard`, `securityd`, `keybagd`, `usermanagerd`, etc. |
| GUI de iOS | **Anda** | Logo de Apple, barra de progreso y lock screen de iOS 16 |
| Pantalla de inicio | **Anda** | Grilla de SpringBoard con íconos, dock, badge de notificación y el botón de la Biblioteca de apps (`docs/images/home-screen.jpg`); con el fondo Bokeh detrás; el widget de Clima sigue en su marcador de carga y el de Calendario en un recuadro blanco (ver la fila de widgets más abajo) |
| Ajustes | **Anda** | Toda la lista de Ajustes se dibuja y se recorre (`docs/images/settings.jpg`) |
| Configuración de Siri | **Solo UI** | Aparece la pantalla inicial de Siri (`docs/images/siri-setup.jpg`); sin micrófono ni parlante, Siri no puede responder |
| Self-tests de Skylight | **Pasan** | La consola de arranque imprime `PASSED` en los self-tests de blit por software del window server (`docs/images/boot-self-tests.jpg`) |
| Code signing de bring-up | **Anda** | `cs_enforcement_disable=1`, `unentitled_ios_sim_launch=1`, `launchd_unsecure_cache=1` |
| Log persistente | **Anda** | Anillo `SANTONILOG1` de 4 MiB en SYSTEM |
| 4 Gold cores | **Anda** | `cpus=4`; `SWEET CPU: requested=4 selected=4` |
| Orientación del panel | **Anda** | El lock screen y la pantalla de inicio se ven verticales y derechos, ocupando el panel 1080×2340 (`docs/images/lock-screen.jpg`, `docs/images/home-screen.jpg`) |
| Botones físicos | **Anda** | Parche `0201`: Power/Vol+/Vol− por el árbitro SPMI v3 de SDM660, llegan al HID de iOS |
| Red USB | **Anda** | Parches `0200`, `0210`–`0213`: DWC3 + QUSB2, CDC-NCM (UsbNcm de Windows, macOS, Linux), `ping 192.168.7.2`, log del kernel en vivo por UDP 5140 (`sweetusb=2`; `tools/usbnet-windows.ps1`). SSH root por el enlace (dropbear, `docs/SSH.md`) |
| Touch | **Anda** | Novatek NT36672A en QUP1 I2C (bit-bang): parches `0193`–`0208`, `0230`, `0231`, `0234`. Taps, íconos y el swipe hacia arriba para desbloquear funcionan; ~249 frames cada 10 s con 0 errores I2C (~14 ms por lectura). `0234` hace que SweetHID use el panel real de 1080×2340 (asumía 2400: desplazaba los toques y escondía el borde inferior). El kernel debe compilarlo el CI (Xcode 15): una compilación local con Xcode 16.2 leyó información de firmware basura y dejó el touch desactivado |
| Lecturas eMMC por ADMA2 | **Anda** | Parches `0232`, `0233`: el core SDCC v5 no tiene SDMA, solo ADMA2. Lecturas en frío ~465 KB/s → ~16 MB/s; boot al lock screen ~23 min → ~2–3 min. Las escrituras siguen por PIO |
| Wallpaper | **Anda** | El fondo elegido (Bokeh) se dibuja detrás de la pantalla de inicio (`docs/images/home-screen.jpg`); la galería abre, pero casi todas las vistas previas siguen en blanco (ver Wallpapers) |

## Qué falta

| Área | Estado | Notas |
| --- | --- | --- |
| GPU | **Anda (sin optimizar)** | Driver propio de kernel para el Adreno 512 más un plug-in de dispositivo Metal ([lavender-metal](https://github.com/pcorezzola/lavender-metal)); `backboardd` dibuja el lock screen, el Centro de control y el fondo con la GPU. Las animaciones y transiciones siguen lentas (dominan la copia por CPU al IOSurface y la compilación de shaders; la GPU no puede tocar memoria por encima de 4 GB). Tras reiniciar hay que encender el plug-in (`lavmetal_switch on`) |
| Cámara | **Falta** | Botón y stack de cámara sin implementar |
| Wi-Fi / internet | **Falta** | Sin driver ni firmware para la radio WCN3990, así que no hay internet; la única red es USB |
| Bluetooth | **Falta** | Sin transporte Bluetooth; `bluetoothd` respawnea en bucle sin hardware |
| Audio | **Ruta de datos verificada, sin sonido** | ADSP/GLINK/APR, AFE, ADM y ASM funcionan y el tono de 16 KiB se consume 12 veces (`WRITE_DONE`); falta el digital codec/SoundWire del LPASS que alimenta el puerto SWR DAC del PM660L. Las lecturas AP de `0x152c0000`/`0x152c1000` se cuelgan cuando el DSP ya arrancó el audio. Ver [Audio](#audio) |
| Brillo | **Falta** | Sin control de retroiluminación: el deslizador de brillo no hace nada |
| Vistas previas de la galería | **Parcial** | La galería abre y el fondo se ve, pero las vistas previas salen en blanco (un iPhone real muestra la imagen, `docs/images/wallpaper-editor.jpg`); Astronomía (Metal) aborta al arrancar. Ver Wallpapers |
| Batería / carga | **Parcial** | Fallback estático (100 %, 4200 mV); sin gauge |
| Widgets de inicio | **Parcial** | Los slots ya se dibujan: Clima muestra un marcador de carga y Calendario un recuadro blanco. La extensión del widget de Calendario gasta minutos de CPU en el layout de SwiftUI en los núcleos lentos y todavía no entrega contenido |
| Colores de pantalla | **A verificar** | En el lock screen y en la pantalla de inicio, fecha, hora y barra de estado salen rojas/anaranjadas: tinte del wallpaper o formato de color del framebuffer, sin revisar |
| RTC / fecha | **Parcial** | El parche `0220` lee el RTC del PM660. Su cuenta cruda no es hora Unix (Android guarda su propio offset): cada teléfono necesita `sweetrtcoff=` medido una vez en TWRP (`docs/BOOT.md`); el reloj de la pantalla de inicio todavía marca `mié 4 feb 05:28` en vez de la fecha real, así que falta fijar el offset de esta unidad |
| `preboot` / data volume | **Parcial** | `libignition` no encuentra el preboot; el boot sigue sin sellar |
| Segundo boot / tiempo de boot | **Anda** | El segundo boot corre sin migración y llega al lock screen en ~2–3 min desde la lectura ADMA2 (`0233`); con PIO eran ~18–23 min |

## Rendimiento

Todo lo que se ve en pantalla lo dibuja la CPU: QuartzCore cae a su renderizador OpenGL por software (`CA::OGL::SW`) porque no hay GPU. Medido en el teléfono (4× Cortex-A73, panel 1080×2340):

| Métrica | Valor | Cómo se midió |
| --- | --- | --- |
| Cuadros presentados | mediana **~13 fps** en un arranque, picos de **30–34 fps** al animar | líneas `SWEET FB: swap` del log en vivo, contadas por segundo (parche `0215`) |
| Refresco del panel | **30 Hz** | `sweetfb_hz=30`; el framebuffer copia como máximo un cuadro cada dos vsync de 60 Hz |
| CPU de `backboardd` | **~150–170 %** (de 400 %) en el lock screen quieto | tiempo de CPU acumulado de `ps` en ventanas de 60 s; muestreado con `spindump` |
| En qué se va el tiempo | `CA::OGL::SW::scan_convert`, `image_sampler`, `tex_color_matrix_4x4_bias`, `tex_multiply_color`, `fill_round_rect` | `spindump` de `backboardd`, simbolizado con el dyld shared cache de 20H392 |
| Lectura eMMC | **~16 MB/s** (antes ~465 KB/s) | ruta de lectura ADMA2, `0233` |
| Touch | **~25 frames/s** leídos del controlador, 0 errores I2C | línea de log `WHYRED TOUCH: 10s frames` |
| Boot al lock screen | **~2–3 min** (antes ~23 min) | marcas de tiempo del log en vivo |

Lo que se probó para bajar la carga del compositor y su efecto:

- Notificar el vsync a CoreAnimation al ritmo del panel en vez de 60 Hz fijos (`0235`): la CPU del lock screen quieto siguió en ~150–170 %; no hay ganancia medible en reposo, solo limita las animaciones a 30 fps.
- *Reducir transparencia* y *Reducir movimiento* en Ajustes: sin cambio en la CPU en reposo (~149 %).
- Congelar PosterBoard (el proceso del fondo): sin cambio, o sea que el fondo no es lo que mantiene ocupado al compositor. El lock screen quieto sigue presentando 7–16 cuadros por segundo; el proceso que los provoca todavía no está identificado.

Una solución real necesita una ruta de GPU (un driver de kernel para el Adreno 512 más una capa compatible con Metal, que no existe para iOS) o un camino de composición por software mucho más barato. Ninguno está empezado.

## Wallpapers

*Ajustes → Fondo de pantalla → Añadir fondo* mostraba la galería en blanco y abortaba PosterBoard. Hallazgos:

- `lsd` no podía cargar `/System/Library/xpc/extensions.plist` (falta AMFI), así que PlugInKit no veía extensiones. Se apartó el catálogo precompilado para que iOS escanee de forma nativa, se reconstruyeron una vez las bases de LaunchServices y se agregó la definición del punto de extensión `com.apple.posterkit.provider`. `GradientPosterExtension` y `CollectionsPoster` ahora se registran y arrancan.
- Después PosterBoard abortaba en `-[PBFHomeScreenPreviewImageBuilder _imageForView:]`: genera la vista previa con Metal (`MTLCreateSystemDefaultDevice()` → `newCommandQueue`) y metía una cola nula en un `NSDictionary`. Un reemplazo pequeño de ese método (`tools/posterboard-preview.m`) evita el crash.
- Resultado: la galería abre y **Color muestra su vista previa**. Falta verificar que se aplique en inicio y en el lock screen.
- **Astronomy** y otros fondos que enlazan Metal siguen fallando: `AegirPoster` y `CollectionsPoster` terminan con `extension check-in timeout` y el sistema se pone muy lento. Esto necesita un dispositivo Metal, o sea el mismo límite que la GPU.

**Estado al 2026-11-27.** Anda: USB (red, SSH, log en vivo), touch y gráfica (renderizado por GPU, sin optimizar). No anda: audio, internet (Wi-Fi), control de brillo, cámara y Bluetooth.

**2026-11-27 — fondos funcionando.** Tras reparar el sistema de archivos raíz (el almacén de posters tenía archivos ilegibles) y recuperar `AegirPoster`, la galería de fondos abre y la pantalla de inicio dibuja el fondo. Primera foto: pantalla de inicio; segunda: *Ajustes → General → Información* (iPhone X, iOS 16.7.16); tercera: el editor de fondos junto a un iPhone real, donde las vistas previas siguen en blanco.

| Pantalla de inicio | Información | Editor de fondos (izquierda: este port, derecha: un iPhone real) |
| --- | --- | --- |
| <img src="docs/images/home-screen.jpg" width="230" alt="Pantalla de inicio con el fondo Bokeh"> | <img src="docs/images/settings-about.jpg" width="230" alt="Ajustes, General, Información"> | <img src="docs/images/wallpaper-editor.jpg" width="300" alt="Editor de fondos en el port y en un iPhone real"> |

## Hitos

- **2026-11-27 — Wallpaper animado con transparencias, pipeline Metal de PhotosPicker y LAVTSENS (`0385`).** Con GPU batch/perf (`0365`–`0370`), telemetría térmica TSENS (`0371`, `lavtsens=30`), `LavenderMetal` (`newDefaultLibraryWithBundle:error:` + fix en Mesa `libair.a` para `memcpy` de `array<texture2d/3d>` a slots) y sandbox XNU `op 4`/`5`/`6` (`sandbox_container_path_for_pid` → `"/var/tmp"` + extensiones TCC Photos), la pantalla renderiza wallpaper animado (`CollectionsPoster`) y transparencias del sistema en la GPU Adreno 512 (`shim-gpu-lavender-0385-picker-tsens.img`).
- **2026-11-26 — Ruta de datos de audio verificada, sin sonido.** El ADSP arranca, GLINK/APR responde, el puerto AFE `INT4_MI2S_RX` inicia, ADM rutea y ASM reproduce: un buffer de 16 KiB a 1 kHz se consume 12 veces con eventos `WRITE_DONE`. Se aplican el MCLK del LPASS (`0x15020018 = 1`) y el clock block/PA del PM660L. No hay salida audible porque el digital codec/SWR del LPASS que alimenta el puerto SWR DAC del PM660L no está clockeado/configurado; las lecturas AP de ese bloque se cuelgan cuando el DSP ya arrancó el audio. Ver [Audio](#audio).
- **2026-11-21 — Pantalla de inicio, Ajustes y Siri en pantalla.** Una captura del dispositivo muestra la pantalla de inicio de SpringBoard con íconos, dock, badge de notificación y el botón de la Biblioteca de apps; los dos widgets esperan en `Esperando…`. Ajustes abre con toda su lista y aparece la pantalla inicial de Siri. La consola de arranque muestra los self-tests de blit por software de Skylight pasando antes del lock screen. Evidencia: `docs/images/home-screen.jpg`, `settings.jpg`, `siri-setup.jpg`, `boot-self-tests.jpg`.
- **2026-11-19 — Fase 0.** ROM stock verificada, TWRP, backups de 15 particiones + GPT y runbook de restauración.
- **2026-11-21 — Touch arreglado.** Las compilaciones locales leían información de firmware basura del Novatek (`nvt fwinfo invalid`) y dejaban el touch desactivado; los kernels del CI (Xcode 15) no. `0234` hace que SweetHID use el panel real de 1080×2340: el swipe desde el borde inferior ahora desbloquea y los taps caen donde corresponde.
- **2026-11-21 — La galería de wallpapers abre.** Puntos de extensión y proveedores registrados y crash de la vista previa sin Metal esquivado: Color muestra vista previa. Astronomy sigue lento.
- **2026-11-21 — Perfil del compositor.** `spindump` muestra la CPU en el renderizador por software de CoreAnimation; `0235` alinea la notificación de vsync con el panel. Sin ganancia en reposo.
- **2026-11-19 — Primer boot XNU.** Kernel `LAVENDER` compilado en GitHub Actions; `launchd` murió con `CS_KILLED`.
- **2026-11-19 — Code signing.** Con los flags CS/RW arrancan `launchd`, `xpcproxy`, `cfprefsd`, `powerd`, `distnoted`, `watchdogd` y `SpringBoard`.
- **2026-11-19 — Escrituras eMMC.** El parche `0192` espera DAT0 + `TRAN`/`READY_FOR_DATA` por CMD13 después de cada write PIO. Antes, CMD24/CMD25 daban timeout y el anillo quedaba vacío; después, el anillo volcó 100+ KiB sin timeouts.
- **2026-11-21 — Lecturas eMMC por ADMA2.** El núcleo SDCC v5 no tiene SDMA, solo ADMA2. El parche `0233` pasa las lecturas a ADMA2 (con fallback a PIO; las escrituras siguen por PIO): las lecturas en frío suben de ~465 KB/s a ~16 MB/s y el arranque hasta el lock screen baja de ~23 min a ~2 min.
- **2026-11-19 — Cuatro Gold.** `cpus=4` levanta los cuatro A73; los A53 Silver siguen aparcados por las páginas de 16 KiB.
- **2026-11-19 — Migración inicial.** Una raíz nueva necesita una pasada larga de `DataMigration`. La imagen lleva `com.apple.DataMigration.plist` con `LastSystemVersion = 20H392` para que los siguientes boots la salten.
- **2026-11-19 — Lock screen de iOS 16.** El teléfono llegó al lock screen de iOS 16.7.16: `Wednesday, December 31`, `4:37`, acceso a cámara y home indicator. Ese es el estado actual.

## Audio

Estado (2026-11-26): **la ruta de datos AFE/ADM/ASM está verificada; todavía no hay sonido.**
Parches `0500`–`0522`; el detalle de registros está en [`docs/AUDIO.md`](docs/AUDIO.md).

```
iOS HAL -> /dev/lavender-audio -> ASM -> ADM COPP
  -> AFE INT4_MI2S_RX (0x1036) -> digital codec/SWR del LPASS (0x152c0000 / 0x152c1000)
  -> PM660L (SPMI SID 3, 0xf000): SWR DAC port -> PA/boost -> parlante
```

El PM660L es el códec del parlante. Los **WSA881x no están montados** en lavender
(`qcom,wsa-disable`; los hijos WSA del `msm-sdw-codec` están `disabled`), y `tasha`/`tavil`
están apagados por el DTBO. El auricular usa la misma ruta LPASS/SWR, así que no hay atajo
AP-only.

### Qué anda

- PAS/ADSP, GLINK `lpass` y APR: `0507`–`0509b`.
- AFE `INT4_MI2S_RX` clock/config/start, ADM open/route y ASM open/format/run: status 0.
- Tono de 16 KiB mapeado y escrito; el DSP devuelve `WRITE_DONE` (`0x10D99`), 12 veces con
  **map handle 0** (`0517c`).
- MCLK `0x15020018 = 1` y clock block + PA del PM660L (`0520`).

### Qué falta

- El **digital codec/SWR master** del LPASS no está clockeado/configurado para el AP: leer
  `0x152c0000`/`0x152c1000` con el DSP arriba cuelga (`0519`, `0522`). El voto AFE
  `0x100f4` solo acepta **AVTIMER (2)**; MACRO (3) y DCODEC (4) devuelven
  `ADSP_EUNSUPPORTED` (`0526`). Los clocks NPL (`0x307`) e INT_MCLK_1 (`0x306`) se aceptan
  (`0525`), pero el write al master SWR igual se traba.
- La ruta `INT4_MI2S_RX -> RX4/COMP1 -> SpkrLeft SWR DAC_Port` no se habilita, así que el DAC
  del PM660L no recibe datos; el PA encendido solo amplifica silencio.
- `VDD_SPK/BOB` y los reguladores del PM660L son de RPM; este port no tiene ese driver.

### Trampas

- SPMI de lavender: `0x0800f000`, observer `0x09400000 + 0x8000*apid`, PM660L **SID 3** base
  `0xf000`.
- Header APR de 32 bytes (`hdr_len = 8`); `mem_mapping_hdr` de 12 bytes en `SET_PARAM_V2`.
- ADM `DEVICE_OPEN_V5` responde `{status, u16 copp_id, u16 reserved}` y el COPP es **0**.
- ASM usa `src = dest = (session << 8) | stream = 0x0101`; el write usa **handle 0**, no el
  valor del map response (`0xb095aad8`).
- 16 KiB por write; 64 KiB cuelga; repetir solo al recibir `0x10D99`.
- El clock set `0x306` del AFE no genera MCLK; el real es `0x15020018 = 1`.
- El parlante es **INT4**, mono; INT0 es auricular.
- El voto AFE `0x100f4` solo sirve para **AVTIMER (2)**: con bloques 2/3/4, solo el 2
  devuelve handle; MACRO (3) y DCODEC (4) dan `ADSP_EUNSUPPORTED`. No reintentarlos.
- No leer `0x152c0000`/`0x152c1000` con el DSP arriba.

### Próximo

1. Topology AFE (`AFE_CMD_ADD_TOPOLOGIES`, ACDB 15) o el clock controller del LPASS: el
   voto AFE no puede encender MACRO/DCODEC.
2. Init del master SWR (`swr-mstr-ctrl.c`) en `0x152c1000`.
3. Ruta `SWR DAC_Port` del PM660L + PA/BOB.
4. `/dev/lavender-audio` + HAL y luego mic/auricular.

## Cómo arranca

```
ABL de Xiaomi
  -> shim en la partición recovery (Power + Volumen ARRIBA)
  -> kernel XNU + ADT + boot args
  -> raíz HFSX desde userdata (rd=emmcroot0)
  -> launchd -> mount-phase-1/2 -> backboardd -> SpringBoard
  -> lock screen de iOS 16
```

El shim se flashea en `recovery`; `fastboot boot` está stubbeado en esta unidad. `fastboot reboot recovery` arranca Android y MIUI restaura el recovery stock, así que la secuencia que funciona es: flashear el shim, apagar y encender con **Power + Volumen ARRIBA**.

Los boot args actuales incluyen:

```
rd=emmcroot0 emmcrw=1 rootrw=1
cs_enforcement_disable=1 unentitled_ios_sim_launch=1 launchd_unsecure_cache=1
sweetlog=1 sweetfb_noconsole=1 sweetfb_stayon=1 sweetfb_hz=30
wdt=-1 wdt_test=no_user_checkin cpus=4
```

Ver [`docs/BOOT.md`](docs/BOOT.md) para la cadena completa y cómo sacar el log.

## Build

El repositorio no contiene binarios de Apple, el IPSW, la imagen raíz de 16 GiB ni la ROM stock. Hay que aportarlos.

1. **Kernel** — forkear `aLpHa-Git-69/ios-lavender`, pushear a `lavender` y dejar que `.github/workflows/xnu.yml` compile `kernel.development.lavender` en `macos-15`.
2. **Shim** — `BOARD=lavender shim/build.sh <kernel>` dentro del árbol base; empaqueta kernel, ADT y boot args en un boot image Android.
3. **Imagen raíz** — extraer `OS.dmg`, `SystemOS.dmg` y `AppOS.dmg` del IPSW de iPhone X 16.7.16 y construir la HFSX con `bergamota/tools/mkiosroot-macos.sh` + `sweet-root-tweaks.py`.
4. **Flash** — `fastboot -S 512M flash userdata ios16-root.simg`, después `fastboot flash recovery shim-boot.img` y Power + Volumen ARRIBA.

Los comandos, hashes y advertencias están en [`docs/BUILD.md`](docs/BUILD.md).

## Créditos y legal

- [`aLpHa-Git-69/ios-lavender`](https://github.com/aLpHa-Git-69/ios-lavender): árbol XNU lavender/whyred, driver eMMC, shim y notas.
- [`dipa-studio/ios-sweet`](https://github.com/dipa-studio/ios-sweet): tooling y metodología.
- [`ry-diffusion/bergamota`](https://github.com/ry-diffusion/bergamota): `mkiosroot-macos.sh` y shim.
- [`MakrSas/ios-topaz`](https://github.com/MakrSas/ios-topaz): referencia A73/A53, Wi-Fi y GPU.
- [`oosexgang/hakios-xnu-kernel`](https://github.com/oosexgang/hakios-xnu-kernel): metodología.

XNU es open source (APSL); el userland de iOS, el IPSW y los binarios de Apple **no** se distribuyen acá. Es investigación; no usar con datos personales ni Apple ID. Ver [`docs/LEGAL.md`](docs/LEGAL.md).
