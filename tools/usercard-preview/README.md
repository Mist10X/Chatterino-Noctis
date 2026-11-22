# Offline usercard fixture

`Leafyrino-usercard-preview` checks the actual `UserInfoPopup`, `ChannelView`,
and related public widgets using fictional in-memory data. It does not launch
the normal application, load accounts or initialize live chat providers.

Enable `-DLEAFYRINO_USERCARD_PREVIEW=ON` and build
`Leafyrino-usercard-preview`. Use a fresh output folder and the Qt runtime.
Fixtures use temporary settings independently of installed user profiles.

Examples for the compiled executable:

```powershell
Leafyrino-usercard-preview.exe --render-hidden --case a --scripted-checks --report D:/preview/card.json --screenshot D:/preview/card.png --quit-after 24000
Leafyrino-usercard-preview.exe --render-hidden --history-checks --report D:/preview/history.json
Leafyrino-usercard-preview.exe --render-hidden --roles-checks --report D:/preview/roles.json --screenshot D:/preview/roles.png
Leafyrino-usercard-preview.exe --render-hidden --public-logs-checks --report D:/preview/public-logs.json
```

The history and role fixtures use process-owned loopback stand-ins and the
production requests. They cover identity isolation, public history import,
message deduplication, loading on user request and opt-out handling. The
scripted card fixture covers normal messages, current moderation notices,
quick actions, badges and responsive layout. No actual Twitch account or
external provider traffic is involved in these checks.

Additional opt-in fixtures check startup history, badge and emote pickers,
toasts, stream statistics, image/link previews, channel points, predictions,
and service-health display. Use `--help` for the available flags. Modes ending
in `-live` explicitly contact their named external public provider and are
unsuitable for routine offline test runs.
