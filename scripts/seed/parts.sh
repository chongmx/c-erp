#!/usr/bin/env bash
#
# scripts/seed/parts.sh — a realistic electronics catalogue for the faceted
# browser (docs/098).
#
# The parts browser is only meaningful against a catalogue with enough shape to
# filter: several manufacturers, several packages, a real surface-mount /
# through-hole split, and parameters that actually spread across a range. Four
# products cannot demonstrate faceting.
#
# Everything it creates carries the default_code prefix 'DP-', and the script
# deletes that set before reinserting, so it is idempotent and removable:
#
#   ./scripts/seed.sh parts            # seed / re-seed
#   ./scripts/seed.sh parts --clean    # remove, leave the catalogue empty
#
set -euo pipefail
# Repo root by walking up for CMakeLists.txt, so this works from any directory
# and survives being nested one folder deeper.
R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
while [ "$R" != "/" ] && [ ! -f "$R/CMakeLists.txt" ]; do R="$(dirname "$R")"; done
cd "$R" || exit 1

PGHOST="${PGHOST:-localhost}"
PGUSER="${PGUSER:-odoo}"
PGDATABASE="${PGDATABASE:-odoo}"
export PGPASSWORD="${PGPASSWORD:-odoo}"

psql_() { psql -h "$PGHOST" -U "$PGUSER" -d "$PGDATABASE" -v ON_ERROR_STOP=1 "$@"; }

if [[ "${1:-}" == "--clean" ]]; then
    psql_ -qc "DELETE FROM product_product WHERE default_code LIKE 'DP-%'"
    echo "Demo parts removed."
    exit 0
fi

psql_ -q <<'SQL'
BEGIN;

-- part_parameter and part_manufacturer_info both cascade from product_product.
DELETE FROM product_product WHERE default_code LIKE 'DP-%';

-- to_char with FM strips trailing zeros but leaves the decimal point behind,
-- so 1.0 formats as "1." and the catalogue fills up with "1.kΩ" and "±5.%".
CREATE OR REPLACE FUNCTION pg_temp.num(v numeric, fmt text) RETURNS text AS $fn$
    SELECT rtrim(btrim(to_char($1, $2)), '.');
$fn$ LANGUAGE sql IMMUTABLE;

DO $seed$
DECLARE
    v_uom        int := (SELECT id FROM uom_uom ORDER BY id LIMIT 1);
    v_cat_smd    int := (SELECT id FROM product_category WHERE name = 'SMD Resistors' LIMIT 1);
    v_cat_tht    int := (SELECT id FROM product_category WHERE name = 'Through-Hole Resistors' LIMIT 1);
    v_cat_mlcc   int := (SELECT id FROM product_category WHERE name = 'Ceramic Capacitors (MLCC)' LIMIT 1);

    u_ohm        int := (SELECT id FROM part_unit WHERE symbol = 'Ω');
    u_farad      int := (SELECT id FROM part_unit WHERE symbol = 'F');
    u_volt       int := (SELECT id FROM part_unit WHERE symbol = 'V');
    u_watt       int := (SELECT id FROM part_unit WHERE symbol = 'W');
    u_pct        int := (SELECT id FROM part_unit WHERE symbol = '%');
    u_ppm        int := (SELECT id FROM part_unit WHERE symbol = 'ppm');
    -- '%' is the BASE of the ratio kind (factor 1), so a 5% tolerance stores a
    -- value_base of 5 — not 0.05. Getting this wrong makes the Tolerance facet
    -- default to ppm and report a span of "1e+3 – 5e+4".
    f_pct        numeric := (SELECT factor FROM part_unit WHERE symbol = '%');
    f_ppm        numeric := (SELECT factor FROM part_unit WHERE symbol = 'ppm');

    r_mfrs       text[] := ARRAY['YAGEO','Vishay','KOA Speer','Panasonic','Bourns','ROHM'];
    c_mfrs       text[] := ARRAY['Murata','Samsung Electro-Mechanics','TDK','YAGEO','KEMET'];
    r_types      text[] := ARRAY['Thick Film','Thin Film','Metal Film'];
    dielectrics  text[] := ARRAY['C0G/NP0','X7R','X5R','Y5V'];
    smd_pkgs     text[] := ARRAY['0402','0603','0805','1206'];
    c_pkgs       text[] := ARRAY['0402','0603','0805','1206'];
    e12          numeric[] := ARRAY[1.0,1.2,1.5,1.8,2.2,2.7,3.3,3.9,4.7,5.6,6.8,8.2];
    e6           numeric[] := ARRAY[1.0,1.5,2.2,3.3,4.7,6.8];
    tol_choices  numeric[] := ARRAY[0.1,0.5,1,5];
    pwr_choices  numeric[] := ARRAY[0.0625,0.1,0.125,0.25,0.5];
    volt_choices numeric[] := ARRAY[16,25,50,100];

    dec_i    int;  val_i   int;  pkg_i int;  k int := 0;
    v_val    numeric;  v_disp text;  v_pkg text;  v_mfr text;
    v_tol    numeric;  v_pwr numeric;  v_type text;  v_tc numeric;
    v_diel   text;  v_volt numeric;  v_suffix text;  v_ohm text;
    v_tht    boolean;  v_mount text;  v_cat int;
    v_pid    int;  v_fid int;  v_mid int;  v_code text;  v_mpn text;
