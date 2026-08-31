You are tidying the rows of a bill of materials so an ERP can store them
consistently. The rows below were read out of some EDA tool's export and are
written however that tool and that engineer happened to write them.

Return the SAME rows, in the same order, with the same number of rows,
normalised to the conventions below. Answer with a SINGLE JSON object and
nothing else:
{"rows":[{"designators":string,"quantity":int,"mpn":string,"manufacturer":string,
 "value":string,"footprint":string,"description":string,"fitted":boolean}],
 "notes":string}

NEVER drop, merge, split or reorder a row. If a row cannot be tidied, return it
unchanged. The person reviewing is comparing your output line by line against
what they gave you.

NEVER choose a part. You are normalising text, not deciding which capacitor
this is - that lookup happens afterwards and has to be reproducible.

VALUE
  Write the magnitude ONCE. "4.7K" and "4K7" and "4k7ohm" all become "4k7".
  Use the SI shorthand an engineer would write: 4k7, 100n, 10u, 2R2, 125m.
  Strip a unit that is already implied by the component - "100nF" for a
  capacitor becomes "100n", because the unit belongs in its own field.
  Leave a value you do not understand exactly as it is.

FOOTPRINT
  Reduce a tool-specific footprint to the package name. KiCad's
  "Capacitor_SMD:C_0603_1608Metric" is "0603"; EAGLE's "C0603" is "0603";
  "R_0805_2012Metric" is "0805". Keep SOT-23, SOIC-8, QFN-32 and the like
  as they are.
  Prefer one of these packages the ERP already knows:
{{footprints}}
  If none fits, keep the shortest sensible package name rather than inventing
  a new spelling.

MPN
  The manufacturer's ordering part number only. A library reference
  ("CAP", "R-EU_R0603", "DEVICE=RES"), an internal ID or a description is NOT
  an MPN - blank it rather than pass it through.

DESIGNATORS
  Comma-separated. Keep ranges as ranges: "R1-R4" stays "R1-R4". Remove
  stray spaces and quotes. Do not renumber anything.

QUANTITY
  An integer. If it is missing but the designators are countable, set it to
  the number of designators.

FITTED
  false when the row is marked DNP, DNI, "do not populate", "no fit" or the
  description says so. true otherwise.

DESCRIPTION
  A short human description. Drop tool noise like "~", "DEVICE=", trailing
  separators and repeated whitespace. Empty is better than junk.

Units the ERP accepts, for reference:
{{units}}

"notes" is one or two short lines for the person reviewing: what you changed
in bulk, and anything you were unsure about.

=== ROWS ===
{{rows}}
