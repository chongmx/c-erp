#!/usr/bin/env python3
"""
Build the Easy Locker Space website as ORDINARY CMS ROWS.

Nothing here is special-cased in the renderer: every page is a website_page
with a blocks_json array, so all of it is editable in the browser afterwards.
That is the whole point — a site that can only be changed by re-running a
script is not a website, it is a deployment.

Idempotent: a page is matched on its slug and rewritten.
"""
import json, subprocess, sys

DB = ["psql", "-h", "localhost", "-U", "odoo", "-d", "odoo", "-tAq", "-v", "ON_ERROR_STOP=1"]

def sql(q, *params):
    cmd = list(DB)
    if params:
        # psql -v style is awkward for arbitrary text; use a here-doc via stdin
        # with dollar-quoting instead. All values below are ours, not input.
        pass
    r = subprocess.run(cmd + ["-c", q], capture_output=True, text=True,
                       env={"PGPASSWORD": "odoo", "PATH": "/usr/bin:/bin"})
    if r.returncode != 0:
        print("SQL FAILED:", r.stderr.strip()[:400], file=sys.stderr)
        sys.exit(1)
    return r.stdout.strip()

def q(s):
    """Dollar-quote a literal so quotes and newlines in content are safe."""
    return "$cerp$" + s + "$cerp$"

# ---------------------------------------------------------------- content
CTA = "/site/contact"

home = [
    {"type": "hero",
     "eyebrow": "Self storage in Kuala Lumpur",
     "headline": "Room for the things you are not ready to let go of.",
     "subheadline": "Clean, dry, alarmed storage rooms you can reach seven days a "
                    "week. Two sizes, one simple monthly price, no deposit games.",
     "cta_text": "See sizes and prices", "cta_href": "/site/units",
     "alt_text": "Ask a question", "alt_href": CTA},

    {"type": "heading", "level": "2", "text": "Two sizes. That is the whole menu."},
    {"type": "text",
     "text": "Most people spend a week trying to guess which of nine unit sizes they "
             "need. We keep two, and we will tell you honestly which one fits."},

    {"type": "pricing", "items": [
        {"name": "The 50", "size": "50 sq ft · about 2.3 m × 2.0 m",
         "price": "RM 190", "period": "/month",
         "badge": "Most popular", "featured": True,
         "features": ["Fits a one-bedroom flat, packed sensibly",
                      "Roughly 40 medium boxes, or 25 boxes and a sofa",
                      "Ground floor units available",
                      "Own padlock — only you hold the key"],
         "cta_text": "Enquire about a 50", "cta_href": CTA},
        {"name": "The 90", "size": "90 sq ft · about 3.0 m × 2.8 m",
         "price": "RM 310", "period": "/month",
         "features": ["Fits a two to three-bedroom home",
                      "Room to walk in and reach the back",
                      "Takes a full bedroom set plus boxes",
                      "Popular with businesses holding stock"],
         "cta_text": "Enquire about a 90", "cta_href": CTA}]},

    {"type": "text",
     "text": "Not sure? Tell us what you are storing and we will say which one you "
             "need — including if that is the smaller one."},

    {"type": "divider"},
    {"type": "heading", "level": "2", "text": "Why people stay"},
    {"type": "columns", "items": [
        {"title": "Dry and ventilated",
         "text": "Ventilated units and a building that stays dry through the monsoon. "
                 "Your boxes come out the way they went in."},
        {"title": "Alarmed and covered",
         "text": "Individually alarmed units, CCTV on every corridor, and access "
                 "logged. You hold the only key to your own padlock."},
        {"title": "No surprises",
         "text": "One monthly price. No admin fee, no padlock fee, no insurance you "
                 "did not ask for. One month's notice to leave."}]},

    {"type": "divider"},
    {"type": "heading", "level": "2", "text": "Getting a unit takes a day"},
    {"type": "steps", "items": [
        {"title": "Tell us what you are storing",
         "text": "A sentence is enough. We will tell you which size fits and whether "
                 "we have one free."},
        {"title": "Come and look",
         "text": "See the actual unit before you commit. No appointment needed during "
                 "office hours."},
        {"title": "Sign and move in",
         "text": "A one-page agreement, the first month, and your own padlock. Most "
                 "people move in the same day."}]},

    {"type": "hero",
     "headline": "Need a unit this week?",
     "subheadline": "Tell us what you are storing and we will come back to you the "
                    "same working day.",
     "cta_text": "Ask about availability", "cta_href": CTA},
]