BEGIN
    IF v_uom IS NULL THEN RAISE EXCEPTION 'no uom_uom rows — is the schema initialised?'; END IF;
    IF v_cat_smd IS NULL OR v_cat_mlcc IS NULL THEN
        RAISE EXCEPTION 'expected part categories are missing — start the server once to seed them';
    END IF;
    IF u_ohm IS NULL THEN RAISE EXCEPTION 'part_unit is empty — start the server once to seed units'; END IF;

    -- Manufacturers are partners; create the ones we reference.
    FOREACH v_mfr IN ARRAY (r_mfrs || c_mfrs) LOOP
        IF NOT EXISTS (SELECT 1 FROM res_partner WHERE name = v_mfr) THEN
            INSERT INTO res_partner (name, active) VALUES (v_mfr, true);
        END IF;
    END LOOP;

    -- ---------- resistors: mostly SMD chip, some axial through-hole ----------
    FOR dec_i IN 1..5 LOOP                       -- 10Ω .. 1MΩ
      FOR val_i IN 1..array_length(e12,1) LOOP
        v_val := e12[val_i] * power(10::numeric, dec_i);

        FOR pkg_i IN 1..2 LOOP
          k := k + 1;

          -- Every fifth part is genuinely through-hole, so Package, Mounting
          -- and Category agree with each other instead of listing an 0805 chip
          -- resistor under "Through-Hole".
          v_tht   := (k % 5 = 0);
          v_pkg   := CASE WHEN v_tht THEN 'Axial'
                          ELSE smd_pkgs[1 + ((k + dec_i) % array_length(smd_pkgs,1))] END;
          v_mount := CASE WHEN v_tht THEN 'Through Hole' ELSE 'Surface Mount' END;
          v_cat   := CASE WHEN v_tht THEN COALESCE(v_cat_tht, v_cat_smd) ELSE v_cat_smd END;

          v_mfr  := r_mfrs[1 + (k % array_length(r_mfrs,1))];
          v_tol  := tol_choices[1 + (k % array_length(tol_choices,1))];
          v_pwr  := CASE WHEN v_tht THEN (ARRAY[0.25,0.5,1])[1 + (k % 3)]
                         ELSE pwr_choices[1 + (k % array_length(pwr_choices,1))] END;
          v_type := r_types[1 + (k % array_length(r_types,1))];
          v_tc   := (ARRAY[25,50,100,200])[1 + (k % 4)];

          -- Display notation deliberately alternates between R-notation ("4k7")
          -- and a plain scaled number ("4.7k"). Both must be found by the same
          -- range query — that is the whole point of value_base.
          IF v_val >= 1000000 THEN
              v_disp := pg_temp.num(v_val/1000000, 'FM999999.999') || 'M';
          ELSIF v_val >= 1000 THEN
              v_disp := pg_temp.num(v_val/1000, 'FM999999.999') || 'k';
          ELSE
              v_disp := pg_temp.num(v_val, 'FM999999.999') || 'R';
          END IF;
          -- R-notation moves the suffix into the decimal point's place:
          -- 4.7k -> 4k7, 1.2M -> 1M2. Values with no decimal are already there.
          IF k % 2 = 0 AND position('.' in v_disp) > 0 THEN
              v_suffix := right(v_disp, 1);
              v_disp   := replace(left(v_disp, length(v_disp) - 1), '.', v_suffix);
          END IF;
          -- A trailing R already means ohms ("470R"); don't write "470RΩ".
          v_ohm := CASE WHEN right(v_disp, 1) = 'R'
                        THEN left(v_disp, length(v_disp) - 1) || 'Ω'
                        ELSE v_disp || 'Ω' END;

          v_code := 'DP-R-' || v_pkg || '-' || k;
          v_mpn  := CASE v_mfr
                      WHEN 'YAGEO'      THEN 'RC' || v_pkg || 'FR-07' || k || 'L'
                      WHEN 'Vishay'     THEN 'CRCW' || v_pkg || 'F' || k || 'T'
                      WHEN 'KOA Speer'  THEN 'RK73H' || v_pkg || 'TTD' || k
                      WHEN 'Panasonic'  THEN 'ERJ-' || v_pkg || 'D' || k || 'V'
                      WHEN 'Bourns'     THEN 'CR' || v_pkg || '-FX-' || k || 'ELF'
                      ELSE                   'MCR' || v_pkg || 'ZPF' || k
                    END;

          SELECT id INTO v_fid FROM part_footprint WHERE name = v_pkg;

          INSERT INTO product_product
              (name, default_code, description, type, categ_id, uom_id, uom_po_id,
               list_price, standard_price, qty_available, footprint_id, active,
               sale_ok, purchase_ok, company_id)
          VALUES
              (v_ohm || ' ±' || pg_temp.num(v_tol,'FM990.9') || '% ' || v_pkg || ' ' || v_type || ' Resistor',
               v_code,
               v_type || ' resistor, ' || v_pkg || ', ' || pg_temp.num(v_pwr,'FM0.9999') || 'W, ±' ||
               pg_temp.num(v_tc,'FM999') || 'ppm/°C',
               'product', v_cat, v_uom, v_uom,
               ((2 + (k % 40))::bigint) * 1000,          -- micros: $0.002 .. $0.042
               ((1 + (k % 20))::bigint) * 1000,
               (CASE WHEN k % 11 = 0 THEN 0 ELSE (500 + k * 137) END)::bigint * 1000000,
               v_fid, true, true, true, NULL)
          RETURNING id INTO v_pid;

          INSERT INTO part_parameter (product_id, name, value_numeric, unit_id, value_text, value_base, quantity_kind)
          VALUES (v_pid, 'Resistance', v_val, u_ohm, v_ohm, v_val, 'resistance'),
                 (v_pid, 'Tolerance',  v_tol, u_pct,
                  '±' || pg_temp.num(v_tol,'FM990.9') || '%', v_tol * f_pct, 'ratio'),
                 (v_pid, 'Power',      v_pwr, u_watt, pg_temp.num(v_pwr,'FM0.9999') || 'W', v_pwr, 'power'),
                 (v_pid, 'Temperature Coefficient', v_tc, u_ppm,
                  '±' || pg_temp.num(v_tc,'FM999') || 'ppm/°C', v_tc * f_ppm, 'ratio'),
                 -- text-only attributes: no unit, so these become enum facets
                 (v_pid, 'Type',     0, NULL, v_type,  NULL, NULL),
                 (v_pid, 'Mounting', 0, NULL, v_mount, NULL, NULL);

          SELECT id INTO v_mid FROM res_partner WHERE name = v_mfr LIMIT 1;
          INSERT INTO part_manufacturer_info (product_id, manufacturer_id, part_number)
          VALUES (v_pid, v_mid, v_mpn);
        END LOOP;
      END LOOP;
    END LOOP;

    -- ---------- MLCC capacitors ----------
    k := 0;
    FOR dec_i IN 1..8 LOOP                        -- 10pF .. 100µF
      FOR val_i IN 1..array_length(e6,1) LOOP
        v_val := e6[val_i] * power(10::numeric, dec_i) * 1e-12;   -- farads
        EXIT WHEN v_val > 0.0001;

        k := k + 1;
        v_pkg  := c_pkgs[1 + (k % array_length(c_pkgs,1))];
        v_mfr  := c_mfrs[1 + (k % array_length(c_mfrs,1))];
        v_diel := dielectrics[1 + (k % array_length(dielectrics,1))];
        v_volt := volt_choices[1 + (k % array_length(volt_choices,1))];
        v_tol  := tol_choices[1 + (k % array_length(tol_choices,1))];

        IF    v_val >= 1e-6  THEN v_disp := pg_temp.num(v_val*1e6,  'FM999999.999') || 'µF';
        ELSIF v_val >= 1e-9  THEN v_disp := pg_temp.num(v_val*1e9,  'FM999999.999') || 'nF';
        ELSE                      v_disp := pg_temp.num(v_val*1e12, 'FM999999.999') || 'pF';
        END IF;

        v_code := 'DP-C-' || v_pkg || '-' || k;
        v_mpn  := CASE v_mfr
                    WHEN 'Murata' THEN 'GRM' || substr(v_pkg,1,2) || 'R' || k || 'KA01L'
                    WHEN 'TDK'    THEN 'C' || v_pkg || 'X7R1H' || k || 'KT'
                    WHEN 'KEMET'  THEN 'C' || v_pkg || 'C' || k || 'K5RACTU'
                    WHEN 'YAGEO'  THEN 'CC' || v_pkg || 'KRX7R9BB' || k
                    ELSE               'CL' || substr(v_pkg,1,2) || 'B' || k || 'KBNNNE'
                  END;

        SELECT id INTO v_fid FROM part_footprint WHERE name = v_pkg;

        INSERT INTO product_product
            (name, default_code, description, type, categ_id, uom_id, uom_po_id,
             list_price, standard_price, qty_available, footprint_id, active,
             sale_ok, purchase_ok, company_id)
        VALUES
            (v_disp || ' ' || pg_temp.num(v_volt,'FM9990') || 'V ' || v_diel || ' ' || v_pkg || ' MLCC',
             v_code,
             'Multilayer ceramic capacitor, ' || v_diel || ', ' || v_pkg || ', ' ||
             pg_temp.num(v_volt,'FM9990') || 'V rated',
             'product', v_cat_mlcc, v_uom, v_uom,
             ((1 + (k % 30))::bigint) * 1000,
             ((1 + (k % 15))::bigint) * 1000,
             (CASE WHEN k % 9 = 0 THEN 0 ELSE (800 + k * 211) END)::bigint * 1000000,
             v_fid, true, true, true, NULL)
        RETURNING id INTO v_pid;

        INSERT INTO part_parameter (product_id, name, value_numeric, unit_id, value_text, value_base, quantity_kind)
        VALUES (v_pid, 'Capacitance',    v_val,  u_farad, v_disp, v_val, 'capacitance'),
               (v_pid, 'Voltage Rating', v_volt, u_volt,
                pg_temp.num(v_volt,'FM9990') || 'V', v_volt, 'voltage'),
               (v_pid, 'Tolerance',      v_tol,  u_pct,
                '±' || pg_temp.num(v_tol,'FM990.9') || '%', v_tol * f_pct, 'ratio'),
               (v_pid, 'Dielectric', 0, NULL, v_diel,          NULL, NULL),
               (v_pid, 'Mounting',   0, NULL, 'Surface Mount', NULL, NULL);

        SELECT id INTO v_mid FROM res_partner WHERE name = v_mfr LIMIT 1;
        INSERT INTO part_manufacturer_info (product_id, manufacturer_id, part_number)
        VALUES (v_pid, v_mid, v_mpn);
      END LOOP;
    END LOOP;
END
$seed$;

COMMIT;
SQL

psql_ -tAc "SELECT 'parts:      ' || count(*) FROM product_product WHERE default_code LIKE 'DP-%'"
psql_ -tAc "SELECT 'parameters: ' || count(*) FROM part_parameter pa JOIN product_product pp ON pp.id=pa.product_id WHERE pp.default_code LIKE 'DP-%'"
psql_ -tAc "SELECT 'mpn lines:  ' || count(*) FROM part_manufacturer_info mi JOIN product_product pp ON pp.id=mi.product_id WHERE pp.default_code LIKE 'DP-%'"
echo "Demo catalogue seeded."
