# Flashlight controller — revision 1.1

Рабочий проект: `flashlight.kicad_pro`, схема `flashlight.kicad_sch`, плата
`flashlight.kicad_pcb`. Исходная версия сохранена в
`revision_backup_20260924.zip`. Монтаж снизу, питание 2S 6–8,4 В,
лампа около 1 А. Генераторы находятся в `tools/`.

## Повторное использование компонентов

Референс: `U:/plate_schemas/speed_and_temp_v8`, все восемь переданных файлов
доступны. Перечень компонентов всех листов сохранён в `tools/reference_parts.json`.
Скопированные посадочные места автономны: для повторной сборки проекта диск U:
не нужен.

| Здесь | Референс | Решение |
|---|---|---|
| U1 | esp U1 | Тот же ESP32-C3-WROOM-02 |
| U2 | fan_power U5 | Тот же MCP16331T-E/CH |
| U3 | power U2 | TPS7A02-3.3 (TPS7A0233PDBVR), SOT-23-5, как в референсе |
| C10, C11 | power C6, C5 | 1 мкФ на входе и 4,7 мкФ на выходе LDO, 0805 |
| U4 | ina U6 | Тот же INA226, корпус 3×3 мм с шагом 0,5 мм |
| Q1 | fan_power Q1 | MMFTP3334K вместо AO3401A, G1/S2/D3 |
| Q2 | fan_power Q2, esp Q3 | NXV40UNR вместо 2N7002, G1/S2/D3 |
| R1 | ina R17 | WSL2512R0470FEA, 47 мОм / 1% / 1 Вт |
| D1 | fan_power D2 | PD3S120LQ, скопировано посадочное место PowerDI323 |
| D2 | fan_power D3 | 1N4148WS-HG3_A-08 |
| SW1, SW2 | esp SW1–SW3 | Скопировано посадочное место кнопок |
| R2, R8 | fan_power R29, R34 | 61,9 кОм и 100 кОм из референса |
| R3–R6, R9–R16, R18 | разные листы | Повторно используются номиналы 1 кОм, 100 кОм, 10 Ом, 10 кОм, 4,7 кОм |
| C2–C7, C10, C12–C16 | разные листы | 100 нФ, 1 мкФ и 10 мкФ в 0805; напряжение и диэлектрик по BOM |

Новые номиналы: R7 523 кОм, R17 12,4 кОм. Сохранены конденсаторы 22 мкФ,
470 мкФ и дроссель 22 мкГн: они выбраны для этого источника и нагрузки.
SRN4018-100M из референса имеет 10 мкГн; без проверки устойчивости и пульсаций
его не подставляем вместо рекомендованных для 5 В 22 мкГн.
Установлен TPS7A02-3.3 из референса: IN и EN подключены к 5 В, NC оставлен
неподключённым. Его номинальный выходной ток — 200 мА; работу ESP32-C3 с радио
нужно проверить на прототипе по просадке 3,3 В. ADS1115 не требуется для
одного термодатчика. Разъёмы SATA и вентиляторов не соответствуют назначению платы.

## Делители и потребление

R7/R8: **523 кОм / 100 кОм**, вместо 52,3 кОм / 10 кОм.
При VFB = 0,8 В выход 4,984 В, ток делителя 8 мкА вместо 80 мкА.
При типичном токе входа FB 3 нА добавочная ошибка выхода около 1,57 мВ.
Максимальный ток FB производитель не гарантирует, поэтому 100 кОм снизу —
выбранный инженерный предел, а не паспортный максимум. Увеличение ещё на порядок
без измерений неоправданно. Для 1% резисторов и диапазона VFB 0,784–0,816 В
статический диапазон примерно 4,803–5,170 В без учёта входного тока FB.
На прототипе проверить запуск, пульсации и скачки нагрузки при 6 и 8,4 В.

