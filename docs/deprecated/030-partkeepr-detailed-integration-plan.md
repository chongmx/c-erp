# PartKeepr Integration — Detailed Plan
*Planned: 2026-03-26 | Supersedes: 029-partkeeper-integration-plan.md*

---

## 1. Objectives & Scope

This document is the authoritative implementation plan for integrating electronics component
management (inspired by PartKeepr) into c-erp. It covers:

- Complete schema and API design for all phases
- Meta-part concept: virtual parts that aggregate stock from matching real parts
- Parameter-based part search with SI-prefix-normalised range queries
- Component type catalog with typical attributes and units (seed data)
- **Protocol strategy**: JSON-RPC only for all data operations; future migration path to
  binary gRPC and HTTP/3 for real-time features

### Out-of-scope in this plan
- Project runs / production batch tracking (covered by MRP)
- Stock audit log (covered by stock transfers)
- Storage location images (trivial add-on once PK7 is complete)
- Distributor URL templates (cosmetic)

---

## 2. Protocol Strategy

### 2.1 Current: JSON-RPC over HTTP/1.1

All data operations use the existing `/web/jsonrpc` endpoint:

```
POST /web/jsonrpc
Content-Type: application/json

{
  "jsonrpc": "2.0",
  "method": "call",
  "params": {
    "service": "object",
    "method": "execute_kw",
    "args": ["product.product", "search_read",
             [[["categ_id","=",5]]], {"fields":["name","default_code"]}]
  }
}
```

**File upload** (PK7 attachments): base64-encoded content in the `db_datas` field of an
`ir.attachment` create call — no multipart form needed.

**File download**: A thin binary GET route `/web/content/<id>` streams the `db_datas` back
as raw bytes. This is NOT a data API; it is a browser helper required because `<img src>`,
`<embed src>` and PDF-viewer iframes cannot consume a JSON-RPC payload. It does not carry any
business logic — just decodes base64 and sets `Content-Type`. This is the only non-JSON-RPC
route added by these phases.

### 2.2 Future: binary gRPC (planned, not implemented here)

When the system grows to high-frequency inventory operations or microservice split:

| Now (JSON-RPC) | Future (gRPC) |
|---|---|
| `search_read` | `rpc SearchParts(SearchRequest) returns (stream Part)` |
| `write` / `create` | `rpc UpsertPart(Part) returns (Part)` |
| Batch stock update | `rpc BulkStockUpdate(stream StockEntry) returns (BatchResult)` |

Migration path:
1. Define `.proto` files alongside the C++ module (no code gen needed yet — just the spec).
2. When ready, add a `grpc_server` alongside the existing Drogon HTTP server on a separate port.
3. JSON-RPC and gRPC can coexist indefinitely — front-ends migrate independently.

The existing domain-filter format `[["field","op","value"], ...]` maps cleanly to a protobuf
`repeated Filter` message, so the query logic in C++ is reused unchanged.

### 2.3 Future: HTTP/3 / QUIC for real-time features

Chat, voice calls, video meetings are **separate concern** from the parts ERP. When built:

| Feature | Protocol | Notes |
|---|---|---|
| Chat messages | HTTP/3 + SSE or WebSocket over QUIC | Low-latency bidirectional |
| Voice call | WebRTC over QUIC | Browser-native |
| Video meeting | WebRTC data/media channels | Separate service |
| Live inventory push | HTTP/3 SSE | Server pushes stock changes |

These features will live in a separate `modules/realtime/` module and do not share the
JSON-RPC dispatch path.

---

## 3. Revised Phase Map

```
PK1   Category Tree UI            — frontend only, zero new tables
PK2   Footprints                  — 2 tables: part_footprint_category, part_footprint
PK3   Part Parameters & SI Units  — 3 tables: part_si_prefix, part_unit, part_parameter
PK4   Manufacturer Part Numbers   — 1 table:  part_manufacturer_info
PK5   Enhanced Supplier Info      — 0 tables, extend A3b product_supplierinfo
PK6   Min Stock & Part Status     — 0 tables, 3 columns on product_product
PK7   Attachments / Datasheets    — 1 table:  ir_attachment
PK8   Meta-parts                  — 1 table:  part_meta_criteria
                                     1 column: product_product.is_meta_part
```

**Recommended implementation order:**

```
A3b (vendor pricelist + PK5) → PK6 → PK1 → PK4 → PK2 → PK3 → PK7 → PK8
```

PK8 (meta-parts) depends on PK3 (parameters must exist before criteria can reference them).

---

## 4. Component Type Catalog — Seed Data

The seed data below defines a realistic starting category tree and, for each leaf category,
the parameter names + units that are typically populated. This data is seeded via
`ensureSchema_()` using `INSERT … ON CONFLICT DO NOTHING`.

### 4.1 Category Tree (product_category seeds)