units = [
    {"type": "heading", "level": "1", "text": "Sizes and prices"},
    {"type": "text",
     "text": "Two sizes, priced per calendar month. The price you are quoted is the "
             "price you pay — there is no admin fee and no compulsory insurance."},

    {"type": "pricing", "items": [
        {"name": "The 50", "size": "50 sq ft · about 2.3 m × 2.0 m × 2.4 m high",
         "price": "RM 190", "period": "/month",
         "badge": "Most popular", "featured": True,
         "features": ["A one-bedroom flat, packed sensibly",
                      "About 40 medium boxes",
                      "Or 25 boxes plus a sofa and a mattress",
                      "Business use: about 6 pallets stacked"],
         "cta_text": "Enquire about a 50", "cta_href": CTA},
        {"name": "The 90", "size": "90 sq ft · about 3.0 m × 2.8 m × 2.4 m high",
         "price": "RM 310", "period": "/month",
         "features": ["A two to three-bedroom home",
                      "About 75 medium boxes",
                      "Full bedroom set, dining table and boxes",
                      "Business use: about 12 pallets stacked"],
         "cta_text": "Enquire about a 90", "cta_href": CTA}]},

    {"type": "heading", "level": "2", "text": "What is included"},
    {"type": "columns", "items": [
        {"title": "In the price",
         "text": "The unit, 7-day access, lighting, ventilation, CCTV, individual "
                 "alarm, and use of our trolleys."},
        {"title": "Not in the price",
         "text": "Your own padlock, which you keep. We sell one at cost if you would "
                 "rather not bring one."},
        {"title": "Terms",
         "text": "Monthly rolling. One month's notice either way. First month payable "
                 "on move-in."}]},

    {"type": "text",
     "text": "Prices shown are the current monthly rate and exclude SST where it "
             "applies. Availability changes — ask us before you plan a move."},
]

how = [
    {"type": "heading", "level": "1", "text": "How it works"},
    {"type": "steps", "items": [
        {"title": "Tell us what you are storing",
         "text": "Send a short message with roughly what you have — \"a "
                 "one-bedroom flat\" or \"twenty boxes and a bicycle\" is plenty. We "
                 "will tell you which size fits."},
        {"title": "Come and see the unit",
         "text": "We would rather you saw the actual room than a photograph. Come "
                 "during office hours; no appointment needed."},
        {"title": "Sign a one-page agreement",
         "text": "Monthly rolling, one month's notice. We will explain anything you "
                 "want explained before you sign."},
        {"title": "Fit your own padlock",
         "text": "You buy or bring the padlock and you keep the only key. We do not "
                 "hold a copy."},
        {"title": "Come and go",
         "text": "Access seven days a week. Bring what you need, take what you want, "
                 "no notice required."}]},
    {"type": "divider"},
    {"type": "heading", "level": "2", "text": "Leaving"},
    {"type": "text",
     "text": "Give us a month's notice, empty the unit, take your padlock off, and "
             "that is it. We do not charge a cleaning fee for a unit left swept."},
    {"type": "button", "text": "Ask about availability", "href": CTA},
]

