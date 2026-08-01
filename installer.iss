#define MyAppName "Sonalis"
#ifndef MyAppVersion
#define MyAppVersion "5.2.0"
#endif
#ifndef MyAppVersionNumeric
#define MyAppVersionNumeric "5.2.0.0"
#endif
#define MyAppExeName "Sonalis.exe"
#ifndef MyAppSourceExe
#define MyAppSourceExe "Sonalis.exe"
#endif
#ifndef MyGuardianScannerSource
#define MyGuardianScannerSource "release-artifacts\SonalisGuardianScanner.exe"
#endif
#ifndef MyOutputBaseFilename
#define MyOutputBaseFilename "Sonalis-Kurulum-x64"
#endif

[Setup]
AppId={{6EEB5E42-1C8A-4AE5-ACEC-7D0F1E92C684}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher=Sonalis İletişim Teknolojileri A.Ş.
AppPublisherURL=https://sonalis.tr
AppSupportURL=https://sonalis.tr/support
AppUpdatesURL=https://sonalis.tr/download
DefaultDirName={localappdata}\Programs\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=no
AllowNoIcons=yes
OutputDir=.
OutputBaseFilename={#MyOutputBaseFilename}
SetupIconFile=Sonalis.ico
UninstallDisplayIcon={app}\{#MyAppExeName}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
UsePreviousAppDir=yes
UsePreviousGroup=yes
CloseApplications=yes
RestartApplications=no
SetupLogging=yes
VersionInfoCompany=Sonalis İletişim Teknolojileri A.Ş.
VersionInfoDescription=Sonalis Windows Kurulum Programı
VersionInfoProductName=Sonalis
VersionInfoProductVersion={#MyAppVersionNumeric}
VersionInfoVersion={#MyAppVersionNumeric}

[Languages]
Name: "turkish"; MessagesFile: "compiler:Languages\Turkish.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Masaüstünde kısayol oluştur"; GroupDescription: "Ek kısayollar:"; Flags: checkedonce
Name: "autostart"; Description: "Windows ile birlikte bildirim alanında başlat"; GroupDescription: "Başlangıç:"; Flags: checkedonce

[Files]
Source: "{#MyAppSourceExe}"; DestDir: "{app}"; DestName: "{#MyAppExeName}"; Flags: ignoreversion
Source: "{#MyGuardianScannerSource}"; DestDir: "{app}"; DestName: "SonalisGuardianScanner.exe"; Flags: ignoreversion
Source: "LICENSES.md"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\Sonalis"; Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"
Name: "{userdesktop}\Sonalis"; Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"; Tasks: desktopicon

[Registry]
Root: HKCU; Subkey: "Software\Classes\sonalis"; ValueType: string; ValueName: ""; ValueData: "URL:Sonalis Protocol"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\sonalis"; ValueType: string; ValueName: "URL Protocol"; ValueData: ""
Root: HKCU; Subkey: "Software\Classes\sonalis\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#MyAppExeName},0"
Root: HKCU; Subkey: "Software\Classes\sonalis\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExeName}"" ""%1"""
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "Sonalis"; ValueData: """{app}\{#MyAppExeName}"" --background"; Tasks: autostart; Flags: uninsdeletevalue

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "Sonalis'i başlat"; Flags: nowait postinstall skipifsilent unchecked; Check: not RestartSonalisAfterUpdate
Filename: "{app}\{#MyAppExeName}"; Flags: nowait; Check: RestartSonalisAfterUpdate

[Code]
function RestartSonalisAfterUpdate(): Boolean;
var
  Index: Integer;
begin
  Result := False;
  for Index := 1 to ParamCount do
  begin
    if CompareText(ParamStr(Index), '/RESTARTSONALIS') = 0 then
    begin
      Result := True;
      Exit;
    end;
  end;
end;