```
Electronics
├── Passives
│   ├── Resistors
│   │   ├── Through-Hole Resistors
│   │   ├── SMD Resistors
│   │   ├── Resistor Networks / Arrays
│   │   └── Potentiometers & Trimmers
│   ├── Capacitors
│   │   ├── Ceramic Capacitors (MLCC)
│   │   ├── Electrolytic Capacitors
│   │   ├── Tantalum Capacitors
│   │   ├── Film Capacitors
│   │   └── Supercapacitors
│   ├── Inductors & Coils
│   │   ├── SMD Inductors
│   │   ├── Through-Hole Inductors
│   │   ├── Ferrite Beads
│   │   └── Transformers
│   └── Crystals & Oscillators
│       ├── Quartz Crystals
│       └── Oscillator Modules (TCXO, VCXO, XO)
├── Semiconductors
│   ├── Diodes
│   │   ├── Signal / Switching Diodes
│   │   ├── Rectifier Diodes
│   │   ├── Schottky Diodes
│   │   ├── Zener Diodes
│   │   └── TVS / ESD Protection
│   ├── Transistors
│   │   ├── NPN BJT
│   │   ├── PNP BJT
│   │   ├── N-Channel MOSFET
│   │   ├── P-Channel MOSFET
│   │   └── JFET
│   ├── Voltage Regulators
│   │   ├── Linear Regulators (LDO)
│   │   ├── Switching Regulators (Buck)
│   │   ├── Switching Regulators (Boost)
│   │   ├── Buck-Boost Regulators
│   │   └── Voltage References
│   ├── Operational Amplifiers
│   │   ├── General Purpose Op-Amps
│   │   ├── Rail-to-Rail Op-Amps
│   │   ├── Instrumentation Amplifiers
│   │   └── Comparators
│   ├── Logic ICs
│   │   ├── Gates (AND, OR, NAND, NOR, XOR)
│   │   ├── Buffers / Drivers
│   │   ├── Flip-Flops / Latches
│   │   ├── Multiplexers / Demultiplexers
│   │   └── Level Shifters / Translators
│   ├── Microcontrollers & Processors
│   │   ├── 8-bit MCU (AVR, PIC, STM8)
│   │   ├── 32-bit MCU (ARM Cortex-M)
│   │   ├── RISC-V MCU
│   │   ├── Application Processors
│   │   └── DSP
│   ├── Memory
│   │   ├── EEPROM
│   │   ├── Flash (SPI/Parallel)
│   │   ├── SRAM
│   │   └── SDRAM / DDR
│   ├── Interface ICs
│   │   ├── UART / RS-232 / RS-485
│   │   ├── SPI / I2C / SMBus
│   │   ├── USB Controllers / Bridges
│   │   ├── CAN / LIN Transceivers
│   │   └── Ethernet ICs
│   ├── Power Management
│   │   ├── Battery Management ICs
│   │   ├── Load Switches
│   │   ├── Power Multiplexers
│   │   └── Hot-Swap Controllers
│   ├── RF / Wireless
│   │   ├── Bluetooth Modules
│   │   ├── Wi-Fi Modules
│   │   ├── LoRa Modules
│   │   ├── Zigbee / Thread ICs
│   │   └── RF Amplifiers / Filters
│   ├── Sensors
│   │   ├── Temperature Sensors
│   │   ├── Humidity Sensors
│   │   ├── Pressure Sensors
│   │   ├── IMU (Accelerometer / Gyroscope)
│   │   ├── Magnetic Sensors / Hall Effect
│   │   ├── Light / Proximity Sensors
│   │   ├── Gas / Chemical Sensors
│   │   └── Current / Power Monitors
│   └── Optocouplers & Optoisolators
├── Display & LED
│   ├── Discrete LEDs
│   │   ├── Through-Hole LEDs
│   │   └── SMD LEDs
│   ├── LED Drivers
│   ├── 7-Segment Displays
│   ├── LCD Modules
│   ├── OLED Modules
│   └── e-Paper Displays
├── Electromechanical
│   ├── Relays
│   │   ├── Signal Relays
│   │   ├── Power Relays
│   │   └── Solid State Relays (SSR)
│   ├── Switches & Buttons
│   │   ├── Tactile Switches
│   │   ├── Toggle / Slide Switches
│   │   ├── DIP Switches
│   │   ├── Rotary Switches
│   │   └── Limit / Micro Switches
│   ├── Connectors
│   │   ├── Pin Headers (2.54mm)
│   │   ├── JST Connectors
│   │   ├── XT30 / XT60 Power Connectors
│   │   ├── USB Connectors (Type-A/B/C, Micro, Mini)
│   │   ├── Audio Jacks (3.5mm, 6.35mm)
│   │   ├── RJ45 Connectors
│   │   ├── D-Sub Connectors
│   │   ├── Screw Terminals
│   │   └── FFC / FPC Connectors
│   ├── Motors & Actuators
│   │   ├── DC Motors
│   │   ├── Stepper Motors
│   │   ├── Servo Motors
│   │   └── Solenoids
│   └── Fuses & Protection
│       ├── Blade Fuses
│       ├── Resettable Fuses (PTC)
│       └── Fuse Holders
├── Mechanical & Hardware
│   ├── Fasteners
│   │   ├── Screws (M2 / M3 / M4 / M5)
│   │   ├── Nuts
│   │   ├── Standoffs & Spacers
│   │   └── Washers
│   ├── Heatsinks & Thermal
│   │   ├── TO-220 Heatsinks
│   │   ├── Thermal Pads
│   │   └── Thermal Paste
│   └── Enclosures & Panels
├── PCB & Fabrication
│   ├── Blank PCBs
│   ├── Prototyping / Stripboard
│   └── Flex PCBs
└── Cables & Wire
    ├── Hook-Up Wire
    ├── Ribbon Cable
    ├── Coaxial Cable
    └── USB / Power Cables
```

---

### 4.2 SI Prefixes (seed data — read-only)

| id | name  | symbol | exponent | base |
|----|-------|--------|----------|------|
| 1  | pico  | p      | -12      | 10   |
| 2  | nano  | n      | -9       | 10   |
| 3  | micro | µ      | -6       | 10   |
| 4  | milli | m      | -3       | 10   |
| 5  | —     | (none) | 0        | 10   |
| 6  | kilo  | k      | 3        | 10   |
| 7  | mega  | M      | 6        | 10   |
| 8  | giga  | G      | 9        | 10   |
| 9  | tera  | T      | 12       | 10   |

`normalized_value = value × base^exponent`

Example: 10 µF → `10 × 10^-6 = 0.00001 F`

---

### 4.3 Part Units (seed data)

| id | name              | symbol | typical_prefixes (ids)    | use_case |
|----|-------------------|--------|---------------------------|----------|
| 1  | Ohm               | Ω      | 1,2,3,4,5,6,7,8           | Resistance, impedance |
| 2  | Farad             | F      | 1,2,3,4,5                 | Capacitance |
| 3  | Henry             | H      | 1,2,3,4,5,6               | Inductance |
| 4  | Volt              | V      | 3,4,5,6                   | Voltage |
| 5  | Ampere            | A      | 1,2,3,4,5,6               | Current |
| 6  | Watt              | W      | 3,4,5,6                   | Power |
| 7  | Hertz             | Hz     | 5,6,7,8,9                 | Frequency |
| 8  | Coulomb           | C      | 1,2,3,4,5                 | Charge |
| 9  | Celsius           | °C     | 5                         | Temperature |
| 10 | Kelvin            | K      | 5                         | Temperature (absolute) |
| 11 | Second            | s      | 1,2,3,4,5,6               | Time |
| 12 | Meter             | m      | 1,2,3,4,5,6               | Length / physical dimension |
| 13 | Gram              | g      | 4,5,6                     | Weight |
| 14 | Percent           | %      | 5                         | Tolerance, duty cycle |
| 15 | Parts per million | ppm    | 5                         | Temperature coefficient, accuracy |
| 16 | Decibel           | dB     | 5                         | Gain, attenuation |
| 17 | dB-milliwatt      | dBm    | 5                         | RF power level |
| 18 | Bit               | b      | 5,6,7,8,9                 | Memory capacity |
| 19 | Byte              | B      | 5,6,7,8,9                 | Memory capacity |
| 20 | V/µs              | V/µs   | 5                         | Slew rate (op-amps) |
| 21 | Ω·ppm/°C          | ppm/°C | 5                         | Temp. coefficient of resistance |
| 22 | mΩ/square         | mΩ/sq  | 5                         | Sheet resistance |
| 23 | Volt-Ampere       | VA     | 3,4,5,6                   | Apparent power (transformers) |
| 24 | Ampere-hour       | Ah     | 3,4,5                     | Battery capacity |

---

### 4.4 Parameter Definitions by Component Type

These are the standard parameters for each category. A `parameter_template` table (future)
or the seed data in `ensureSchema_()` can auto-populate these on new product creation.

#### Resistors

| Parameter         | Type    | Unit | Common Prefixes | Notes |
|-------------------|---------|------|-----------------|-------|
| Resistance        | numeric | Ω    | Ω, kΩ, MΩ       | Primary value |
| Tolerance         | numeric | %    | —               | ±1%, ±5% |
| Power Rating      | numeric | W    | mW, W           | Max dissipation |
| Voltage Rating    | numeric | V    | V               | Max operating voltage |
| Temp. Coefficient | numeric | ppm/°C | —             | TCR |
| Package / Case    | string  | —    | —               | 0402, 0603, 0805, 1206, MELF, TO-92 |

#### Capacitors