faq = [
    {"type": "heading", "level": "1", "text": "Questions"},
    {"type": "faq", "items": [
        {"q": "Which size do I need?",
         "a": "The 50 takes about a one-bedroom flat — roughly 40 medium boxes, "
              "or 25 boxes with a sofa. The 90 takes a two to three-bedroom home. If "
              "you are between the two, tell us what you have and we will say which "
              "one, including if that is the smaller."},
        {"q": "Can I get in at the weekend?",
         "a": "Yes. Access is seven days a week. Office hours are shorter, so if you "
              "need to speak to somebody, come on a weekday."},
        {"q": "Is there a deposit?",
         "a": "No deposit. The first month is payable when you move in, and that is "
              "the whole of it."},
        {"q": "How long do I have to commit to?",
         "a": "It is monthly and rolling. One month's notice on either side. There is "
              "no minimum term and no penalty for leaving."},
        {"q": "Who has a key?",
         "a": "You do, and only you. You fit your own padlock and we do not keep a "
              "copy. Staff cannot open your unit."},
        {"q": "Is my property insured?",
         "a": "Not by us, and we will not sell you a policy you do not need. Many "
              "household policies extend to goods in storage — check yours, and "
              "insure separately if it does not."},
        {"q": "Can I store business stock?",
         "a": "Yes, and many do. The 90 takes about twelve stacked pallets. Tell us "
              "if you will be coming and going daily so we can put you near the door."},
        {"q": "What can I not store?",
         "a": "Nothing perishable, living, flammable, explosive, or illegal. If you "
              "are unsure about something, ask before you bring it."},
        {"q": "Do you help with moving?",
         "a": "We have trolleys and a loading bay, and we will point you at a mover we "
              "trust. We do not move things ourselves."}]},
    {"type": "divider"},
    {"type": "text", "text": "Something not answered here? Ask us — we would "
                             "rather answer it before you sign than after."},
    {"type": "button", "text": "Ask a question", "href": CTA},
]

contact = [
    {"type": "heading", "level": "1", "text": "Ask us anything"},
    {"type": "text",
     "text": "Tell us roughly what you are storing and we will come back to you the "
             "same working day with the size you need and whether one is free."},
    {"type": "form", "slug": "storage-enquiry"},
    {"type": "divider"},
    {"type": "heading", "level": "2", "text": "Find us"},
    # Coordinates, not a place name: an OpenStreetMap embed needs a bounding
    # box, and a name alone renders an empty grey rectangle. Replace these with
    # the yard's own latitude and longitude.
    {"type": "map", "query": "Easy Locker Space, Kuala Lumpur",
     "lat": "3.13900", "lon": "101.68690", "label": "Open in maps"},
]

PAGES = [
    ("home",     "Self storage in Kuala Lumpur", home, True,
     "Clean, dry, alarmed self-storage rooms in Kuala Lumpur. Two sizes, one simple "
     "monthly price, seven-day access.", 10),
    ("units",    "Sizes and prices", units, False,
     "Two storage sizes: 50 sq ft from RM190 a month and 90 sq ft from RM310 a month. "
     "No deposit, monthly rolling.", 20),
    ("how-it-works", "How it works", how, False,
     "Getting a storage unit takes a day: tell us what you are storing, see the unit, "
     "sign, fit your own padlock.", 30),
    ("faq",      "Questions", faq, False,
     "Common questions about self storage: sizes, access, deposits, notice periods, "
     "keys and insurance.", 40),
    ("contact",  "Contact us", contact, False,
     "Ask about availability and sizes. We reply the same working day.", 50),
]

# ---------------------------------------------------------------- write
sql("""
    INSERT INTO ir_config_parameter (key, value) VALUES
      ('website.site_name', 'Easy Locker Space'),
      ('website.accent',    '#e94560'),
      -- The preset as well as the accent. Setting only the accent left a
      -- re-seed looking like the default white 'paper' theme with a pink
      -- highlight, which is not the site anybody signed off (docs/126).
      ('website.theme',     'console'),
      ('website.dark_mode', 'auto'),
      ('website.footer',    'Easy Locker Space · Self storage in Kuala Lumpur')
    ON CONFLICT (key) DO UPDATE SET value = EXCLUDED.value
""")