R2 = 61,9 кОм снижает постоянный ток включённого ключа примерно до
95–134 мкА. Увеличение до мегомов ограничено утечкой защищённого затвора
MMFTP3334K: 10 мкА плюс утечка Q2 1 мкА дали бы 0,681 В на 61,9 кОм.
Это оценка по указанным в паспортах условиям, не гарантия при любой температуре.
Ключ предназначен для медленного включения/выключения лампы; высокочастотный PWM
с резистивным выключением затвора не проверен.

Напряжение аккумулятора измеряет VBUS INA226 непосредственно; отдельного
постоянно включённого делителя батареи нет. Шунт 47 мОм даёт 47 мВ и 47 мВт
при 1 А. Диапазон INA226 ±81,92 мВ соответствует ±1,743 А **общего** тока,
включая электронику; выше этого измерение насыщается.
Для Current_LSB = 100 мкА регистр CAL = round(0,00512 / (0,0001 × 0,047)) = 1089.
Прошивку с прежними 20 мОм использовать без изменения калибровки нельзя.

Экономия на делителях не означает микроамперного сна всей платы:
включённый MCP16331 сам потребляет типично около 1 мА. INA226 также следует
переводить в power-down между измерениями, если требуется экономичный режим.

## АЦП и термодатчик

**Предварительное допущение: NTC 10 кОм при 25 °C, B3950, диапазон 0…100 °C.**
Фактический датчик и его допуски ещё нужно подтвердить. Выбранный номинал R17
нельзя считать окончательным для неизвестного датчика или отрицательных температур.

Используется GPIO0 / ADC1_CH0. GPIO1 / TEMP_EXCITE питает делитель только
на время измерения. R17 = 12,4 кОм; R18 = 1 кОм; C16 = 100 нФ у входа АЦП.
Постоянное питание 3,3 В с R17 убрано. Большое сопротивление R17 при том же NTC
ухудшило бы полезный размах сигнала; выключаемое питание уменьшает средний ток
без этого ухудшения.

| Температура | Сопротивление NTC, примерно | Напряжение АЦП при 3,3 В |
|---|---|---|
| 0 °C | 33,62 кОм | 2,411 В |
| 25 °C | 10 кОм | 1,473 В |
| 100 °C | 697,5 Ом | 0,176 В |

Размах 2,235 В использует около 89% диапазона 0…2,5 В.
Верхняя точка оставляет запас до насыщения, но после получения допусков датчика
нужен окончательный worst-case расчёт. При обрыве NTC вход поднимается к 3,3 В:
это насыщение измерителя; прошивка должна считать такой результат неисправностью,
а не температурой. При коротком замыкании вход близок к нулю.

Последовательность будущей прошивки:

1. Настроить ADC1_CH0 на ATTEN3 (`ADC_ATTEN_DB_12`; старое имя `ADC_ATTEN_DB_11`),
   включить поддерживаемую ESP-IDF калибровку.
2. Установить GPIO1 в высокий уровень; подождать **15 мс**.
   Даже при обрыве постоянная времени (12,4 кОм + 1 кОм) × 100 нФ = 1,34 мс.
3. Отбросить первую выборку, усреднить несколько измерений, перевести результат
   в мВ через калибровочный API. Измерения редкие, не поток 100 квыб/с.
4. Установить GPIO1 в низкий уровень до следующего измерения, в том числе перед сном.
5. Рассчитать Rntc = 12400 × Vadc / (Vexcite − Vadc), затем
   T = 1 / (1/298,15 + ln(Rntc/10000)/3950) − 273,15.
   Использовать измеренное/откалиброванное Vexcite: напряжение GPIO не идеальные 3,3 В.

Ток при 25 °C во время измерения около 147 мкА. При включении на 20 мс раз
в секунду среднее потребление делителя около 2,95 мкА; токи утечки сюда не включены.
Для C16 100 нФ обеспечен локальный запас заряда SAR и фильтрация; буфер для этой
редкой выборки не установлен. Итоговую погрешность проверить на известных
сопротивлениях с фактической частотой выборки и работающим радио. 12 бит не означают
12 бит абсолютной точности: для ATTEN3 паспортная ошибка после калибровки ±35 мВ
при оговорённых условиях, что особенно заметно на горячем конце NTC.