| Parameter         | Type    | Unit | Common Prefixes  | Notes |
|-------------------|---------|------|------------------|-------|
| Capacitance       | numeric | F    | pF, nF, µF       | Primary value |
| Voltage Rating    | numeric | V    | V                | DC working voltage |
| Tolerance         | numeric | %    | —                | ±10%, ±20% |
| Dielectric        | string  | —    | —                | X5R, X7R, C0G, Y5V, electrolytic |
| ESR               | numeric | Ω    | mΩ, Ω            | Equivalent series resistance |
| Temp. Coefficient | numeric | ppm/°C | —              | — |
| Package / Case    | string  | —    | —                | 0402, 0603, 0805, radial, SMD |
| Height            | numeric | m    | mm               | Important for space-constrained designs |
| Rated Temperature | numeric | °C   | —                | Max operating temp |

#### Inductors & Coils

| Parameter           | Type    | Unit | Common Prefixes | Notes |
|---------------------|---------|------|-----------------|-------|
| Inductance          | numeric | H    | µH, mH, H       | Primary value |
| DC Resistance (DCR) | numeric | Ω    | mΩ, Ω           | Series resistance |
| Current Rating (Isat)| numeric | A   | mA, A           | Saturation current |
| Current Rating (Irms)| numeric | A   | mA, A           | RMS / thermal rating |
| Self-Resonant Freq. | numeric | Hz   | MHz, GHz        | SRF |
| Q Factor            | numeric | —    | —               | Quality factor |
| Package / Case      | string  | —    | —               | 0402, 0603, 1008, through-hole |
| Shielded            | string  | —    | —               | Yes / No |

#### Ferrite Beads

| Parameter         | Type    | Unit | Notes |
|-------------------|---------|------|-------|
| Impedance @ 100MHz| numeric | Ω    | Primary selection value |
| DC Resistance     | numeric | Ω    | — |
| Current Rating    | numeric | A    | — |
| Package           | string  | —    | — |

#### Crystals

| Parameter         | Type    | Unit | Notes |
|-------------------|---------|------|-------|
| Frequency         | numeric | Hz   | Fundamental frequency |
| Load Capacitance  | numeric | F    | pF |
| Frequency Tolerance| numeric | ppm | At 25°C |
| Frequency Stability| numeric | ppm | Over temperature |
| ESR               | numeric | Ω    | — |
| Package           | string  | —    | HC-49, SMD 3225, SMD 2016 |

#### Oscillator Modules

| Parameter         | Type    | Unit | Notes |
|-------------------|---------|------|-------|
| Frequency         | numeric | Hz   | — |
| Supply Voltage    | numeric | V    | — |
| Output Type       | string  | —    | CMOS, TTL, Clipped Sine |
| Frequency Stability| numeric | ppm | — |
| Package           | string  | —    | DIP-4, SMD |

#### Signal / Switching Diodes

| Parameter         | Type    | Unit | Notes |
|-------------------|---------|------|-------|
| Forward Voltage (Vf)| numeric| V   | At rated current |
| Max Forward Current (If)| numeric| A | — |
| Reverse Voltage (Vr)| numeric| V   | — |
| Reverse Recovery Time| numeric| s  | ns — important for switching |
| Package           | string  | —    | DO-35, SOT-23, SOD-323 |

#### Zener Diodes

| Parameter         | Type    | Unit | Notes |
|-------------------|---------|------|-------|
| Zener Voltage (Vz)| numeric | V    | Regulation voltage |
| Test Current (Iz) | numeric | A    | mA |
| Power Rating      | numeric | W    | mW, W |
| Tolerance         | numeric | %    | — |
| Package           | string  | —    | DO-35, SOT-23 |

#### Schottky Diodes

| Parameter         | Type    | Unit | Notes |
|-------------------|---------|------|-------|
| Forward Voltage (Vf)| numeric| V   | Lower than Si diodes |
| Max Forward Current | numeric| A   | — |
| Reverse Voltage   | numeric | V    | — |
| Reverse Leakage   | numeric | A    | µA — important for power |
| Package           | string  | —    | — |

#### TVS / ESD Protection

| Parameter         | Type    | Unit | Notes |
|-------------------|---------|------|-------|
| Standoff Voltage  | numeric | V    | Working voltage |
| Breakdown Voltage | numeric | V    | — |
| Clamping Voltage  | numeric | V    | At peak pulse current |
| Peak Pulse Current| numeric | A    | A |
| Capacitance       | numeric | F    | pF — critical for high-speed lines |
| Package           | string  | —    | — |

#### BJT Transistors (NPN/PNP)

| Parameter         | Type    | Unit | Notes |
|-------------------|---------|------|-------|
| Vceo (Vce max)    | numeric | V    | Collector-emitter breakdown |
| Ic max            | numeric | A    | Max collector current |
| hFE (DC gain)     | numeric | —    | Current gain range, e.g. "100-300" as string |
| Vce(sat)          | numeric | V    | Saturation voltage |
| Transition Freq (ft)| numeric| Hz  | MHz — unity-gain bandwidth |
| Power Dissipation | numeric | W    | — |
| Package           | string  | —    | TO-92, TO-220, SOT-23 |

#### MOSFET (N/P Channel)

| Parameter         | Type    | Unit | Notes |
|-------------------|---------|------|-------|
| Vds max           | numeric | V    | Drain-source breakdown |
| Id max (continuous)| numeric| A    | — |
| Rds(on)           | numeric | Ω    | mΩ at given Vgs |
| Vgs(th)           | numeric | V    | Gate threshold |
| Vgs max           | numeric | V    | Max gate-source voltage |
| Qg (gate charge)  | numeric | C    | nC — switching speed indicator |
| Coss              | numeric | F    | pF — output capacitance |
| Power Dissipation | numeric | W    | — |
| Package           | string  | —    | TO-220, SOT-23, DPAK |

#### Linear Voltage Regulators (LDO)

| Parameter         | Type    | Unit | Notes |
|-------------------|---------|------|-------|
| Input Voltage max | numeric | V    | — |
| Output Voltage    | numeric | V    | Fixed or adjustable |
| Output Current max| numeric | A    | — |
| Dropout Voltage   | numeric | V    | Vin-Vout minimum |
| Quiescent Current | numeric | A    | µA — important for battery |
| Adjustable        | string  | —    | Yes / No |
| Package           | string  | —    | TO-220, SOT-23, SOT-223 |

#### Switching Regulators (Buck/Boost)

| Parameter         | Type    | Unit | Notes |
|-------------------|---------|------|-------|
| Input Voltage min | numeric | V    | — |
| Input Voltage max | numeric | V    | — |
| Output Voltage    | numeric | V    | Fixed or adjustable range |
| Output Current max| numeric | A    | — |
| Switching Frequency| numeric| Hz  | kHz, MHz |
| Efficiency        | numeric | %    | Typical peak efficiency |
| Topology          | string  | —    | Buck, Boost, Buck-Boost, Flyback, SEPIC |
| Package           | string  | —    | — |

#### Op-Amps

