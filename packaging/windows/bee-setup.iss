; Inno Setup script for the Bee language Windows installer.
; Compile this with Inno Setup (https://jrsoftware.org/isdl.php) to produce
; a friendly bee-<version>-amd64.exe wizard that non-technical users just run.
;
;   1. Build the Windows binaries first (see README-build.md) so that
;        bee.exe, bee_jit.dll, hive.exe and beegen.exe
;      exist next to this script under packaging\windows.
;   2. Open this file in the Inno Setup Compiler and press F9 (Compile),
;      or from a terminal:   iscc bee-setup.iss
;   3. The installer appears in   dist\bee-<version>-amd64.exe
;
; What the installer does:
;   - installs bee.exe and hive.exe (the package manager) to
;       C:\Program Files\BeeLang\bin
;   - installs the example programs
;   - (optional) adds bee to the system PATH
;   - (optional) associates .be / .bee files with a bee icon; double-click runs them
;   - registers a proper uninstaller in "Add or remove programs"

#define MyAppName        "BeeLang"
; Version can be overridden from the command line: iscc /DMyAppVersion=1.2.3 ...
#ifndef MyAppVersion
  #define MyAppVersion   "0.3.2"
#endif
#define MyAppPublisher   "Atanu Debnath"
#define MyAppExeName     "bee.exe"

[Setup]
AppId={{5E5C1B7A-6C2E-4E7C-9E4E-B0EE1A9C0001}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL=https://github.com/beelang-project/bee
AppSupportURL=https://github.com/beelang-project/bee/issues
AppUpdatesURL=https://github.com/beelang-project/bee/releases
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
OutputDir=..\..\dist
OutputBaseFilename=bee-{#MyAppVersion}-amd64
SetupIconFile=bee.ico
UninstallDisplayIcon={app}\bee.ico
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesInstallIn64BitMode=x64compatible
ChangesEnvironment=yes
PrivilegesRequired=admin

[Tasks]
Name: "addtopath"; Description: "Add ""bee"" to the system PATH (run scripts from any terminal)"; GroupDescription: "Integration:"
Name: "assoc";     Description: "Associate .be and .bee files with Bee (double-click to run)"; GroupDescription: "Integration:"

[Files]
Source: "bee.exe";          DestDir: "{app}\bin";      Flags: ignoreversion
; The LLVM JIT backend, dlopen'd by bee.exe from its own directory on first
; compile. Without it bee still runs, on the interpreter/VM.
Source: "bee_jit.dll";      DestDir: "{app}\bin";      Flags: ignoreversion
Source: "hive.exe";         DestDir: "{app}\bin";      Flags: ignoreversion
Source: "beegen.exe";       DestDir: "{app}\bin";      Flags: ignoreversion
Source: "bee.ico";          DestDir: "{app}";          Flags: ignoreversion
Source: "..\..\examples\*"; DestDir: "{app}\examples"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "..\..\README.md";  DestDir: "{app}";          Flags: ignoreversion

[Icons]
Name: "{group}\Bee examples";       Filename: "{app}\examples"
Name: "{group}\Uninstall BeeLang";  Filename: "{uninstallexe}"

[Registry]
; --- add {app}\bin to the system PATH (only if not already present) ---
Root: HKLM; Subkey: "SYSTEM\CurrentControlSet\Control\Session Manager\Environment"; \
    ValueType: expandsz; ValueName: "Path"; ValueData: "{olddata};{app}\bin"; \
    Tasks: addtopath; Check: NeedsAddPath(ExpandConstant('{app}\bin'))

; --- file associations for .be and .bee ---
Root: HKCR; Subkey: ".bee"; ValueType: string; ValueName: ""; ValueData: "BeeLang.Source"; Tasks: assoc; Flags: uninsdeletevalue
Root: HKCR; Subkey: ".be";  ValueType: string; ValueName: ""; ValueData: "BeeLang.Source"; Tasks: assoc; Flags: uninsdeletevalue
Root: HKCR; Subkey: "BeeLang.Source"; ValueType: string; ValueName: ""; ValueData: "Bee source file"; Tasks: assoc; Flags: uninsdeletekey
Root: HKCR; Subkey: "BeeLang.Source\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\bee.ico"; Tasks: assoc
; double-click runs the script and keeps the console window open to show output
Root: HKCR; Subkey: "BeeLang.Source\shell\open\command"; ValueType: string; ValueName: ""; \
    ValueData: """cmd"" /k """"{app}\bin\{#MyAppExeName}"" ""%1"""""; Tasks: assoc

[Run]
Filename: "{app}\bin\{#MyAppExeName}"; Parameters: """{app}\examples\01_hello.bee"""; \
    Description: "Run a sample program now"; Flags: postinstall shellexec skipifsilent

[Code]
{ Return True if Param is not already on the system PATH (avoids duplicates). }
function NeedsAddPath(Param: string): Boolean;
var
  OrigPath: string;
begin
  if not RegQueryStringValue(HKLM,
    'SYSTEM\CurrentControlSet\Control\Session Manager\Environment',
    'Path', OrigPath) then
  begin
    Result := True;
    exit;
  end;
  { look for the exact dir surrounded by ; so we don't match a prefix }
  Result := Pos(';' + Uppercase(Param) + ';', ';' + Uppercase(OrigPath) + ';') = 0;
end;
