You are mapping the columns of a PCB bill of materials so an ERP can
import it. Below is the header row and up to three sample rows, as
written by some EDA tool (KiCad, Altium, EAGLE, OrCAD, EasyEDA...).

Return the ZERO-BASED column index for each field you can identify.
Use null for a field the file does not contain. Answer with a SINGLE
JSON object and nothing else:
{"mapping":{"designators":int|null,"quantity":int|null,"mpn":int|null,
 "manufacturer":int|null,"value":int|null,"footprint":int|null,
 "description":int|null,"fitted":int|null},
 "fitted_negated":boolean,"tool":string,"notes":string}

What each field means here:
  designators  the reference designators - R1, "C1,C2", "R1-R4".
               Altium calls it Designator, EAGLE calls it Part or Parts.
  value        the component value - 100nF, 4k7, 10uF.
               In Altium and JLCPCB exports this column is called Comment.
  mpn          the manufacturer's part number, not a library reference
               and not an internal ID.
  footprint    the package - 0603, SOT-23. EAGLE calls it Package.
  fitted       a do-not-populate column. Set fitted_negated TRUE when the
               column means DNP/exclude (a mark there means NOT fitted),
               and FALSE when it means Populate/Fitted.

Getting fitted_negated backwards populates exactly the parts that were
meant to be left off, so say null rather than guess.

"tool" is which EDA tool you think wrote this, or empty.
"notes" is one short line for the person reviewing - anything ambiguous.

=== HEADER ===
{{header}}
=== SAMPLE ROWS ===
{{samples}}