| Parameter             | Type    | Unit | Notes |
|-----------------------|---------|------|-------|
| Supply Voltage min    | numeric | V    | ±V or single supply |
| Supply Voltage max    | numeric | V    | — |
| GBW (Gain-Bandwidth)  | numeric | Hz   | MHz — key figure of merit |
| Slew Rate             | numeric | V/µs | — |
| Input Offset Voltage  | numeric | V    | µV, mV |
| Input Bias Current    | numeric | A    | pA, nA |
| Rail-to-Rail          | string  | —    | Input / Output / Both / None |
| CMRR                  | numeric | dB   | — |
| Number of Channels    | string  | —    | Single, Dual, Quad |
| Package               | string  | —    | SOT-23-5, SOIC-8, DIP-8 |

#### Comparators

| Parameter             | Type    | Unit | Notes |
|-----------------------|---------|------|-------|
| Supply Voltage max    | numeric | V    | — |
| Response Time         | numeric | s    | ns, µs |
| Input Offset Voltage  | numeric | V    | mV |
| Output Type           | string  | —    | Open-Drain, Push-Pull, CMOS |
| Number of Channels    | string  | —    | Single, Dual, Quad |
| Package               | string  | —    | — |

#### Microcontrollers

| Parameter         | Type    | Unit | Notes |
|-------------------|---------|------|-------|
| Architecture      | string  | —    | AVR, ARM Cortex-M0, M0+, M3, M4, M4F, M7, RISC-V |
| Flash Memory      | numeric | B    | kB, MB |
| RAM               | numeric | B    | kB, MB |
| EEPROM            | numeric | B    | B, kB |
| Max Clock Speed   | numeric | Hz   | MHz |
| Supply Voltage min| numeric | V    | — |
| Supply Voltage max| numeric | V    | — |
| GPIO Pins         | string  | —    | Count — e.g. "23" |
| ADC Channels      | string  | —    | Count + resolution, e.g. "10ch 12-bit" |
| UART              | string  | —    | Count |
| SPI               | string  | —    | Count |
| I2C               | string  | —    | Count |
| USB               | string  | —    | None / FS / HS / OTG |
| Package           | string  | —    | DIP-28, TQFP-44, LQFP-48, QFN-32 |

#### Logic ICs

| Parameter         | Type    | Unit | Notes |
|-------------------|---------|------|-------|
| Logic Family      | string  | —    | 74HC, 74HCT, 74AHC, 74LVC, TTL |
| Supply Voltage min| numeric | V    | — |
| Supply Voltage max| numeric | V    | — |
| Propagation Delay | numeric | s    | ns |
| Output Drive      | numeric | A    | mA — source/sink |
| Function          | string  | —    | 2-input NAND x4, D flip-flop x2, etc. |
| Package           | string  | —    | DIP-14, SOIC-14, TSSOP-14 |

#### Memory ICs

| Parameter         | Type    | Unit | Notes |
|-------------------|---------|------|-------|
| Capacity          | numeric | B    | kB, MB, GB |
| Interface         | string  | —    | SPI, I2C, Parallel, QSPI |
| Supply Voltage    | numeric | V    | — |
| Access Time / Speed| numeric| Hz  | MHz — clock speed |
| Organization      | string  | —    | e.g. 1M×8, 256K×8 |
| Endurance         | numeric | —    | Write cycles — e.g. 1000000 |
| Retention         | numeric | s    | Years |
| Package           | string  | —    | — |

#### Relays

| Parameter         | Type    | Unit | Notes |
|-------------------|---------|------|-------|
| Coil Voltage      | numeric | V    | DC or AC |
| Coil Resistance   | numeric | Ω    | — |
| Coil Current      | numeric | A    | mA |
| Contact Rating (V)| numeric | V    | Max switching voltage |
| Contact Rating (A)| numeric | A    | Max switching current |
| Contact Form      | string  | —    | SPDT, DPDT, SPST-NO, SPST-NC |
| Operate Time      | numeric | s    | ms |
| Release Time      | numeric | s    | ms |
| Package           | string  | —    | PCB, DIN rail, panel mount |

#### Connectors (Pin Headers)

| Parameter         | Type    | Unit | Notes |
|-------------------|---------|------|-------|
| Pin Count         | string  | —    | Total pin count |
| Pitch             | numeric | m    | mm — typically 2.54 or 2.0 mm |
| Rows              | string  | —    | Single, Dual |
| Gender            | string  | —    | Male (header), Female (socket) |
| Orientation       | string  | —    | Vertical, Right Angle |
| Current Rating    | numeric | A    | Per contact |
| Voltage Rating    | numeric | V    | — |
| Mating Cycles     | numeric | —    | Lifecycle |

#### Fuses

| Parameter         | Type    | Unit | Notes |
|-------------------|---------|------|-------|
| Current Rating    | numeric | A    | mA, A |
| Voltage Rating    | numeric | V    | — |
| Breaking Capacity | numeric | A    | kA — interrupt rating |
| Type              | string  | —    | Fast-Blow (F), Slow-Blow (T), SMD |
| Hold Current      | numeric | A    | For PTC resettable fuses |
| Trip Current      | numeric | A    | For PTC resettable fuses |
| Package           | string  | —    | 5×20mm, 6.3×32mm, 1206 SMD |

#### LEDs

| Parameter           | Type    | Unit | Notes |
|---------------------|---------|------|-------|
| Forward Voltage (Vf)| numeric | V    | At rated current |
| Forward Current (If)| numeric | A    | mA — nominal |
| Max Forward Current | numeric | A    | mA — peak |
| Wavelength / Color  | string  | —    | Red/Green/Blue/White/IR or nm value |
| Peak Wavelength     | numeric | m    | nm — for monochromatic |
| Luminous Intensity  | numeric | —    | mcd |
| Viewing Angle       | numeric | —    | degrees |
| Package             | string  | —    | 3mm, 5mm, 0402, 0603, 0805 |

#### Discrete Switches / Buttons

| Parameter         | Type    | Unit | Notes |
|-------------------|---------|------|-------|
| Contact Rating (A)| numeric | A    | mA, A |
| Contact Rating (V)| numeric | V    | — |
| Actuator Type     | string  | —    | Momentary / Latching |
| Operating Force   | numeric | —    | gf, N |
| Poles / Throws    | string  | —    | SPST, SPDT, DPDT |
| Mounting          | string  | —    | Through-hole, SMD, Panel |
| Body Size         | string  | —    | e.g. 6×6mm, 12×12mm |

#### Heatsinks

| Parameter             | Type    | Unit  | Notes |
|-----------------------|---------|-------|-------|
| Thermal Resistance    | numeric | °C/W  | Junction-to-ambient |
| Compatible Package    | string  | —     | TO-220, TO-247, etc. |
| Length                | numeric | m     | mm |
| Width                 | numeric | m     | mm |
| Height                | numeric | m     | mm |
| Material              | string  | —     | Aluminium, Copper |
| Finish                | string  | —     | Anodised, Black, Natural |

#### Fasteners (Screws)

| Parameter         | Type    | Unit | Notes |
|-------------------|---------|------|-------|
| Thread Size       | string  | —    | M2, M2.5, M3, M4, M5 |
| Length            | numeric | m    | mm |
| Head Type         | string  | —    | Pan, Countersunk, Button, Hex |
| Drive Type        | string  | —    | Phillips, Pozidriv, Torx, Hex |
| Material          | string  | —    | Stainless, Zinc-plated steel, Nylon |
| Thread Pitch      | numeric | m    | mm — standard vs fine |

