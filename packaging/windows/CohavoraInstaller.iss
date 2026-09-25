; Cohavora Windows x64 installer. Build the Release application before ISCC.
; Paths supplied through /D are absolute; defaults are relative to this script.
#define MyAppName "Cohavora"
#define MyAppPublisher "Cohavora"
#define MyAppURL "https://github.com/a980374208/Cohavora"
#define MyAppExeName "Cohavora.exe"
; Keep this identity stable so Inno Setup can upgrade in place.
#define MyAppId "{8C8E19B2-9F1C-4B9E-B19E-2A4FD6BE55A1}"
#define RepoRoot AddBackslash(SourcePath) + "..\..\"

#ifndef AppBuildDir
  #define AppBuildDir RepoRoot + "out\build\windows-vs2026-release\src\app\Release"
#endif
#ifndef OutputDir
  #define OutputDir RepoRoot + "out\installer"
#endif
#ifndef ChineseMessagesFile
  #define ChineseMessagesFile CompilerPath + "Languages\ChineseSimplified.isl"
#endif
#ifndef PackageCompression
  #define PackageCompression "lzma2/max"
#endif

#define AppExe AddBackslash(AppBuildDir) + MyAppExeName
#if !FileExists(AppExe)
  #error "Cohavora.exe is missing. Build the Release preset or pass /DAppBuildDir=<absolute runtime directory>."
#endif
#if !FileExists(AddBackslash(AppBuildDir) + "renderers\cohavora-render-dx11.dll")
  #error "Missing renderers\cohavora-render-dx11.dll. Build the cohavora_app target including its renderer dependencies."
#endif
#if !FileExists(AddBackslash(AppBuildDir) + "renderers\cohavora-render-opengl.dll")
  #error "Missing renderers\cohavora-render-opengl.dll. Build the cohavora_app target including its renderer dependencies."
#endif
#if !FileExists(ChineseMessagesFile)
  #error "ChineseSimplified.isl is missing. Install the matching Inno Setup language file or pass /DChineseMessagesFile=<absolute .isl path>."
#endif
; The executable resource is generated from CMake PROJECT_VERSION.
#define MyAppVersion GetStringFileInfo(AppExe, "ProductVersion")
#if MyAppVersion == ""
  #error "Cohavora.exe has no ProductVersion resource. Rebuild the application before packaging."
#endif