## Плата и проверки

В генераторе убраны отверстия в центральном паде ESP32. Трассировщик запрещает
via на любых падах, включая площадки той же цепи. Минимальный зазор между краем
кольца via и прямоугольником, охватывающим медь пада, — 0,15 мм.
`tools/verify_design.py` проверяет геометрию итогового файла, включая тепловые via,
и соответствие экспортированного netlist спецификации соединений.
Отчёты: `erc.rpt`, `drc.json`, `verification.json`.

Для генерации запустить `tools/build_project.py`, `tools/route_board.py`,
`tools/finish_board.py`, затем KiCad CLI ERC, DRC с `--refill-zones --save-board`,
экспорт XML netlist и `tools/verify_design.py`. Использовать настроенный Python SDK.
`tools/import_reference.py` нужен только для повторного импорта оригиналов.

## Документация производителей

- [ESP32-C3 datasheet, ADC и потребление](https://documentation.espressif.com/ESP32-C3_Datasheet_en.pdf)
- [Espressif: схема и вход АЦП](https://docs.espressif.com/projects/esp-hardware-design-guidelines/en/latest/esp32c3/schematic-checklist.html)
- [ESP-IDF: ADC](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/api-reference/peripherals/adc/index.html)
- [MCP16331, раздел 5.2 и характеристики](https://ww1.microchip.com/downloads/en/DeviceDoc/MCP16331-Data-Sheet-20005308.pdf)
- [MMFTP3334K](https://diotec.com/files/diotec/productfiles/datasheet/mmftp3334k.pdf)
- [NXV40UN](https://assets.nexperia.com/documents/data-sheet/NXV40UN.pdf)
- [PD3S120LQ](https://www.diodes.com/datasheet/download/PD3S120LQ.pdf)
- [TPS7A02](https://www.ti.com/product/TPS7A02)
- [SRN4018](https://www.bourns.com/docs/product-datasheets/srn4018.pdf)
- [INA226](https://www.ti.com/lit/ds/symlink/ina226.pdf)


## Compact revision 0.3 (2026-11-16)

Board outline: 65 x 55.25 mm, previously 75 x 65 mm (26.3% less area).
The ESP32 antenna still overhangs the top edge; these dimensions describe the PCB.
All component packages and four mounting holes are retained. Mounting coordinates changed.
PACK_PLUS / SYS_PLUS / LAMP_PLUS use 2 mm tracks with local SOT-23 necks.
INA226 inputs use separate sense branches; the buck switching loop remains on B.Cu.
Noise and power loss have not been measured on physical hardware.

J5: two 2 x 2 mm SMD solder pads, 3 mm pitch. Pad 1 = GYRO_SIGNAL / GPIO7
(module pin 6); pad 2 = GND. For a contact switching to ground, configure an input
with a pull-up in firmware. Active outputs must use 3.3 V logic. No sensor supply
is provided on J5.

Current production exports: `fabrication_rev03/`. The previous `gerber/` directory
belongs to the old layout. Updated generators are in `tools/`.
Checks: `erc.rpt`, `drc.json`, `verification.json`.


## Two-sided block layout, revision 0.4 (2026-11-16)

Current PCB outline: **42 x 44 mm**, area 1848 mm2. This is 48.5% smaller than
revision 0.3 (65 x 55.25 mm) and 62.1% smaller than the original 75 x 65 mm.
The antenna still overhangs the upper edge. Four 3.2 mm mounting holes are retained,
with new centres (mm, top view): H1 (2, 12.8), H2 (39, 3), H3 (3, 41), H4 (40, 35.3).

B.Cu: ESP32 module, buck switching loop, shunt, lamp MOSFET, bulk capacitor and
battery/lamp contacts. F.Cu: INA226 and its input filter, LDO, buttons, gate-drive
passives, UART and sensor solder pads. The analogue block lies above the power
area on the other face, away from the buck SW node. Existing component packages
and circuit values are retained. Main battery/lamp copper remains 2 mm wide,
with local necks at device pads. Low-current signals use 0.15 mm tracks.
Both faces require assembly. Electrical noise and temperature require prototype
measurements; DRC alone does not evaluate them.

J5 is on F.Cu at (5, 34): pad 1 SIG = GPIO7, pad 2 GND, 2 x 2 mm pads at 3 mm pitch.
The signal is still 3.3 V logic; configure a pull-up for a contact to ground.

Use **fabrication_rev04/** for production. **fabrication_rev03/** and **gerber/**
are previous layouts. Layer previews: `previews/compact-rev04-top.svg` and
`previews/compact-rev04-bottom.svg`. `revision03_before_block_relayout.zip` preserves
the previous board and its generators.

Validation: ERC 0 violations; DRC 0 violations, 0 unconnected items, 0 schematic
parity issues. Independent geometry verification: 98 vias, none on a pad,
minimum required via annulus-to-pad gap 0.15 mm. Reports are in `erc.rpt`,
`drc.json` and `verification.json`.


## Mechanical revision 0.5 (2026-11-16)

Current outline: **42 x 56 mm**, from KiCad (0, -8) to (42, 48).
Four NPTH mounting holes, diameter 3.2 mm, now form a **38 x 31 mm rectangle**.
From the physical upper-left board corner their centres are:
H1 (2, 22), H2 (40, 22), H3 (2, 53), H4 (40, 53) mm.
KiCad coordinates: H1 (2, 14), H2 (40, 14), H3 (2, 45), H4 (40, 45).
The hole diameter is retained; screw-head and enclosure clearances must be matched
against the actual mounting hardware.

The ESP32 module, including its antenna, is fully above the base-board FR4.
The module body outline is x = 4.5..22.5, y = -5.85..14.15 mm in KiCad coordinates;
there is 2.15 mm of FR4 beyond its antenna end. The footprint antenna rule area
prohibits copper pours, tracks, vias and pads on both copper layers. The added
upper FR4 strip has no copper. Keep metal enclosure parts away from the antenna.
RF performance with FR4 under the antenna remains to be measured on the prototype;
see [Espressif module placement guidance](https://docs.espressif.com/projects/esp-hardware-design-guidelines/en/latest/esp32c3/pcb-layout-design.html#general-principles-of-pcb-layout-for-modules-positioning-a-module-on-a-base-board).

Current manufacturing outputs: **fabrication_rev05/**.
Mechanical drawing: `previews/mechanical-rev05.svg`; assembly layer views:
`previews/compact-rev05-top.svg`, `previews/compact-rev05-bottom.svg`.
Previous revision files remain historical. `revision04_before_mechanical_update.zip`
preserves the board before this update. `tools/verify_design.py` now checks the
hole rectangle, integer pitch, module support inside the outline and antenna keepout.


## Active buzzer, revision 0.6 (2026-11-16)

BZ1 is an active 3 V magnetic sounder, PUI AI-1223-TWT-3V-2-R or equivalent:
diameter 12 mm, lead pitch 7.6 mm, operating voltage 2..4 V, rated current <=30 mA
at 3 V. Standard KiCad land pattern: `Buzzer_Beeper:Buzzer_12x9.5RM7.6`, copied
into the local Flashlight library. The selected PUI part is 14 mm high: the generic
footprint name's 9.5 mm does not describe the selected part's height.
Datasheet: https://puiaudio.com/file/specs-AI-1223-TWT-3V-2-R.pdf

BZ1 pad 1 = 3V3, pad 2 = BUZZER_MINUS = Q3 drain. Q3 NXV40UNR is G1/S2/D3;
source connects to board GND, gate is driven by GPIO18 (U1 module pad 13) through
R19 = 100 ohm. R20 = 100 kohm pulls gate to source, ensuring default OFF.
D4 1N4148WS clamps the magnetic load: cathode to 3V3, anode to BUZZER_MINUS.
GPIO18 high = sound, low = silence. PWM is unnecessary for an active indicator.
GPIO18 is reserved for this output and cannot simultaneously be USB D-.
BZ1 is assembled on B.Cu, its driver and resistors are on F.Cu.
The 42 x 56 mm outline, 38 x 31 mm mounting pattern and antenna support remain.

Correction to earlier prose: J5 signal is **GPIO7 (module pad 6)**. Earlier notes
calling it GPIO10 were incorrect; the electrical connection has not changed.
GPIO10 (module pad 10) controls the lamp. Current pin assignment is authoritative
in `tools/expected_nets.json` and the schematic.

Current production exports: **fabrication_rev06/**. Current layer previews:
`previews/compact-rev06-top.svg`, `previews/compact-rev06-bottom.svg`.
The prior board is preserved in `revision05_before_buzzer.zip`.


Revision 0.6 validation: ERC and schematic/PCB parity checked; no incomplete
connections. Mechanical verification and 0.15 mm via-to-pad exclusion pass.


## Manual reset, revision 0.7 (2026-11-16)

SW3 RESET uses the same local `Flashlight:Reference_Button` footprint as BOOT
and USER. It is on B.Cu at (36.5, 0.6) mm in the KiCad board coordinate system.
Contact 1 connects to ESP_EN; contact 2 connects to GND. Pressing the momentary
button holds ESP32-C3 in reset without cycling the permanently connected supply.
No GPIO or firmware handler is needed.

R9 = 10 kohm connects 3V3 to ESP_EN. C14 = 1 uF connects ESP_EN to GND, in
parallel with the reset switch. The RC time constant is 10 ms; it delays enable
while the supply rises and filters short glitches. EN is not wired directly to
3V3: R9 limits the pressed-button current to approximately 0.33 mA.
These values follow Espressif's recommended EN power-on RC circuit:
https://docs.espressif.com/projects/esp-hardware-design-guidelines/en/latest/esp32c3/schematic-checklist.html

The 42 x 56 mm outline, 38 x 31 mm mounting pattern, antenna support, active
buzzer circuit and its clamp diode are retained. Current manufacturing exports:
**fabrication_rev07/**. New previews: `previews/compact-rev07-top.svg`,
`previews/compact-rev07-bottom.svg`, and `previews/rev07-schematic/`.
The previous board is preserved in `revision06_before_reset.zip`.


## UART pads only, revision 0.8 (2026-11-16)

J4 is now `Flashlight:UART_Pads_3` with only three SMD contacts at 2.54 mm pitch:
1 = GND, 2 = UART_TX (ESP32 TX), 3 = UART_RX (ESP32 RX). EN, BOOT and 3V3
contacts were removed. Programming mode is entered with the existing BOOT and
RESET buttons: hold BOOT, press/release RESET, then release BOOT.
Connect adapter RX to board TX, adapter TX to board RX, and share GND;
use 3.3 V UART logic. Power remains supplied through PACK.

There are no standalone testpoint footprints. The functional PACK/LAMP, NTC and
external contact-sensor pads are retained. Earlier sections describe past revisions.
The 42 x 56 mm outline, 38 x 31 mm mounting pattern, reset button, buzzer and antenna
support are retained. Current manufacturing exports: **fabrication_rev08/**.
Layer views: `previews/compact-rev08-top.svg`, `previews/compact-rev08-bottom.svg`.
Schematic previews: `previews/rev08-schematic/`. Previous board archive:
`revision07_before_uart_cleanup.zip`.


## Functional silkscreen and button ergonomics, revision 0.9 (2026-11-16)

BOOT (SW1) and RESET (SW3) are on F.Cu in one horizontal row at (8, 12) and
(21, 12) mm, 13 mm apart, using the existing Reference_Button footprint. They can
be held with two fingers from the same face. USER (SW2) is separate at (34, 6).
For flashing: hold BOOT, press RESET, release RESET while still holding BOOT,
then release BOOT. RESET is EN-to-GND; BOOT is GPIO9-to-GND.

Mandatory functional legends replace connector/button reference-only labels.
BOOT, RESET and USER have full names and actuator outlines; FLASH identifies the
programming button pair. UART has UART / GND / TX / RX (board signal names).
PACK contacts have BAT and large + / - on both faces; lamp contacts have LAMP and
large + / - on both faces. NTC has NTC / T / GND on both faces. J5 has GYRO / SIG /
GND on F.Cu. Main labels are 0.9..1.4 mm high; assembly references are secondary.
The finishing script raises an error if a required functional label cannot fit.
The 42 x 56 mm board and 38 x 31 mm mounting pattern are retained.

Current production exports: **fabrication_rev09/**. Layer views:
`previews/compact-rev09-top.svg`, `previews/compact-rev09-bottom.svg`.
Schematic previews: `previews/rev09-schematic/`. The previous revision is saved in
`revision08_before_ui_layout.zip`.


## Flat wired schematic, revision 1.0 (2026-11-16)

The only current schematic is **flashlight.kicad_sch**: one A2 landscape sheet,
all 57 physical components, with visible circuit wires. No hierarchical sheets.
The former power/sense/controller sheets are preserved in
`revision09_before_flat_schematic.zip` and removed from the current project.
Power flows across the top: J1 battery -> R1 shunt -> SYS_PLUS -> lamp switch,
with the SYS_PLUS branch feeding MCP16331 U2 + L1 -> 5V -> TPS7A02 U3 -> 3V3.

**5V is generated on this PCB, not an external input.** The only external supply
is the 2S battery on J1 (6.0..8.4 V). The lamp gets switched battery voltage;
ESP32, INA226 and buzzer use the internally generated 3.3 V rail. PWR_FLAG symbols
are ERC declarations on existing wires, not additional supply sources.

The shunt input filters and INA226 are below the entrance, the ESP32 and its
reset/BOOT/USER circuits occupy the middle, and NTC, UART, LED/contact input and
buzzer circuits are wired nearby. Power/ground markers are shared in the usual
way; signal net names appear once on their connected conductors. Dots identify
connected intersections; the exported PDF/SVG also draw wire-crossing hops.

Readable exports: **previews/flashlight-flat-rev10.pdf** (one vector page) and
**previews/rev10-flat/flashlight.svg**. Source generator: `tools/flat_schematic.py`,
called by `tools/build_project.py`. Existing component UUIDs are retained and
PCB footprint hierarchy links updated to the root sheet. Net names and every
physical component pin connection match `tools/expected_nets.json`.
The PCB copper, vias, component positions and board outline were not changed;
manufacturing outputs in `fabrication_rev09/` remain current.
Validation reports: `erc.rpt`, `drc.json`, `verification.json`,
`flat-schematic-check.json` (unchanged copper/graphics checksum).


## Wired logical sheets, revision 1.1 (2026-11-16)

The project again has three logical circuit sheets under `flashlight.kicad_sch`:

- `sense.kicad_sch`: battery input, shunt, lamp switch and its gate drive, INA226
  and differential filter. The circuits are connected with visible wires.
- `power.kicad_sch`: one complete wired power chain, SYS_PLUS -> MCP16331/L1 ->
  internally generated 5V -> TPS7A02 -> 3V3, including capacitors and feedback.
- `controller.kicad_sch`: ESP32 with its connected reset/BOOT/USER circuits,
  UART, I2C pull-ups, NTC/ADC filter, LED, contact input and buzzer driver.

Only rails and actual cross-sheet signals use named links. Local resistors,
capacitors, buttons and transistor drivers remain wired as complete circuits,
instead of disconnected symbols with a label on each pin.
The top sheet is an index; each block has its own circuit page.
**5V is generated by U2/L1 on the power sheet. No external 5V input is required.**

Current readable export: **previews/flashlight-logical-rev11.pdf** (index plus
three circuit pages), with SVG pages in `previews/rev11-logical/`.
Source generation: `tools/logical_schematic.py` uses the wired placement template
in `tools/wired_circuits.py`; both are called by `tools/build_project.py`.
The previous single-sheet version is preserved in `revision10_before_logical_sheets.zip`.
The physical board layout and manufacturing files in `fabrication_rev09/` remain
valid. Only schematic hierarchy links in PCB footprints were updated; copper,
vias, component positions and board geometry are unchanged.