#### PCBs (blank / prototype)

| Parameter         | Type    | Unit | Notes |
|-------------------|---------|------|-------|
| Layers            | string  | —    | 1, 2, 4, 6 |
| Width             | numeric | m    | mm |
| Height            | numeric | m    | mm |
| PCB Thickness     | numeric | m    | mm — typically 1.6 |
| Copper Thickness  | numeric | —    | oz — 1oz, 2oz |
| Surface Finish    | string  | —    | HASL, ENIG, OSP |
| Solder Mask Color | string  | —    | Green, Black, Red, Blue |
| Material          | string  | —    | FR4, Flex, Rogers, Aluminium |

---

## 5. Schema — Complete DDL

### 5.1 SI Prefixes (Part of PK3)

```sql
CREATE TABLE IF NOT EXISTS part_si_prefix (
    id       SERIAL PRIMARY KEY,
    name     VARCHAR(20)  NOT NULL UNIQUE,
    symbol   VARCHAR(8)   NOT NULL,
    exponent INTEGER      NOT NULL,
    base     INTEGER      NOT NULL DEFAULT 10
);

INSERT INTO part_si_prefix (name, symbol, exponent) VALUES
    ('pico',  'p',  -12),
    ('nano',  'n',   -9),
    ('micro', 'µ',   -6),
    ('milli', 'm',   -3),
    ('none',  '',     0),
    ('kilo',  'k',    3),
    ('mega',  'M',    6),
    ('giga',  'G',    9),
    ('tera',  'T',   12)
ON CONFLICT (name) DO NOTHING;
```

### 5.2 Part Units (Part of PK3)

```sql
CREATE TABLE IF NOT EXISTS part_unit (
    id          SERIAL PRIMARY KEY,
    name        VARCHAR(50)  NOT NULL UNIQUE,
    symbol      VARCHAR(20)  NOT NULL,
    description TEXT,
    create_date TIMESTAMP DEFAULT now(),
    write_date  TIMESTAMP DEFAULT now()
);
-- Seeds: see section 4.3
```

### 5.3 Footprint Category (PK2)

```sql
CREATE TABLE IF NOT EXISTS part_footprint_category (
    id          SERIAL PRIMARY KEY,
    name        VARCHAR(100) NOT NULL,
    parent_id   INTEGER REFERENCES part_footprint_category(id) ON DELETE SET NULL,
    description TEXT,
    create_date TIMESTAMP DEFAULT now(),
    write_date  TIMESTAMP DEFAULT now()
);
```

### 5.4 Footprint (PK2)

```sql
CREATE TABLE IF NOT EXISTS part_footprint (
    id          SERIAL PRIMARY KEY,
    name        VARCHAR(100) NOT NULL UNIQUE,
    description TEXT,
    category_id INTEGER REFERENCES part_footprint_category(id) ON DELETE SET NULL,
    image       TEXT,           -- base64 SVG or PNG of pad layout
    active      BOOLEAN NOT NULL DEFAULT TRUE,
    create_date TIMESTAMP DEFAULT now(),
    write_date  TIMESTAMP DEFAULT now()
);

ALTER TABLE product_product
    ADD COLUMN IF NOT EXISTS footprint_id INTEGER REFERENCES part_footprint(id) ON DELETE SET NULL;
```

### 5.5 Part Parameter (PK3)

```sql
CREATE TABLE IF NOT EXISTS part_parameter (
    id               SERIAL PRIMARY KEY,
    product_id       INTEGER NOT NULL REFERENCES product_product(id) ON DELETE CASCADE,
    name             VARCHAR(100)  NOT NULL,
    description      VARCHAR(255),
    value_type       VARCHAR(10)   NOT NULL DEFAULT 'numeric',  -- 'numeric' | 'string'

    -- numeric fields
    value            DOUBLE PRECISION,
    normalized_value DOUBLE PRECISION,       -- value × base^exponent (computed on write)
    min_value        DOUBLE PRECISION,
    max_value        DOUBLE PRECISION,
    normalized_min   DOUBLE PRECISION,
    normalized_max   DOUBLE PRECISION,
    si_prefix_id     INTEGER REFERENCES part_si_prefix(id),
    min_si_prefix_id INTEGER REFERENCES part_si_prefix(id),
    max_si_prefix_id INTEGER REFERENCES part_si_prefix(id),
    unit_id          INTEGER REFERENCES part_unit(id),

    -- string field
    string_value     VARCHAR(255),

    create_date      TIMESTAMP DEFAULT now(),
    write_date       TIMESTAMP DEFAULT now()
);

CREATE INDEX IF NOT EXISTS part_parameter_product_idx ON part_parameter(product_id);
CREATE INDEX IF NOT EXISTS part_parameter_name_idx    ON part_parameter(name);
CREATE INDEX IF NOT EXISTS part_parameter_nval_idx    ON part_parameter(normalized_value);
```

`normalized_value` must be recomputed whenever `value` or `si_prefix_id` changes.
The C++ handler does: `normalizedValue = value * pow(base, exponent)` using the joined SI prefix row.

### 5.6 Manufacturer Part Numbers (PK4)

```sql
CREATE TABLE IF NOT EXISTS part_manufacturer_info (
    id          SERIAL PRIMARY KEY,
    product_id  INTEGER NOT NULL REFERENCES product_product(id) ON DELETE CASCADE,
    partner_id  INTEGER NOT NULL REFERENCES res_partner(id),
    part_number VARCHAR(100),
    sequence    INTEGER NOT NULL DEFAULT 10,
    create_date TIMESTAMP DEFAULT now(),
    write_date  TIMESTAMP DEFAULT now()
);

CREATE INDEX IF NOT EXISTS part_mfr_product_idx ON part_manufacturer_info(product_id);
```

### 5.7 Supplier Info Extension (PK5 — folded into A3b)

```sql
ALTER TABLE product_supplierinfo
    ADD COLUMN IF NOT EXISTS order_number   VARCHAR(100),
    ADD COLUMN IF NOT EXISTS packaging_unit INTEGER NOT NULL DEFAULT 1;
```

### 5.8 Product Extensions (PK6)

```sql
ALTER TABLE product_product
    ADD COLUMN IF NOT EXISTS min_stock_qty NUMERIC(16,4) NOT NULL DEFAULT 0,
    ADD COLUMN IF NOT EXISTS part_status   VARCHAR(20)   NOT NULL DEFAULT 'new',
    ADD COLUMN IF NOT EXISTS needs_review  BOOLEAN       NOT NULL DEFAULT FALSE,
    ADD COLUMN IF NOT EXISTS is_meta_part  BOOLEAN       NOT NULL DEFAULT FALSE;
```

Valid `part_status` values: `new` | `used` | `broken` | `unknown`

### 5.9 Attachments (PK7)

