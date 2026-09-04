# 111 — Account access policy: admin-only creation, admin-issued resets

## The policy

Two public, unauthenticated surfaces used to let a stranger act on the system
without an account. Both are now closed. There is exactly one way to get an
account and one way to reset a password, and both go through an administrator.

| Capability | Before | Now |
|---|---|---|
| Get an account | `POST /web/signup` created a full internal user (`share=false`) for anyone who could reach it, gated only by `auth_signup.allow` (default **on**) | An administrator creates the user (`res.users create`, admin-gated). `/web/signup` refuses every request. |
| Reset a password | `POST /web/reset_password` with just a login **issued a token** to the caller (self-service), gated by `auth_signup.reset_pwd` (default **on**) | An administrator mints a one-time link (`res.users action_generate_reset_link`) and sends it by hand. `/web/reset_password` only **completes** a reset with a token the admin already generated. |

Neither door is gated by config any more. The old flags
(`auth_signup.allow`, `auth_signup.reset_pwd`) are no longer seeded and no
longer read — a leftover row set to `True` cannot re-open either path. The
refusal lives in code so the secure state is the only state.

## Why config gating was not enough

`configBool_` defaulted to **allow** when the parameter row was missing, and the
seed shipped both flags as `True`. So the safe-looking default was in fact the
open one: a fresh database, or one whose config row had been deleted, self-
registered internal users to the public. Auth policy should fail closed, so it
is now a code invariant rather than a data value.

## How an admin resets a password now

`res.users.action_generate_reset_link([[user_id]])` (Administrator or
Settings/Configuration group):

1. mints a cryptographically-random token (`AuthService::randomToken`, 24 bytes),
2. stores it on the user's **partner** (`signup_token` / `signup_expiration`,
   24-hour expiry — the same columns the completion route reads),
3. returns `{ login, token, reset_url, expires_hours }`.

`reset_url` is `<web.base.url>/?reset_login=<login>&reset_token=<token>`. The
admin copies it and sends it to the user out of band — nothing is emailed, and
the admin never sees or sets the user's actual password.

In the UI: **Settings → Users**, open a user, **Generate reset link**. The link
appears in a read-only, copyable field with a note that it is valid 24 hours and
single-use.

## What the user does with the link

Opening the link lands on the SPA, which — being unauthenticated — shows the
login page. `LoginPage` reads `reset_login` + `reset_token` from the query
string and, only when both are present, renders a **"set a new password"**
panel instead of the sign-in form. The panel POSTs `{login, token, password}`
to `/web/reset_password`; on success the token is scrubbed from the address bar.

The token is the credential. The server validates it, enforces the 8-character
floor **before** spending it (so a rejected weak password does not burn the
one-time token), and clears it once used.

## Guarantees, and where they are tested

`tests/functional/10-account` (41 checks) is the executable statement of this
policy:

- `/web/signup` refuses (403) and creates nothing — **even with the legacy
  `auth_signup.allow=True` row re-added**, which is the assertion that proves
  the door is code-closed, not config-closed.
- `res.users create` is the only account path, and a non-admin cannot use it.
- self-service reset (`{login}` with no token) is refused (403) and issues no
  token.
- only an admin can mint a link; the token then completes a reset, is
  **single-use**, is **time-boxed** (an expired token is refused), and a wrong
  token changes nothing.
- the 8-char floor holds on the completion route and does not consume the token.
- **§11 drives the whole loop through real Chrome** — mint link → open it
  anonymously → the panel renders (an OWL template error here would be invisible
  server-side) → set a password → the new password signs in.

## Files

- `modules/auth/AuthSignupModule.cpp` — `/web/signup` and `/web/reset_password`
  handlers (signup closed; reset is completion-only).
- `modules/auth/AuthViewModel.hpp` — `res.users action_generate_reset_link`.
- `modules/auth/AuthService.hpp` — `randomToken`.
- `modules/ir/IrModule.cpp` — the two `auth_signup.*` seed rows removed.
- `web/static/src/components/LoginPage.js` — the reset panel.
- `web/static/src/app.js` — the **Generate reset link** button on the user form.
- `tests/functional/10-account/` — `test.sh` + `drive.mjs`.
