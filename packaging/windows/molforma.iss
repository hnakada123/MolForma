; SPDX-License-Identifier: GPL-2.0-or-later
; The installer of MolForma for Windows (docs/installation/windows.md), made with Inno Setup 6 from the folder
; of a release; packaging/windows/release.sh compiles it:
;
;   ISCC /DVersion=<version> /DFolder=<the folder> /DOutput=<where to put it> /DRuntime=<MSVC's version> molforma.iss
;
; It installs that folder as it is, for the user alone (%LOCALAPPDATA%\Programs\MolForma) or, when asked, for
; every user (Program Files), with a shortcut in the Start menu and an uninstaller. Like the folder, it holds
; none of Microsoft's runtime DLLs (README.txt): it tells when the computer has no runtime as new as the
; compiler of molforma.exe (Runtime) and offers Microsoft's installer. Preferences, autosaves and jobs are
; never touched, also on removal.

#ifndef Version
  #error Give the version of MolForma: /DVersion=<version>
#endif
#ifndef Folder
  #error Give the folder of the release: /DFolder=<folder>
#endif
#ifndef Output
  #define Output "."
#endif
#ifndef Runtime
  #error Give the version of MSVC that compiled MolForma: /DRuntime=<major.minor.build>
#endif

[Setup]
AppId={{DE446B5A-7CE9-4888-8B78-54D3EA704FCC}
AppName=MolForma
AppVersion={#Version}
AppPublisher=hnakada123
AppPublisherURL=https://hnakada123.github.io/MolForma/
AppSupportURL=https://github.com/hnakada123/MolForma/issues
AppUpdatesURL=https://github.com/hnakada123/MolForma/releases
VersionInfoVersion={#Version}
VersionInfoProductName=MolForma
VersionInfoDescription=MolForma {#Version} Setup
DefaultDirName={autopf}\MolForma
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Windows 10 1809, the oldest that Qt 6 runs on; the release is checked on Windows 11.
MinVersion=10.0.17763
LicenseFile={#Folder}\LICENSE
SetupIconFile=molforma.ico
UninstallDisplayIcon={app}\molforma.ico
UninstallDisplayName=MolForma {#Version}
WizardStyle=modern
Compression=lzma2/ultra64
SolidCompression=yes
OutputDir={#Output}
OutputBaseFilename=MolForma-{#Version}-windows-x86_64-setup
SetupLogging=yes

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"
Name: "ja"; MessagesFile: "compiler:Languages\Japanese.isl"

[CustomMessages]
en.ShowReadme=Show README.txt (how to set up NWChem)
ja.ShowReadme=README.txt を表示する（NWChemの設定のしかた）
en.NoRuntime=MolForma needs the Microsoft Visual C++ Redistributable (x64), version %1 or later, which this computer does not have.%n%nDownload its installer from Microsoft now? Run it, then start MolForma.
ja.NoRuntime=MolFormaには、Microsoft Visual C++ 再頒布可能パッケージ（x64）の %1 以降が必要ですが、このPCには入っていません。%n%nMicrosoftからそのインストーラーをダウンロードしますか？ 実行してから、MolFormaを起動してください。

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[InstallDelete]
; What an earlier version installed (Qt and the other libraries of their own versions), when the folder is
; MolForma's.
Type: filesandordirs; Name: "{app}\bin"; Check: IsMolFormaFolder
Type: filesandordirs; Name: "{app}\plugins"; Check: IsMolFormaFolder
Type: filesandordirs; Name: "{app}\translations"; Check: IsMolFormaFolder
Type: filesandordirs; Name: "{app}\share"; Check: IsMolFormaFolder
Type: filesandordirs; Name: "{app}\licenses"; Check: IsMolFormaFolder

[Files]
Source: "{#Folder}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "molforma.ico"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\MolForma"; Filename: "{app}\bin\molforma.exe"; IconFilename: "{app}\molforma.ico"
Name: "{autodesktop}\MolForma"; Filename: "{app}\bin\molforma.exe"; IconFilename: "{app}\molforma.ico"; Tasks: desktopicon

[Run]
Filename: "{app}\bin\molforma.exe"; Description: "{cm:LaunchProgram,MolForma}"; Flags: nowait postinstall skipifsilent; Check: HasRuntime
Filename: "{app}\README.txt"; Description: "{cm:ShowReadme}"; Flags: shellexec postinstall skipifsilent unchecked

[Code]
const
  RuntimeKey = 'SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64';
  RuntimeInstaller = 'https://aka.ms/vc14/vc_redist.x64.exe';

function IsMolFormaFolder: Boolean;
begin
  Result := FileExists(ExpandConstant('{app}\bin\molforma.exe'));
end;

// The runtime that Microsoft's installer recorded in one view of the registry.
function RuntimeIn(Root: Integer; var Version: Int64): Boolean;
var
  Installed, Major, Minor, Build: Cardinal;
begin
  Result := RegQueryDWordValue(Root, RuntimeKey, 'Installed', Installed) and (Installed = 1) and
    RegQueryDWordValue(Root, RuntimeKey, 'Major', Major) and
    RegQueryDWordValue(Root, RuntimeKey, 'Minor', Minor) and
    RegQueryDWordValue(Root, RuntimeKey, 'Bld', Build);
  if Result then
    Version := PackVersionComponents(Major, Minor, Build, 0);
end;

// Whether the computer has a runtime as new as the compiler of molforma.exe.
function HasRuntime: Boolean;
var
  Needed, Found: Int64;
begin
  if not StrToVersion('{#Runtime}', Needed) then
    RaiseException('Bad runtime version: {#Runtime}');
  Result := (RuntimeIn(HKLM64, Found) and (ComparePackedVersion(Found, Needed) >= 0)) or
    (RuntimeIn(HKLM32, Found) and (ComparePackedVersion(Found, Needed) >= 0));
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Code: Integer;
begin
  if (CurStep = ssPostInstall) and not HasRuntime then
    if SuppressibleMsgBox(FmtMessage(CustomMessage('NoRuntime'), ['{#Runtime}']), mbConfirmation, MB_YESNO,
        IDNO) = IDYES then
      ShellExecAsOriginalUser('open', RuntimeInstaller, '', '', SW_SHOWNORMAL, ewNoWait, Code);
end;