```sql
CREATE TABLE IF NOT EXISTS ir_attachment (
    id          SERIAL PRIMARY KEY,
    name        VARCHAR(255)  NOT NULL,
    res_model   VARCHAR(100)  NOT NULL,
    res_id      INTEGER       NOT NULL,
    file_name   VARCHAR(255),
    mimetype    VARCHAR(100),
    file_size   INTEGER,
    db_datas    TEXT,               -- base64-encoded content
    description TEXT,
    is_public   BOOLEAN NOT NULL DEFAULT FALSE,
    create_uid  INTEGER REFERENCES res_users(id),
    create_date TIMESTAMP DEFAULT now(),
    write_date  TIMESTAMP DEFAULT now()
);

CREATE INDEX IF NOT EXISTS ir_attachment_res_idx ON ir_attachment(res_model, res_id);
```

### 5.10 Meta-part Criteria (PK8)

```sql
CREATE TABLE IF NOT EXISTS part_meta_criteria (
    id               SERIAL PRIMARY KEY,
    meta_product_id  INTEGER NOT NULL REFERENCES product_product(id) ON DELETE CASCADE,
    parameter_name   VARCHAR(100) NOT NULL,    -- must match part_parameter.name
    value_type       VARCHAR(10)  NOT NULL DEFAULT 'numeric',

    -- numeric criterion
    operator         VARCHAR(4),               -- '=', '<', '>', '<=', '>=', '!='
    value            DOUBLE PRECISION,
    normalized_value DOUBLE PRECISION,         -- pre-computed for fast comparison
    si_prefix_id     INTEGER REFERENCES part_si_prefix(id),
    unit_id          INTEGER REFERENCES part_unit(id),

    -- string criterion
    string_operator  VARCHAR(10),              -- '=' | 'like' | 'in'
    string_value     VARCHAR(255),

    sequence         INTEGER NOT NULL DEFAULT 10,
    create_date      TIMESTAMP DEFAULT now(),
    write_date       TIMESTAMP DEFAULT now()
);

CREATE INDEX IF NOT EXISTS part_meta_product_idx ON part_meta_criteria(meta_product_id);
```

---

## 6. JSON-RPC API Design

### 6.1 Model names registered in `checkModelAccess_()`

```
part.footprint.category    → part_footprint_category
part.footprint             → part_footprint
part.si.prefix             → part_si_prefix          (read-only)
part.unit                  → part_unit
part.parameter             → part_parameter
part.manufacturer.info     → part_manufacturer_info
part.meta.criteria         → part_meta_criteria
ir.attachment              → ir_attachment
```

### 6.2 Standard CRUD

All models follow the existing JSON-RPC dispatch pattern:

```json
{ "method": "execute_kw", "args": ["part.parameter", "search_read",
  [[["product_id","=",42]]], {"fields":["name","value","unit_id","si_prefix_id"]}] }
```

```json
{ "method": "execute_kw", "args": ["part.parameter", "create",
  [{"product_id":42,"name":"Resistance","value_type":"numeric",
    "value":10,"si_prefix_id":6,"unit_id":1}], {}] }
```

### 6.3 Parameter-based Part Search

Find all N-channel MOSFETs with Vds ≥ 30 V and Rds(on) ≤ 0.050 Ω:

```json
{
  "method": "execute_kw",
  "args": ["product.product", "search_read",
    [[
      ["categ_id.name", "=", "N-Channel MOSFET"],
      ["parameter_ids.name", "=", "Vds max"],
      ["parameter_ids.normalized_value", ">=", 30],
      ["parameter_ids.name", "=", "Rds(on)"],
      ["parameter_ids.normalized_value", "<=", 0.050]
    ]],
    {"fields": ["name","default_code","footprint_id","min_stock_qty"]}
  ]
}
```

The C++ `handleSearchRead()` translates `parameter_ids.X` domain leaves into a JOIN on
`part_parameter` with appropriate WHERE clauses.

**Supported operators on `normalized_value`:**
`=`, `!=`, `<`, `>`, `<=`, `>=`, `in`

**Supported operators on `string_value`:**
`=`, `!=`, `like`, `ilike`, `in`

### 6.4 Get Distinct Parameter Values (for filter UI)

A custom method `get_parameter_values` on `part.parameter` returns all distinct values
for a named parameter (populates filter dropdowns):

```json
{
  "method": "execute_kw",
  "args": ["part.parameter", "get_parameter_values",
    [{"name": "Package / Case", "categ_id": 12}], {}]
}
```

Response:
```json
{"result": ["0402", "0603", "0805", "1206", "SOT-23", "TO-92"]}
```

### 6.5 Meta-part Stock Resolution

When `product.product` `search_read` includes a meta-part (is_meta_part=true), the backend
appends a resolved `matched_qty` field showing aggregated stock from matching real parts.
This happens transparently in `serializeFields()`:

```
if (product.is_meta_part):
    matched_ids = resolve_meta_part(product.id, txn)
    matched_qty = SUM(qty_on_hand WHERE product_id IN matched_ids)
    result["meta_matched_qty"] = matched_qty
    result["meta_matched_count"] = matched_ids.size()
```

The client can also call `get_meta_matches` to see the list of matching parts:

```json
{
  "method": "execute_kw",
  "args": ["product.product", "get_meta_matches", [[5]], {}]
}
```

Response:
```json
{
  "result": [
    {"id": 12, "name": "100R 0603 1% Yageo", "qty_on_hand": 500},
    {"id": 34, "name": "100R 0603 5% Vishay", "qty_on_hand": 200}
  ]
}
```

### 6.6 Attachment Upload (PK7) — JSON-RPC

```json
{
  "method": "execute_kw",
  "args": ["ir.attachment", "create", [{
    "name": "ATmega328P Datasheet",
    "res_model": "product.product",
    "res_id": 42,
    "file_name": "ATmega328P-datasheet.pdf",
    "mimetype": "application/pdf",
    "db_datas": "<base64-encoded PDF content>",
    "description": "Full datasheet rev. G"
  }], {}]
}
```

**Attachment Download (only non-JSON-RPC route added):**

```
GET /web/content/<attachment_id>
GET /web/content/<attachment_id>/<suggested_filename>
```

- Requires valid session cookie (same auth as JSON-RPC)
- Decodes `db_datas` from base64 and streams with correct `Content-Type`
- Used by `<img src>`, `<embed src>`, and download links in the frontend
- No business logic — purely a binary delivery helper

---

## 7. Meta-part Algorithm (PK8)

### 7.1 Concept

A meta-part is a virtual placeholder in the parts catalog. It has `is_meta_part = true`
and a set of criteria in `part_meta_criteria`. When displayed, its effective stock level
is the **sum** of all real (non-meta) parts that match **all** of its criteria simultaneously.

**Use case example:**
- Meta-part: "100 Ohm Resistor (any package)"
  - Criterion 1: Resistance = 100 Ω (normalized: 100.0)
- Meta-part: "100 Ohm 0805 1% Resistor"
  - Criterion 1: Resistance = 100 Ω (normalized: 100.0)
  - Criterion 2: Package = "0805"
  - Criterion 3: Tolerance = 1%

This lets a BOM reference a generic part and have stock automatically drawn from
whichever specific part is available.

### 7.2 Matching SQL (C++ implementation)