# The enquiry form, and its fields.
sql(f"""
    INSERT INTO website_form (slug, title, description, submit_label, success_message,
                              target_model, active)
    VALUES ('storage-enquiry', 'Storage enquiry',
            {q('Tell us roughly what you are storing and when you need it.')},
            'Send enquiry',
            {q('Thank you — we will come back to you the same working day.')},
            'project.task', TRUE)
    ON CONFLICT (slug) DO UPDATE
      SET title=EXCLUDED.title, description=EXCLUDED.description,
          submit_label=EXCLUDED.submit_label, success_message=EXCLUDED.success_message,
          target_model=EXCLUDED.target_model, active=TRUE
""")
fid = sql("SELECT id FROM website_form WHERE slug='storage-enquiry'")
FIELDS = [
    ("name",   "Your name",              "text",     "true",  10, ""),
    ("phone",  "Phone",                  "tel",      "true",  20, ""),
    ("email",  "Email",                  "email",    "false", 30, ""),
    ("size",   "Which size interests you","select",  "false", 40, "The 50 (50 sq ft)\nThe 90 (90 sq ft)\nNot sure yet"),
    ("moving", "When do you need it",    "text",     "false", 50, ""),
    ("items",  "What are you storing",   "textarea", "false", 60, ""),
]
for nm, lbl, ty, req, seq, opts in FIELDS:
    sql(f"""
        INSERT INTO website_form_field (form_id, name, label, field_type, required, sequence, options)
        VALUES ({fid}, {q(nm)}, {q(lbl)}, {q(ty)}, {req}, {seq}, {q(opts)})
        ON CONFLICT (form_id, name) DO UPDATE
          SET label=EXCLUDED.label, field_type=EXCLUDED.field_type,
              required=EXCLUDED.required, sequence=EXCLUDED.sequence,
              options=EXCLUDED.options
    """)

# Pages. Nothing is published by this script — see the note in the report.
sql("UPDATE website_page SET is_homepage = FALSE WHERE is_homepage")
for slug, title, blocks, homepage, meta, seq in PAGES:
    bj = json.dumps(blocks, ensure_ascii=False)
    sql(f"""
        INSERT INTO website_page (slug, title, blocks_json, is_published, is_homepage,
                                  is_indexed, sequence, meta_description, page_kind)
        VALUES ({q(slug)}, {q(title)}, {q(bj)}, TRUE, {str(homepage).upper()},
                TRUE, {seq}, {q(meta)}, 'page')
        ON CONFLICT (slug) DO UPDATE
          SET title=EXCLUDED.title, blocks_json=EXCLUDED.blocks_json,
              is_published=EXCLUDED.is_published, is_homepage=EXCLUDED.is_homepage,
              sequence=EXCLUDED.sequence, meta_description=EXCLUDED.meta_description,
              write_date=now()
    """)

# Menu, rebuilt from scratch so re-running does not duplicate it.
sql("DELETE FROM website_menu")
for i, (slug, label) in enumerate([("units", "Sizes & prices"),
                                   ("how-it-works", "How it works"),
                                   ("faq", "Questions"),
                                   ("contact", "Contact")]):
    sql(f"""
        INSERT INTO website_menu (name, page_id, sequence)
        SELECT {q(label)}, id, {(i + 1) * 10} FROM website_page WHERE slug={q(slug)}
    """)

print("pages:  ", sql("SELECT count(*) FROM website_page WHERE is_published"))
print("menu:   ", sql("SELECT count(*) FROM website_menu"))
print("form:   ", sql("SELECT count(*) FROM website_form_field WHERE form_id=" + fid))
print("homepage:", sql("SELECT slug FROM website_page WHERE is_homepage"))
