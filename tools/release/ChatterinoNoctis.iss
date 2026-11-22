; Installer for Chatterino Noctis (built by build-release.ps1).
;
; The program keeps its settings, cache and logs next to itself (portable
; profile), so it is installed for the current user into a folder that user can
; write to: no administrator rights, no Program Files. Updating over an existing
; installation keeps the settings; uninstalling asks whether to remove them.

#ifndef AppVersion
#define AppVersion "1.0.0"
#endif
#ifndef StagingDir
#define StagingDir "staging"
#endif
#ifndef OutputDir
#define OutputDir "dist"
#endif

#define AppName "Chatterino Noctis"
#define AppExe "Chatterino Noctis.exe"
; Must match the AppUserModelID the program sets (src/common/Version.cpp),
; so the shortcut and the running window share one taskbar button.
; Keep this ID and AppId stable across the rebrand for existing installations.
#define AppUserModel "Mist1X.ChatterinoByMist1X"

[Setup]
AppId={{0599F313-9171-4ABA-9366-C234F95C7018}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=Mist1X
AppCopyright=Chatterino Noctis — на основе Leafyrino, Chatterino7 и Chatterino 2 (MIT)
VersionInfoVersion={#AppVersion}
VersionInfoProductName={#AppName}
VersionInfoDescription=Установщик {#AppName}
DefaultDirName={localappdata}\Programs\{#AppName}
UsePreviousAppDir=yes
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir={#OutputDir}
OutputBaseFilename=ChatterinoNoctis-Setup-{#AppVersion}
SetupIconFile={#StagingDir}\Chatterino-Noctis.ico
UninstallDisplayIcon={app}\{#AppExe}
UninstallDisplayName={#AppName}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
; Close a running copy before replacing its files (it is in the same folder).
CloseApplications=yes
RestartApplications=no
LicenseFile={#StagingDir}\LICENSE.txt
InfoAfterFile={#StagingDir}\Что нового.txt
RestartIfNeededByRun=no

[Languages]
Name: "russian"; MessagesFile: "compiler:Languages\Russian.isl"

[Tasks]
Name: "desktopicon"; Description: "Создать значок на рабочем столе"; GroupDescription: "Дополнительно:"

[Files]
Source: "{#StagingDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
; Old pinned links may explicitly use the icon from the previous installer.
; Refresh that file in place for upgrades.
Source: "{#StagingDir}\Chatterino-Noctis.ico"; DestDir: "{app}"; DestName: "Chatterino-by-Mist1X.ico"; Flags: ignoreversion; Check: FileExists(ExpandConstant('{app}\Chatterino-by-Mist1X.ico'))

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\{#AppExe}"; IconFilename: "{app}\Chatterino-Noctis.ico"; AppUserModelID: "{#AppUserModel}"
Name: "{autoprograms}\{#AppName} — руководство"; Filename: "{app}\Руководство.html"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; IconFilename: "{app}\Chatterino-Noctis.ico"; AppUserModelID: "{#AppUserModel}"; Tasks: desktopicon

[Run]
Filename: "{app}\Руководство.html"; Description: "Открыть руководство (как пользоваться)"; Flags: postinstall shellexec skipifsilent nowait
Filename: "{app}\{#AppExe}"; Description: "Запустить {#AppName}"; Flags: postinstall nowait skipifsilent
; Started from the program's own update (/AUTOUPDATE=1): start it again.
Filename: "{app}\{#AppExe}"; Flags: nowait; Check: IsAutoUpdate

[UninstallDelete]
; Files the program itself creates next to it that are not settings.
Type: filesandordirs; Name: "{app}\Cache"
Type: filesandordirs; Name: "{app}\Crashes"
Type: filesandordirs; Name: "{app}\IPC"

[Code]
procedure SHChangeNotify(EventId, Flags: LongWord; Item1, Item2: String);
  external 'SHChangeNotify@shell32.dll stdcall';

function IsAutoUpdate: Boolean;
begin
  Result := ExpandConstant('{param:AUTOUPDATE|0}') = '1';
end;

{ A pin is registered by Windows with its existing shortcut path and AUMID.
  Never rename or delete it during an upgrade. Refresh only links to our EXE. }
procedure RefreshPinnedIcons;
var
  PinDir, PinPath, Target: String;
  Entry: TFindRec;
  Shell, Shortcut: Variant;
begin
  PinDir := ExpandConstant('{userappdata}\Microsoft\Internet Explorer\Quick Launch\User Pinned\TaskBar');
  Target := ExpandConstant('{app}\{#AppExe}');
  if not FindFirst(PinDir + '\*.lnk', Entry) then
    exit;
  try
    Shell := CreateOleObject('WScript.Shell');
    repeat
      PinPath := PinDir + '\' + Entry.Name;
      try
        Shortcut := Shell.CreateShortcut(PinPath);
        if (CompareText(Shortcut.TargetPath, Target) = 0) or
           (CompareText(Shortcut.TargetPath, ExpandConstant('{app}\Leafyrino.exe')) = 0) then
        begin
          Shortcut.TargetPath := Target;
          Shortcut.IconLocation := ExpandConstant('{app}\Chatterino-Noctis.ico') + ',0';
          Shortcut.Description := '{#AppName}';
          Shortcut.Save;
          SHChangeNotify($2000, $2005, PinPath, '');
        end;
      except
        Log('Unable to refresh an owned taskbar shortcut: ' + PinPath);
      end;
    until not FindNext(Entry);
  finally
    FindClose(Entry);
  end;
end;

{ Retire only old shortcuts that point to this installation. }
procedure RenameLegacyLink(OldLink, NewLink, Target: String);
var
  Shell, Shortcut: Variant;
begin
  if not FileExists(OldLink) then
    exit;
  try
    Shell := CreateOleObject('WScript.Shell');
    Shortcut := Shell.CreateShortcut(OldLink);
    if CompareText(Shortcut.TargetPath, Target) <> 0 then
    begin
      if (CompareText(Target, ExpandConstant('{app}\{#AppExe}')) <> 0) or
         (CompareText(Shortcut.TargetPath, ExpandConstant('{app}\Leafyrino.exe')) <> 0) then
        exit;
      Shortcut.TargetPath := Target;
      Shortcut.Save;
    end;
    if FileExists(NewLink) then
    begin
      Shortcut := Shell.CreateShortcut(NewLink);
      if CompareText(Shortcut.TargetPath, Target) = 0 then
        DeleteFile(OldLink);
    end
    else
      RenameFile(OldLink, NewLink);
  except
    Log('Unable to migrate a legacy shortcut: ' + OldLink);
  end;
end;

procedure MigrateStartupEntry;
var
  RunKey, OldCommand, NewCommand, Existing: String;
begin
  RunKey := 'Software\Microsoft\Windows\CurrentVersion\Run';
  if not RegQueryStringValue(HKCU, RunKey, 'Leafyrino', OldCommand) then
    exit;
  if CompareText(OldCommand, '"' + ExpandConstant('{app}\Leafyrino.exe') + '" --autorun') <> 0 then
    exit;
  NewCommand := '"' + ExpandConstant('{app}\{#AppExe}') + '" --autorun';
  if RegQueryStringValue(HKCU, RunKey, '{#AppName}', Existing) and
     (CompareText(Existing, NewCommand) <> 0) then
    exit;
  if RegWriteStringValue(HKCU, RunKey, '{#AppName}', NewCommand) then
    RegDeleteValue(HKCU, RunKey, 'Leafyrino');
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep <> ssPostInstall then
    exit;
  MigrateStartupEntry;
  RenameLegacyLink(ExpandConstant('{autoprograms}\Leafyrino.lnk'),
                   ExpandConstant('{autoprograms}\{#AppName}.lnk'),
                   ExpandConstant('{app}\{#AppExe}'));
  RenameLegacyLink(ExpandConstant('{autodesktop}\Leafyrino.lnk'),
                   ExpandConstant('{autodesktop}\{#AppName}.lnk'),
                   ExpandConstant('{app}\{#AppExe}'));
  try
    RefreshPinnedIcons;
  except
    Log('Unable to refresh taskbar icons; keeping the existing pins.');
  end;
  RenameLegacyLink(ExpandConstant('{autoprograms}\Chatterino by Mist1X.lnk'),
                   ExpandConstant('{autoprograms}\{#AppName}.lnk'),
                   ExpandConstant('{app}\{#AppExe}'));
  RenameLegacyLink(ExpandConstant('{autodesktop}\Chatterino by Mist1X.lnk'),
                   ExpandConstant('{autodesktop}\{#AppName}.lnk'),
                   ExpandConstant('{app}\{#AppExe}'));
  RenameLegacyLink(ExpandConstant('{autoprograms}\Chatterino by Mist1X — руководство.lnk'),
                   ExpandConstant('{autoprograms}\{#AppName} — руководство.lnk'),
                   ExpandConstant('{app}\Руководство.html'));
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Dir: String;
begin
  if CurUninstallStep <> usPostUninstall then
    exit;
  Dir := ExpandConstant('{app}');
  if UninstallSilent then
    exit;
  if MsgBox('Удалить также ваши настройки, вход в Twitch, журналы и аватарки?' + #13#10 + #13#10 +
            'Нажмите «Нет», если собираетесь установить программу снова и хотите сохранить настройки.',
            mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES then
  begin
    DelTree(Dir + '\Settings', True, True, True);
    DelTree(Dir + '\Logs', True, True, True);
    DelTree(Dir + '\Misc', True, True, True);
    DelTree(Dir + '\ProfileAvatars', True, True, True);
    DelTree(Dir + '\Plugins', True, True, True);
    DelTree(Dir + '\Themes', True, True, True);
    RemoveDir(Dir);
  end;
end;