```cpp
// resolve_meta_part(meta_product_id, txn) → vector<int> matched_product_ids
//
// Strategy: for each criterion, collect the set of matching product IDs,
// then intersect all sets (AND semantics across criteria).

auto criteria = txn.exec(
    "SELECT * FROM part_meta_criteria WHERE meta_product_id=$1 ORDER BY sequence",
    pqxx::params{metaProductId});

std::optional<std::set<int>> result;

for (const auto& c : criteria) {
    std::set<int> matched;

    if (safeStr(c["value_type"]) == "numeric") {
        std::string op = safeStr(c["operator"]);  // '=', '<', '>', '<=', '>=', '!='
        double nval = c["normalized_value"].as<double>(0.0);

        // Build: SELECT DISTINCT product_id FROM part_parameter
        //        WHERE name = $1 AND normalized_value {op} $2
        std::string sql =
            "SELECT DISTINCT product_id FROM part_parameter "
            "WHERE name=$1 AND value_type='numeric' "
            "AND normalized_value " + op + " $2 "
            "AND product_id IN (SELECT id FROM product_product WHERE is_meta_part=FALSE)";

        auto rows = txn.exec(sql, pqxx::params{safeStr(c["parameter_name"]), nval});
        for (const auto& r : rows) matched.insert(r["product_id"].as<int>());

    } else {
        // string criterion
        std::string sop  = safeStr(c["string_operator"]);  // '=' | 'like' | 'ilike'
        std::string sval = safeStr(c["string_value"]);

        std::string sqlOp = (sop == "like")  ? "LIKE"  :
                            (sop == "ilike") ? "ILIKE" : "=";
        std::string sql =
            "SELECT DISTINCT product_id FROM part_parameter "
            "WHERE name=$1 AND value_type='string' "
            "AND string_value " + sqlOp + " $2 "
            "AND product_id IN (SELECT id FROM product_product WHERE is_meta_part=FALSE)";

        auto rows = txn.exec(sql, pqxx::params{safeStr(c["parameter_name"]), sval});
        for (const auto& r : rows) matched.insert(r["product_id"].as<int>());
    }

    // Intersect
    if (!result.has_value()) {
        result = matched;
    } else {
        std::set<int> intersection;
        std::set_intersection(result->begin(), result->end(),
                              matched.begin(), matched.end(),
                              std::inserter(intersection, intersection.begin()));
        result = intersection;
    }

    if (result->empty()) break;  // short-circuit: no matches possible
}

return result.has_value() ? std::vector<int>(result->begin(), result->end())
                          : std::vector<int>{};
```

### 7.3 Stock Aggregation

```cpp
// After resolve_meta_part() returns matched_ids:
double totalQty = 0.0;
if (!matchedIds.empty()) {
    // Build IN clause safely using placeholders
    std::string inClause;
    pqxx::params p;
    int i = 1;
    for (int id : matchedIds) {
        if (i > 1) inClause += ",";
        inClause += "$" + std::to_string(i++);
        p.append(id);
    }
    auto srows = txn.exec(
        "SELECT COALESCE(SUM(sq.quantity),0) AS total "
        "FROM stock_quant sq "
        "JOIN stock_location sl ON sl.id = sq.location_id "
        "WHERE sl.usage = 'internal' AND sq.product_id IN (" + inClause + ")", p);
    totalQty = srows[0]["total"].as<double>(0.0);
}
result["meta_matched_qty"]   = totalQty;
result["meta_matched_count"] = (int)matchedIds.size();
```

---

## 8. Frontend — Parts Catalog UI

### 8.1 Parts Browser

The main parts view combines category tree + parameter filters + results grid.

```
┌─────────────────────────────────────────────────────────────────────────────┐
│  PARTS CATALOG                                              [+ New Part]    │
├──────────────────┬──────────────────────────────────────────────────────────┤
│ CATEGORIES       │  Filters                                                 │
│ ─────────────    │  ┌────────────────────────────────────────────────────┐  │
│ ▼ Electronics    │  │ Resistance: [    ] – [    ] Ω/kΩ/MΩ [Apply]       │  │
│   ▼ Passives     │  │ Package:    [All ▾]                                │  │
│     ▶ Resistors  │  │ Tolerance:  [All ▾]          [Clear Filters]       │  │
│     ▶ Capacitors │  └────────────────────────────────────────────────────┘  │
│     ▶ Inductors  │                                                          │
│   ▶ Semiconductors  Part No.    Name               Footprint  Stock  Status│
│   ▶ Connectors   │  ──────────  ─────────────────  ─────────  ─────  ─────│
│   ▶ LEDs         │  R-10K-0603  10kΩ 0603 1% 0.1W  0603-SMD  1240   new  │
│                  │  R-10K-0805  10kΩ 0805 1% 0.1W  0805-SMD   380   new  │
│                  │  🔗 Meta     10kΩ Any Package    —         1620   meta │
│                  │                                                          │
└──────────────────┴──────────────────────────────────────────────────────────┘
```

- Meta-parts shown with 🔗 icon; stock = `meta_matched_qty`
- Clicking a meta-part shows "Matched Parts" sub-panel with individual stocks
- Filter panel is dynamically built from `get_parameter_values` for the selected category

### 8.2 Product Form — New Tabs

```
[ General ] [ Parameters ] [ Manufacturers ] [ Documents ] [ Inventory ] [ Variants ]
```

**Parameters tab** — inline editable table (same pattern as BOM lines):
```
[+ Add Parameter]
Name          Value Type  Value  Prefix  Unit  Range (min–max)  String Value
────────────  ──────────  ─────  ──────  ────  ───────────────  ────────────
Resistance    Numeric     10     k (kΩ)  Ω     —                —
Package/Case  String      —      —       —     —                0805
Tolerance     Numeric     1      —       %     —                —
[+ Add]
```

**Manufacturers tab**:
```
Manufacturer (partner)      MPN
────────────────────────    ────────────────
Yageo                       RC0603FR-0710KL
Vishay                      CRCW060310K0FKEA
[+ Add]
```

**Documents tab** (PK7):
```
[Upload File]
Name                         Type   Size    Actions
───────────────────────────  ─────  ──────  ───────────────
ATmega328P-datasheet.pdf     PDF    2.3 MB  [View] [Delete]
pinout-diagram.png           Image  145 KB  [View] [Delete]
```

**Meta-part Criteria tab** (visible when is_meta_part = true):
```
[ ☑ This is a Meta-part ]

Matching Criteria (ALL must match)
Parameter Name      Op   Value  Prefix  Unit  String Op  String Value
──────────────────  ───  ─────  ──────  ────  ─────────  ────────────
Resistance          =    10     k       Ω     —          —
Package/Case        —    —      —       —     =          0805
[+ Add Criterion]

Matched Parts: 3    Total Stock: 850 units
┌─────────────────────────────────────────────────────┐
│ RC0603FR-0710KL   Yageo 10k 0603 1%    500 in stock │
│ CRCW060310K0FKEA  Vishay 10k 0603 1%   200 in stock │
│ ERJ-3EKF1002V     Panasonic 10k 0603   150 in stock │
└─────────────────────────────────────────────────────┘
```

---

## 9. IR Menu / Action IDs

Continuing from existing ID allocations:

