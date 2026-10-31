; Dante's Inferno installer (Inno Setup 6). Built by package-release.ps1.

#define MyAppName "Dante's Inferno"
#define MyAppPublisher "florinp93"
#define MyAppURL "https://github.com/florinp93/hells-gate-recomp"
#define MyAppExeName "DantesInfernoLauncher.exe"
#define MyAppVersion "0.8.5-beta"

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
; Updates (launcher "Download & Install") reuse the installed folder.
DisableDirPage=auto
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
; Port binaries; package-release.ps1 passes GameBuildDir.
#ifndef GameBuildDir
  #define GameBuildDir SourcePath + "\..\out\build\win-amd64-release"
#endif
Source: "{#GameBuildDir}\dantes_inferno.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#GameBuildDir}\rexruntime.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#GameBuildDir}\rexgpu-xenos.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#GameBuildDir}\amd_fidelityfx_dx12.dll"; DestDir: "{app}"; Flags: ignoreversion
#define FfxVkPath GameBuildDir + "\thirdparty\rexglue-sdk\out\win-amd64\amd_fidelityfx_vk.dll"
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

; The launcher imports the user's ISO into game\ on first run; no game data is shipped.
Source: "..\tools\extract-xiso\extract-xiso.exe"; DestDir: "{app}"; Flags: ignoreversion

; Title Update 2; the launcher copies tu\default.xexp into the game data folder.
#define TuPath GameBuildDir + "\default.xexp"
#if !FileExists(TuPath)
  #error default.xexp (TU2) is missing from GameBuildDir; the game cannot start without it.
#endif
Source: "{#TuPath}"; DestDir: "{app}\game"; Flags: ignoreversion
Source: "{#TuPath}"; DestDir: "{app}\tu"; Flags: ignoreversion

; Recorded shaders and pipelines (tools\pipeline_storage_merge.py), built at
; launch so effects don't compile mid-game.
#define ShaderListPath SourcePath + "\..\shader-list\dante_pipelines.bin"
#if FileExists(ShaderListPath)
Source: "{#ShaderListPath}"; DestDir: "{app}"; Flags: ignoreversion
#endif

[Dirs]
; The launcher (not elevated) saves dantes_inferno.toml here and the game
; writes its logs here, so users need write access under Program Files.
Name: "{app}"; Permissions: users-modify

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; IconFilename: "{app}\icon.ico"
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{commondesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; IconFilename: "{app}\icon.ico"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "Launch Dante's Inferno Launcher"; Flags: postinstall nowait skipifsilent

[InstallDelete]
; Left by earlier versions: the SDK Vulkan build and the D3D12 renderer's
; shader caches, unused by the native renderer.
Type: files; Name: "{app}\dantes_inferno_native.exe"
Type: filesandordirs; Name: "{app}\shader_cache"

[UninstallDelete]
Type: filesandordirs; Name: "{app}\game"
Type: filesandordirs; Name: "{app}\logs"
Type: files; Name: "{app}\dantes_inferno.toml"
Type: files; Name: "{app}\version.txt"
Type: files; Name: "{app}\extract-xiso.log"

[Code]
procedure CurStepChanged(CurStep: TSetupStep);
var
  ConfigPath: string;
  VersionFile: string;
begin
  if CurStep = ssPostInstall then
  begin
    { First install: default settings }
    ConfigPath := ExpandConstant('{app}\dantes_inferno.toml');
    if not FileExists(ConfigPath) then
    begin
      SaveStringToFile(ConfigPath,
        'game_data_root = "' + ExpandConstant('{app}\game') + '"' + #13#10 +
        'anisotropic_override = -1' + #13#10 +
        'fullscreen = true' + #13#10 +
        'input_backend = "sdl"' + #13#10 +
        'dlc_source_path = "' + ExpandConstant('{app}\dlc') + '"' + #13#10,
        False);
    end;

    { The launcher's update check reads version.txt }
    VersionFile := ExpandConstant('{app}\version.txt');
    SaveStringToFile(VersionFile, '{#MyAppVersion}' + #13#10, False);
  end;
end;
