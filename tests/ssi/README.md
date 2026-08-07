# 3DHISTECH SSI live probe

`tests/ssi_probe.py` verifies session creation, read-only slide opening, base
properties, one native JPEG tile, and explicit cleanup. It never prints
credentials, session IDs, slide tokens, or request URLs containing handles.

Slidescape itself prints the complete SlideCenter attributes and hierarchical
slide-properties JSON when verbose mode is enabled. Those documents can contain
identifying clinical metadata; keep verbose SSI output out of public logs.

Set credentials only in the process environment of the terminal that launches
the probe. On Windows, use a masked credential prompt:

```powershell
$credential = Get-Credential -Message "SlideCenter credentials"
$env:SLIDESCAPE_SSI_API_URL = "https://slidecenter.example.test/SSI"
$env:SLIDESCAPE_SSI_SLIDECENTER_URL = "https://slidecenter.example.test"
$env:SLIDESCAPE_SSI_USERNAME = $credential.UserName
$env:SLIDESCAPE_SSI_PASSWORD = $credential.GetNetworkCredential().Password

try {
	conda run -n ml314 python .\tests\ssi_probe.py fe2a3b0ec0f18b9b3981094ba2e3c2df
} finally {
	Remove-Item Env:SLIDESCAPE_SSI_USERNAME -ErrorAction SilentlyContinue
	Remove-Item Env:SLIDESCAPE_SSI_PASSWORD -ErrorAction SilentlyContinue
	$credential = $null
}
```

If Python does not trust the lab's private certificate authority, point
`SLIDESCAPE_SSI_CA_BUNDLE` at a PEM bundle containing that CA. Do not disable
TLS verification.

After the C backend is built, the same environment can open the slide directly:

```powershell
.\slidescape_console.exe ssi:fe2a3b0ec0f18b9b3981094ba2e3c2df
```

Variables set in a terminal are inherited only by programs launched from that
terminal. They do not change the environment of an already-running Codex or
Slidescape process. The graphical Open URI window can instead prompt for the
username and password; those values are kept only in process memory.

Slidescape also accepts secure 3DHISTECH SlideViewer launcher links in both the
Open URI window and on the command line:

```text
mview:OfflTelSlide?HUBIP=slidecenter.example.test&HUBSecure=true&SldID=fe2a3b0ec0f18b9b3981094ba2e3c2df&AccID=...&page=1&Capt=...
```

For `mview:` links, the SSI API and SlideCenter URLs are inferred from `HUBIP`.
`AccID` is passed to SSI's `session/connect` operation. The returned SSI session
is independent of the launcher session. Credentials are requested only if that
connection is unavailable or expired. `page` is ignored, while `Capt` is used
as an initial display-name hint.