```
ir_ui_menu:
  110   Inventory → Configuration → Footprints         (PK2)
  111   Inventory → Configuration → Footprint Categories (PK2)
  112   Inventory → Configuration → Part Units         (PK3)
  113   Inventory → Configuration → Part SI Prefixes   (PK3, read-only)
  114   Inventory → Parts Catalog                      (PK3 — main browse view)
  115   Inventory → Parts Catalog → Meta-parts         (PK8)

ir_act_window:
  37    Footprints                                      (PK2)
  38    Footprint Categories                            (PK2)
  39    Part Units                                      (PK3)
  40    Part SI Prefixes                                (PK3)
  41    Parts Catalog (all parts)                       (PK3)
  42    Meta-parts                                      (PK8)
```

---

## 10. Implementation Checklist

### PK1 — Category Tree UI

- [ ] Backend: add `category_path` recursive CTE in `ProductCategoryViewModel::serializeFields()`
- [ ] Frontend: `ProductCategoryListView` — show `category_path` column
- [ ] Frontend: product form category picklist — show full path in option text
- [ ] Frontend: category form — add `parent_id` Many2one field

### PK2 — Footprints

- [ ] `ensureSchema_()`: CREATE `part_footprint_category`, `part_footprint`; ALTER `product_product`
- [ ] `PartFootprintCategoryViewModel`: CRUD, parent_id path helper
- [ ] `PartFootprintViewModel`: CRUD, serialize category_id name
- [ ] `checkModelAccess_()`: add `part.footprint.category`, `part.footprint`
- [ ] Frontend: Footprint Category list + form
- [ ] Frontend: Footprint list + form (with image upload)
- [ ] Frontend: Product form — Footprint Many2one on General tab
- [ ] IR: menus 110, 111 and actions 37, 38

### PK3 — Parameters & SI Units

- [ ] `ensureSchema_()`: CREATE `part_si_prefix`, `part_unit`, `part_parameter`; insert seeds
- [ ] `PartSiPrefixViewModel`: read-only, no create/write/unlink
- [ ] `PartUnitViewModel`: CRUD
- [ ] `PartParameterViewModel`: CRUD; compute `normalized_value` on create/write
- [ ] `ProductProductViewModel`: return `parameter_ids` array in `serializeFields()`
- [ ] `ProductProductViewModel::handleWrite()`: handle `parameter_ids` One2many commands
- [ ] `ProductProductViewModel::handleSearchRead()`: translate `parameter_ids.X` domain leaves
- [ ] Custom `get_parameter_values` method for filter UI
- [ ] `checkModelAccess_()`: add the three new models
- [ ] Frontend: Parameters tab on product form (inline editable)
- [ ] Frontend: Part Units list + form
- [ ] IR: menus 112, 113, 114 and actions 39, 40, 41

### PK4 — Manufacturer MPN

- [ ] `ensureSchema_()`: CREATE `part_manufacturer_info`
- [ ] `PartManufacturerInfoViewModel`: CRUD; serialize partner name
- [ ] `ProductProductViewModel`: return `manufacturer_ids` array; handle One2many commands
- [ ] `checkModelAccess_()`: add `part.manufacturer.info`
- [ ] Frontend: Manufacturers tab on product form

### PK5 — Enhanced Supplier Info (fold into A3b)

- [ ] `ensureSchema_()`: ALTER `product_supplierinfo` ADD `order_number`, `packaging_unit`
- [ ] `ProductSupplierinfoViewModel`: expose new fields
- [ ] Frontend: A3b vendor pricelist table — add Order Ref. and Pack Qty columns

### PK6 — Min Stock & Part Status

- [ ] `ensureSchema_()`: ALTER `product_product` ADD `min_stock_qty`, `part_status`, `needs_review`, `is_meta_part`
- [ ] `ProductProductViewModel::serializeFields()`: expose new fields
- [ ] `ProductProductViewModel::handleWrite()`: accept new fields
- [ ] Frontend: Product form Inventory tab — Stock Control section
- [ ] Frontend: Product list — low-stock row highlight when `qty_on_hand < min_stock_qty`

### PK7 — Attachments

- [ ] `ensureSchema_()`: CREATE `ir_attachment`
- [ ] `IrAttachmentViewModel`: CRUD; validate res_model/res_id; compute `file_size`
- [ ] `checkModelAccess_()`: add `ir.attachment`
- [ ] `ProductProductViewModel::serializeFields()`: return `attachment_ids` array
- [ ] GET `/web/content/<id>` route in `ReportModule` or new `AttachmentModule` — decode base64, stream with Content-Type
- [ ] Frontend: Documents tab on product form (upload + list)

### PK8 — Meta-parts

- [ ] `ensureSchema_()`: CREATE `part_meta_criteria`
- [ ] `PartMetaCriteriaViewModel`: CRUD; compute `normalized_value` on create/write
- [ ] `checkModelAccess_()`: add `part.meta.criteria`
- [ ] `ProductProductViewModel::serializeFields()`: for meta-parts, call `resolve_meta_part()`, return `meta_matched_qty` + `meta_matched_count`
- [ ] Custom `get_meta_matches` method on `product.product`
- [ ] `ProductProductViewModel::handleSearchRead()`: include meta-part aggregate in result
- [ ] Frontend: Meta-part Criteria tab (visible when `is_meta_part=true`)
- [ ] Frontend: Parts Catalog browser — meta-part indicator, matched parts sub-panel
- [ ] Frontend: Parts Catalog filter panel with dynamic parameter filters
- [ ] IR: menus 115, action 42

---

## 11. Table Count Projection

| After Phase | New Tables | Running Total |
|-------------|-----------|---------------|
| Current state | — | ~45 |
| A3b + PK5 | 1 (`product_supplierinfo`) | 46 |
| PK6 | 0 (4 columns on `product_product`) | 46 |
| PK1 | 0 (frontend only) | 46 |
| PK4 | 1 (`part_manufacturer_info`) | 47 |
| PK2 | 2 (`part_footprint_category`, `part_footprint`) | 49 |
| PK3 | 3 (`part_si_prefix`, `part_unit`, `part_parameter`) | 52 |
| PK7 | 1 (`ir_attachment`) | 53 |
| PK8 | 1 (`part_meta_criteria`) | 54 |

---

## 12. Notes on Future gRPC Migration

When this system eventually migrates to gRPC, the proto definitions would follow naturally
from the JSON-RPC models. For example, a search request currently expressed as:

```json
[["parameter_ids.name","=","Resistance"],["parameter_ids.normalized_value",">=",1000]]
```

maps to:

```proto
message Filter {
  string field    = 1;
  string operator = 2;   // "=", ">", ">=", etc.
  oneof value {
    string string_val = 3;
    double double_val = 4;
    int64  int_val    = 5;
  }
}

message SearchPartsRequest {
  repeated Filter filters = 1;
  repeated string fields  = 2;
  int32 limit             = 3;
  int32 offset            = 4;
}

rpc SearchParts(SearchPartsRequest) returns (stream PartRecord);
```

The C++ query-building logic (`handleSearchRead`, `resolve_meta_part`) is fully reusable —
only the serialisation layer changes. The domain filter format is the migration-safe interface
boundary between frontend and backend.

For real-time stock updates, an additional streaming RPC would be added:

```proto
rpc WatchStock(WatchStockRequest) returns (stream StockEvent);
```

This is the only place where HTTP/3 QUIC multiplexing provides a tangible benefit (many
concurrent subscription streams without head-of-line blocking). ERP data operations
(CRUD, search) are unary and do not need QUIC.
