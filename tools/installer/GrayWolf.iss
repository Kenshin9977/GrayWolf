; GrayWolf's Windows installer (Inno Setup 6): the release zip's files, installed per user.
;
; Per user, no administrator rights: GrayWolf writes beside its exe (graywolf.json, package_index\,
; the updater's .update\ folder), which Program Files would refuse. {autopf} under
; PrivilegesRequired=lowest is %LocalAppData%\Programs. The in-app updater keeps working unchanged
; there and, once it has replaced the files, writes the new DisplayVersion into this setup's
; uninstall entry (Updater.cpp, ApplyStaged).
;
; Built by .github/workflows/build.yml on a v* tag, after GrayWolf.exe is signed:
;   ISCC.exe /DAppVersion=<version> /DSourceDir=<folder of the release zip's files> /O<out dir> GrayWolf.iss

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\..\src\GrayWolf\x64\Release"
#endif

[Setup]
; Never change AppId: it names the uninstall entry the updater looks for (GrayWolf_is1).
AppId=GrayWolf
AppName=GrayWolf
AppVersion={#AppVersion}
AppVerName=GrayWolf {#AppVersion}
AppPublisher=Kenshin9977
AppPublisherURL=https://github.com/Kenshin9977/GrayWolf
AppSupportURL=https://github.com/Kenshin9977/GrayWolf/issues
AppUpdatesURL=https://github.com/Kenshin9977/GrayWolf/releases
DefaultDirName={autopf}\GrayWolf
DefaultGroupName=GrayWolf
DisableProgramGroupPage=yes
; no "install for all users": the updater could not write to Program Files
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
LicenseFile=..\..\LICENSE
SetupIconFile=..\..\src\GrayWolf\Resources\GrayWolf.ico
UninstallDisplayIcon={app}\GrayWolf.exe
UninstallDisplayName=GrayWolf
OutputBaseFilename=GrayWolf-{#AppVersion}-x64-setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
; a running GrayWolf is closed (Restart Manager) before its files are replaced
CloseApplications=yes
RestartApplications=no

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\GrayWolf"; Filename: "{app}\GrayWolf.exe"
Name: "{autodesktop}\GrayWolf"; Filename: "{app}\GrayWolf.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\GrayWolf.exe"; Description: "{cm:LaunchProgram,GrayWolf}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; what GrayWolf downloaded or wrote after the install; graywolf.json (the user's settings) stays
Type: filesandordirs; Name: "{app}\.update"
Type: filesandordirs; Name: "{app}\package_index"
Type: filesandordirs; Name: "{app}\zonekit"
Type: files; Name: "{app}\*.log"