[Setup]
AppId={{#MyAppId}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
VersionInfoVersion={#GetVersionNumbersString(AppExe)}
LicenseFile={#RepoRoot}LICENSE
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
OutputDir={#OutputDir}
OutputBaseFilename=Cohavora-{#MyAppVersion}-x64-Setup
SetupIconFile={#RepoRoot}src\ui\icons\cohavora.ico
UninstallDisplayIcon={app}\{#MyAppExeName}
Compression={#PackageCompression}
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
WizardStyle=modern
; Keep the existing all-users default; /CURRENTUSER selects a per-user install.
PrivilegesRequired=admin
PrivilegesRequiredOverridesAllowed=commandline dialog
; Restart Manager only considers processes using the installed files.
CloseApplications=yes
RestartApplications=no

[Languages]
Name: "chinesesimp"; MessagesFile: "{#ChineseMessagesFile}"
Name: "english"; MessagesFile: "compiler:Default.isl"

[CustomMessages]
chinesesimp.CreateDesktopIcon=创建桌面快捷方式(&D)
chinesesimp.AutoStartTask=登录 Windows 时启动 Cohavora
chinesesimp.AppRunningUninstallPrompt=Cohavora 的安装文件正在使用中。请关闭此安装目录中的应用及相关程序后，重新运行卸载程序。
chinesesimp.AppCheckFailed=无法检查安装文件是否正在使用。请关闭 Cohavora 后重试；详细原因见卸载日志。
chinesesimp.LaunchProgram=立即运行 Cohavora
english.CreateDesktopIcon=Create a &desktop shortcut
english.AutoStartTask=Start Cohavora when signing in to Windows
english.AppRunningUninstallPrompt=Cohavora installation files are in use. Close the application and any related programs using this installation, then run the uninstaller again.
english.AppCheckFailed=Unable to check whether installation files are in use. Close Cohavora and retry; see the uninstall log for details.
english.LaunchProgram=Launch Cohavora now

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"
Name: "autostart"; Description: "{cm:AutoStartTask}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
; Match cmake/CohavoraPackaging.cmake: Qt, CRT and other dependencies are static.
; Explicit files prevent stale DLLs, PDBs and other build outputs being shipped.
Source: "{#AppExe}"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#AppBuildDir}\renderers\cohavora-render-dx11.dll"; DestDir: "{app}\renderers"; Flags: ignoreversion
Source: "{#AppBuildDir}\renderers\cohavora-render-opengl.dll"; DestDir: "{app}\renderers"; Flags: ignoreversion
Source: "{#RepoRoot}src\ui\icons\cohavora.ico"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#RepoRoot}LICENSE"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"; IconFilename: "{app}\cohavora.ico"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"; IconFilename: "{app}\cohavora.ico"; Tasks: desktopicon

[Registry]
; HKA follows the installation scope (HKLM for all users, HKCU for current user).
Root: HKA; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "{#MyAppName}"; ValueData: """{app}\{#MyAppExeName}"""; Flags: uninsdeletevalue; Tasks: autostart
; Unchecking a remembered task during an upgrade must remove the previous value.
Root: HKA; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: none; ValueName: "{#MyAppName}"; Flags: deletevalue; Tasks: not autostart

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram}"; Flags: nowait postinstall skipifsilent runasoriginaluser

; User settings, credentials and meeting data are deliberately preserved on uninstall.

[Code]
const
  RmSuccess = 0;
  RmMoreData = 234;

// Query only the files owned by this installation. Never shut down processes here.
function RmStartSession(var SessionHandle: Cardinal; SessionFlags: Cardinal;
  SessionKey: String): Cardinal;
  external 'RmStartSession@rstrtmgr.dll stdcall delayload uninstallonly';
function RmRegisterResources(SessionHandle: Cardinal; FileCount: Cardinal;
  FileNames: TArrayOfString; ApplicationCount: Cardinal; Applications: UINT_PTR;
  ServiceCount: Cardinal; ServiceNames: UINT_PTR): Cardinal;
  external 'RmRegisterResources@rstrtmgr.dll stdcall delayload uninstallonly';
function RmGetList(SessionHandle: Cardinal; var ProcessInfoNeeded: Cardinal;
  var ProcessInfoCount: Cardinal; AffectedApplications: UINT_PTR;
  var RebootReasons: Cardinal): Cardinal;
  external 'RmGetList@rstrtmgr.dll stdcall delayload uninstallonly';
function RmEndSession(SessionHandle: Cardinal): Cardinal;
  external 'RmEndSession@rstrtmgr.dll stdcall delayload uninstallonly';

function CanRemoveApplication(var FilesInUse: Boolean): Boolean;
var
  SessionHandle, Code, Needed, Count, RebootReasons: Cardinal;
  SessionKey: String;
  Files: TArrayOfString;
begin
  Result := False;
  FilesInUse := False;
  SessionHandle := 0;
  SessionKey := StringOfChar(#0, 33);
  try
    Code := RmStartSession(SessionHandle, 0, SessionKey);
    if Code <> RmSuccess then begin
      Log(Format('RmStartSession failed: %d', [Code]));
      Exit;
    end;
    try
      SetArrayLength(Files, 3);
      Files[0] := ExpandConstant('{app}\{#MyAppExeName}');
      Files[1] := ExpandConstant('{app}\renderers\cohavora-render-dx11.dll');
      Files[2] := ExpandConstant('{app}\renderers\cohavora-render-opengl.dll');
      Code := RmRegisterResources(SessionHandle, GetArrayLength(Files), Files, 0, 0, 0, 0);
      if Code <> RmSuccess then begin
        Log(Format('RmRegisterResources failed: %d', [Code]));
        Exit;
      end;
      Needed := 0;
      Count := 0;
      RebootReasons := 0;
      Code := RmGetList(SessionHandle, Needed, Count, 0, RebootReasons);
      Result := (Code = RmSuccess) and (Needed = 0) and (Count = 0);
      FilesInUse := ((Code = RmSuccess) or (Code = RmMoreData)) and
        ((Needed > 0) or (Count > 0));
      if not Result then
        Log(Format('Uninstall blocked: Restart Manager code=%d, processes=%d', [Code, Needed]));
    finally
      Code := RmEndSession(SessionHandle);
      if Code <> RmSuccess then begin
        Result := False;
        Log(Format('RmEndSession failed: %d', [Code]));
      end;
    end;
  except
    Result := False;
    Log('Unable to check application usage: ' + GetExceptionMessage);
  end;
end;

function InitializeUninstall: Boolean;
var
  FilesInUse: Boolean;
  MessageText: String;
begin
  Result := CanRemoveApplication(FilesInUse);
  if (not Result) and (not UninstallSilent) then begin
    if FilesInUse then
      MessageText := CustomMessage('AppRunningUninstallPrompt')
    else
      MessageText := CustomMessage('AppCheckFailed');
    SuppressibleMsgBox(MessageText, mbError, MB_OK, IDOK);
  end;
end;
