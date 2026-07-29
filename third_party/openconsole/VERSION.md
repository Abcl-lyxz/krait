# Bundled OpenConsole / ConPTY (ADR-0011)

- Source: microsoft/terminal release v1.24.11911.0, asset
  Microsoft.Windows.Console.ConPTY.1.24.260710001.nupkg (x64 payload:
  conpty.dll, OpenConsole.exe, conpty.lib, conpty.h).
- Acquisition path decided in T15: REPACKAGE the official ConPTY nupkg
  (no source build needed; the nupkg is exactly the redistributable).
- Passthrough floor: >= v1.22 DCS passthrough fix (this is v1.24). MIT
  license shipped alongside (LICENSE).
- Bump procedure: new nupkg + update this file + conpty smoke ('dir' +
  vttest) per ADR-0011.
