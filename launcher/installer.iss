; Dante's Inferno - Inno Setup installer script
; Produces a standard Windows installer that is less likely to be flagged
; by SmartScreen/Windows Defender than the custom WPF payload installer.

#define MyAppName "Dante's Inferno"
#define MyAppPublisher "florinp93"
#define MyAppURL "https://github.com/florinp93/hells-gate-recomp"
#define MyAppExeName "DantesInfernoLauncher.exe"
#define MyAppVersion "0.7.2-beta-hotfix"

[Setup]
AppId={{DANTES-INFERNO-PC-PORT}}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}/issues
AppUpdatesURL={#MyAppURL}/releases
DefaultDirName={commonpf64}\{#MyAppName}
DefaultGroupName={#MyAppName}
AllowNoIcons=yes
OutputDir=..\beta-release
OutputBaseFilename=DantesInfernoInstaller
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=admin
DisableProgramGroupPage=yes
UsePreviousAppDir=yes
UninstallDisplayIcon={app}\{#MyAppExeName}
UninstallDisplayName={#MyAppName}
SetupIconFile=icon.ico
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "italian"; MessagesFile: "compiler:Languages\Italian.isl"
Name: "spanish"; MessagesFile: "compiler:Languages\Spanish.isl"
Name: "french"; MessagesFile: "compiler:Languages\French.isl"
Name: "brazilianportuguese"; MessagesFile: "compiler:Languages\BrazilianPortuguese.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
; Port binaries (from the C++ build)
Source: "..\out\build\win-amd64-release\dantes_inferno.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\out\build\win-amd64-release\dantes_inferno_native.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\out\build\win-amd64-release\rexruntime.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\out\build\win-amd64-release\rexgpu-xenos.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\out\build\win-amd64-release\amd_fidelityfx_dx12.dll"; DestDir: "{app}"; Flags: ignoreversion
#define FfxVkPath SourcePath + "\..\out\build\win-amd64-release\thirdparty\rexglue-sdk\out\win-amd64\amd_fidelityfx_vk.dll"
#if FileExists(FfxVkPath)
Source: "{#FfxVkPath}"; DestDir: "{app}"; Flags: ignoreversion
#endif

; Launcher
Source: "DantesInfernoLauncher\bin\Release\DantesInfernoLauncher.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "DantesInfernoLauncher\bin\Release\DantesInferno.Shared.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "DantesInfernoLauncher\bin\Release\version.txt"; DestDir: "{app}"; Flags: ignoreversion

; Assets
Source: "icon.ico"; DestDir: "{app}"; Flags: ignoreversion
Source: "banner.jpg"; DestDir: "{app}"; Flags: ignoreversion

; ISO extraction tool (bundled, used post-install)
Source: "..\tools\extract-xiso\extract-xiso.exe"; DestDir: "{tmp}"; Flags: deleteafterinstall

; Pre-generated shader caches — seeded into the user shader storage on first
; launch so the startup preload runs instead of mid-game shader compilation.
Source: "..\packaging\shader_cache\*"; DestDir: "{app}\shader_cache"; Flags: ignoreversion

; NOTE: Game data (game/) is NOT included — the user provides their own ISO
; and the post-install step extracts it.

; Title Update 2 (TU2) patch — enables playable DLC like Trials of Saint Lucia.
; The runtime automatically applies this patch when default.xexp is present
; alongside default.xex in the game folder.
#define TuPath SourcePath + "\..\out\build\win-amd64-release\default.xexp"
#if FileExists(TuPath)
Source: "{#TuPath}"; DestDir: "{app}\game"; Flags: ignoreversion
#endif

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; IconFilename: "{app}\icon.ico"
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{commondesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; IconFilename: "{app}\icon.ico"; Tasks: desktopicon

[Run]
; Extract game data from ISO during install (visible console so user sees progress)
Filename: "{tmp}\extract-xiso.exe"; Parameters: "-x -d ""{app}\game"" -s ""{code:GetIsoPath}"""; WorkingDir: "{app}"; StatusMsg: "Extracting game data from ISO (this may take a few minutes)..."; Flags: hidewizard; Check: NeedsExtraction

; Launch the launcher after install
Filename: "{app}\{#MyAppExeName}"; Description: "Launch Dante's Inferno Launcher"; Flags: postinstall nowait skipifsilent

[UninstallDelete]
Type: filesandordirs; Name: "{app}\game"
Type: filesandordirs; Name: "{app}\logs"
Type: files; Name: "{app}\dantes_inferno.toml"
Type: files; Name: "{app}\version.txt"

[Code]
var
  IsoPage: TInputFileWizardPage;
  IsoSizeMB: Int64;

{ Windows API to get file size (supports >4GB files) }
function GetFileSizeEx(hFile: THandle; var lpFileSize: Int64): Boolean;
  external 'GetFileSizeEx@kernel32.dll stdcall';
function CreateFileW(lpFileName: WideString; dwDesiredAccess: Cardinal; dwShareMode: Cardinal; lpSecurityAttributes: Cardinal; dwCreationDisposition: Cardinal; dwFlagsAndAttributes: Cardinal; hTemplateFile: Cardinal): THandle;
  external 'CreateFileW@kernel32.dll stdcall';
function CloseHandle(hObject: THandle): Boolean;
  external 'CloseHandle@kernel32.dll stdcall';

const
  GENERIC_READ = $80000000;
  FILE_SHARE_READ = 1;
  OPEN_EXISTING = 3;
  INVALID_HANDLE_VALUE = $FFFFFFFF;

function GetFileSizeMB(FileName: string): Int64;
var
  hFile: THandle;
  FileSize: Int64;
begin
  Result := 0;
  hFile := CreateFileW(FileName, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
  if hFile <> INVALID_HANDLE_VALUE then
  begin
    if GetFileSizeEx(hFile, FileSize) then
      Result := FileSize div (1024 * 1024);
    CloseHandle(hFile);
  end;
end;

function FormatMB(MB: Int64): string;
begin
  if MB >= 1024 then
    Result := FloatToStr(Round(MB / 1024 * 10) / 10) + ' GB'
  else
    Result := IntToStr(MB) + ' MB';
end;

procedure InitializeWizard;
begin
  { Create a custom page for ISO selection, shown after directory selection }
  IsoPage := CreateInputFilePage(wpSelectDir,
    'Select Game ISO',
    'Choose your Dante''s Inferno Xbox 360 ISO file.',
    'The installer will extract game data from this ISO into the game folder. ' +
    'If you already have game data extracted, you can skip this step.');
  IsoPage.Add('ISO file (*.iso)|*.iso|All files (*.*)|*.*', '.iso', '');
  IsoPage.Values[0] := '';
  IsoSizeMB := 0;
end;

function GetIsoPath(Param: string): string;
begin
  Result := IsoPage.Values[0];
end;

function NeedsExtraction: Boolean;
begin
  Result := (IsoPage.Values[0] <> '');
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  IsoFile: string;
begin
  Result := True;
  { When leaving the ISO selection page, validate and calculate size }
  if CurPageID = IsoPage.ID then
  begin
    IsoFile := IsoPage.Values[0];
    if IsoFile <> '' then
    begin
      if not FileExists(IsoFile) then
      begin
        MsgBox('The selected file does not exist:' + #13#10 + IsoFile, mbError, MB_OK);
        Result := False;
        Exit;
      end;
      IsoSizeMB := GetFileSizeMB(IsoFile);
    end
    else
      IsoSizeMB := 0;
  end;
end;

procedure CurPageChanged(CurPageID: Integer);
var
  BaseMB: Int64;
  TotalMB: Int64;
begin
  { When arriving at the "Ready to Install" page, update the disk space label }
  if CurPageID = wpReady then
  begin
    { Base install size from [Files] (approx 55 MB for port binaries + launcher) }
    BaseMB := 55;
    if IsoSizeMB > 0 then
    begin
      { ISO extraction roughly doubles the ISO size on disk (game data is
        uncompressed from the ISO container). Use 1.5x as a conservative estimate. }
      TotalMB := BaseMB + Round(IsoSizeMB * 1.5);
      WizardForm.DiskSpaceLabel.Caption :=
        'Required space (port + game data from ISO): ' + FormatMB(TotalMB) +
        ' (ISO: ' + FormatMB(IsoSizeMB) + ')';
    end
    else
    begin
      TotalMB := BaseMB;
      WizardForm.DiskSpaceLabel.Caption :=
        'Required space (port only, no ISO): ' + FormatMB(TotalMB) +
        ' — game data will not be extracted.';
    end;
  end;
end;

function UpdateReadyMemo(Space, NewLine, MemoUserInfoInfo, MemoDirInfo, MemoTypeInfo, MemoComponentsInfo, MemoGroupInfo, MemoTasksInfo: string): string;
begin
  Result := MemoDirInfo + NewLine + NewLine +
            'ISO file:' + NewLine + Space;
  if IsoPage.Values[0] <> '' then
  begin
    Result := Result + ExtractFileName(IsoPage.Values[0]);
    if IsoSizeMB > 0 then
      Result := Result + ' (' + FormatMB(IsoSizeMB) + ')';
  end
  else
    Result := Result + '(skipped - game data already extracted)';
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  ConfigPath: string;
  VersionFile: string;
begin
  if CurStep = ssPostInstall then
  begin
    { Write dantes_inferno.toml with sensible defaults }
    ConfigPath := ExpandConstant('{app}\dantes_inferno.toml');
    if not FileExists(ConfigPath) then
    begin
      SaveStringToFile(ConfigPath,
        'game_data_root = "' + ExpandConstant('{app}\game') + '"' + #13#10 +
        'render_target_path_d3d12 = "rov"' + #13#10 +
        'renderer = "native"' + #13#10 +
        'log_level = "off"' + #13#10 +
        'resolution_scale = 1' + #13#10 +
        'swap_post_effect = "none"' + #13#10 +
        'anisotropic_override = -1' + #13#10 +
        'vsync = true' + #13#10 +
        'fullscreen = true' + #13#10 +
        'input_backend = "sdl"' + #13#10 +
        'dlc_source_path = "' + ExpandConstant('{app}\dlc') + '"' + #13#10,
        False);
    end;

    { Always write version.txt so updates reflect the new version }
    VersionFile := ExpandConstant('{app}\version.txt');
    SaveStringToFile(VersionFile, '{#MyAppVersion}' + #13#10, False);
  end;
end;
