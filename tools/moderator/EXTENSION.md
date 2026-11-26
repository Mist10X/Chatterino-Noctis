# Noctis Moderator extension host API 1–2

The access manifest and the independent extension bundle are distinct.
The host validates the current Twitch ID, signed grant, verified channel
role, deduplication and safe moderation actions. The extension cannot
change grants, credentials, paths, networking, or executable code.

The signed JSON package specifies the stable host API version, monotonic
revision, localized UI labels and informational pages. API 1 packages retain
their original reward timeout presets. API 2 packages can also describe
parameter forms (bounded integers, booleans, text, and enumerated choices),
the order of the native reply and moderation pages, and conditional workflows
using approved host actions. Workflows can evaluate validated event data and
rule parameters with a bounded expression evaluator; allowed outcomes are
skip, one bounded timeout, removal of a confirmed timeout, or one chat reply.
The client continues to own all permissions, target validation, deduplication,
dry-run behavior, and dispatch. No package can execute native or arbitrary
script code, access files, issue network requests, or change these checks.
New workflows and their forms can be updated without a client rebuild. New
Twitch capabilities or new renderer primitive types still require a host
update. Existing auto-reply rules,
manual timers and moderation configurations retain their original files.
Only rules explicitly using a module action gain its identifier and optional
typed parameter values. If a later bundle removes the action its stored rule
is retained but cannot execute. An older host sees the module rule as disabled.

After full validation, a package is stored atomically and activated when
all editing windows are closed. Rejected/older packages leave the current
module intact. The host fetches the separate `moderator/extension.json`
from the repository every five minutes. The local import dialog accepts
the same owner-signed format; it cannot install unsigned scripts or grant
access. The initial published API 2 bundle is version 2.0.0, revision 3.

Build a signed package offline using `sign-extension.py --spec ... --output ...`.
Use the same protected per-user signing key as the access manifest.

The local API 2 baseline is `extension-spec-v2.json` (revision 3). The next
fixture, `extension-spec-v2-preview.json` (revision 4), retains prior action
identifiers, reorders the pages and adds a configurable message-warning
workflow. Both are compatible with the API 2 host. Test with dry-run and local
import; the revision 4 preview is not distributed as a public update.
